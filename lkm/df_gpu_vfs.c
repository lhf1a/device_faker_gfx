// SPDX-License-Identifier: GPL-2.0-only
/*
 * df_gpu_vfs.ko - Zero-residency per-UID VFS redirection LKM for Device Faker GFX.
 *
 * Design:
 * - No /dev, /proc, or /sys control nodes (stealth prctl kprobe channel).
 * - Root-only configuration commands (PING, SET_REDIRECT, SET_UID_CFG, ...).
 * - Untargeted UIDs receive zero response on prctl probe and hit a single
 *   atomic fast-path check in getname_flags.
 * - Targeted UIDs have configured vendor graphics HAL paths transparently
 *   redirected in-place in getname_flags (struct filename::iname) to thin
 *   proxy .so files, while ".df_orig_" prefixed paths strip the prefix in-place
 *   so the proxy can dlopen the real underlying vendor driver cleanly inside
 *   Android's sphal linker namespace.
 * - Static tables + rwlock_t + copy_{from,to}_user_nofault => zero dynamic
 *   allocation and zero sleeping inside kprobe atomic context.
 */

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/rwlock.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#include "../include/df_gpu_abi.h"

#define DF_MAX_REDIRECT_RULES   16
#define DF_MAX_UID_RULES        128

struct df_redirect_slot {
    bool used;
    struct df_gpu_redirect_rule rule;
};

struct df_uid_slot {
    bool used;
    struct df_gpu_uid_cfg cfg;
};

static DEFINE_RWLOCK(df_state_lock);
static struct df_redirect_slot df_redirects[DF_MAX_REDIRECT_RULES];
static struct df_uid_slot df_uids[DF_MAX_UID_RULES];
static atomic_t df_uid_count = ATOMIC_INIT(0);
static atomic_t df_redirect_count = ATOMIC_INIT(0);

/* Per-CPU scratch buffers keep kprobe kernel stack usage tiny (<128B). */
static DEFINE_PER_CPU(struct df_gpu_redirect_rule, df_redir_scratch);
static DEFINE_PER_CPU(struct df_gpu_uid_cfg, df_uid_scratch);
static DEFINE_PER_CPU(struct df_gpu_self_cfg, df_self_scratch);

static bool df_is_root(void)
{
    return uid_eq(current_uid(), GLOBAL_ROOT_UID);
}

static void df_sanitize_str(char *buf, size_t size)
{
    if (size > 0)
        buf[size - 1] = '\0';
}

static struct df_uid_slot *df_find_uid_locked(u32 uid)
{
    int i;

    for (i = 0; i < DF_MAX_UID_RULES; i++) {
        if (df_uids[i].used && df_uids[i].cfg.uid == uid)
            return &df_uids[i];
    }
    return NULL;
}

static void df_fill_orig_paths_locked(struct df_gpu_self_cfg *out)
{
    int i;

    out->real_vulkan_path[0] = '\0';
    out->real_gles_path[0] = '\0';
    out->real_egl_path[0] = '\0';

    for (i = 0; i < DF_MAX_REDIRECT_RULES; i++) {
        if (!df_redirects[i].used)
            continue;
        switch (df_redirects[i].rule.kind) {
        case DF_GPU_KIND_VULKAN:
            if (!out->real_vulkan_path[0])
                strscpy(out->real_vulkan_path, df_redirects[i].rule.orig_path,
                        sizeof(out->real_vulkan_path));
            break;
        case DF_GPU_KIND_GLES:
            if (!out->real_gles_path[0])
                strscpy(out->real_gles_path, df_redirects[i].rule.orig_path,
                        sizeof(out->real_gles_path));
            break;
        case DF_GPU_KIND_EGL:
            if (!out->real_egl_path[0])
                strscpy(out->real_egl_path, df_redirects[i].rule.orig_path,
                        sizeof(out->real_egl_path));
            break;
        default:
            break;
        }
    }
}

static void df_write_reply(unsigned long reply_uaddr, u32 val)
{
    if (!reply_uaddr)
        return;
    (void)copy_to_user_nofault((void __user *)reply_uaddr, &val, sizeof(val));
}

static void df_handle_admin_cmd(u32 cmd, unsigned long arg_uaddr,
                                unsigned long arg_len,
                                unsigned long reply_uaddr)
{
    unsigned long flags;
    int i;

    switch (cmd) {
    case DF_GPU_CMD_PING:
        if (arg_uaddr && arg_len >= sizeof(u32)) {
            u32 ver = DF_GPU_ABI_VERSION;
            (void)copy_to_user_nofault((void __user *)arg_uaddr, &ver, sizeof(ver));
        }
        df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
        return;

    case DF_GPU_CMD_CLEAR_REDIRECTS:
        write_lock_irqsave(&df_state_lock, flags);
        memset(df_redirects, 0, sizeof(df_redirects));
        atomic_set(&df_redirect_count, 0);
        write_unlock_irqrestore(&df_state_lock, flags);
        df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
        return;

    case DF_GPU_CMD_SET_REDIRECT: {
        struct df_gpu_redirect_rule *rule;
        int free_idx = -1;
        int match_idx = -1;

        if (!arg_uaddr || arg_len != sizeof(*rule))
            return;
        rule = get_cpu_ptr(&df_redir_scratch);
        if (copy_from_user_nofault(rule, (const void __user *)arg_uaddr, sizeof(*rule))) {
            put_cpu_ptr(&df_redir_scratch);
            return;
        }
        df_sanitize_str(rule->src_path, sizeof(rule->src_path));
        df_sanitize_str(rule->dst_path, sizeof(rule->dst_path));
        df_sanitize_str(rule->orig_path, sizeof(rule->orig_path));
        if (!rule->src_path[0] || !rule->dst_path[0]) {
            put_cpu_ptr(&df_redir_scratch);
            return;
        }

        write_lock_irqsave(&df_state_lock, flags);
        for (i = 0; i < DF_MAX_REDIRECT_RULES; i++) {
            if (df_redirects[i].used) {
                if (strcmp(df_redirects[i].rule.src_path, rule->src_path) == 0) {
                    match_idx = i;
                    break;
                }
            } else if (free_idx < 0) {
                free_idx = i;
            }
        }
        if (match_idx >= 0) {
            df_redirects[match_idx].rule = *rule;
            write_unlock_irqrestore(&df_state_lock, flags);
            put_cpu_ptr(&df_redir_scratch);
            df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
            return;
        }
        if (free_idx >= 0) {
            df_redirects[free_idx].used = true;
            df_redirects[free_idx].rule = *rule;
            atomic_inc(&df_redirect_count);
            write_unlock_irqrestore(&df_state_lock, flags);
            put_cpu_ptr(&df_redir_scratch);
            df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
            return;
        }
        write_unlock_irqrestore(&df_state_lock, flags);
        put_cpu_ptr(&df_redir_scratch);
        return;
    }

    case DF_GPU_CMD_CLEAR_UIDS:
        write_lock_irqsave(&df_state_lock, flags);
        memset(df_uids, 0, sizeof(df_uids));
        atomic_set(&df_uid_count, 0);
        write_unlock_irqrestore(&df_state_lock, flags);
        df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
        return;

    case DF_GPU_CMD_SET_UID_CFG: {
        struct df_gpu_uid_cfg *cfg;
        struct df_uid_slot *slot;
        int free_idx = -1;

        if (!arg_uaddr || arg_len != sizeof(*cfg))
            return;
        cfg = get_cpu_ptr(&df_uid_scratch);
        if (copy_from_user_nofault(cfg, (const void __user *)arg_uaddr, sizeof(*cfg))) {
            put_cpu_ptr(&df_uid_scratch);
            return;
        }
        df_sanitize_str(cfg->renderer, sizeof(cfg->renderer));
        df_sanitize_str(cfg->vendor, sizeof(cfg->vendor));
        df_sanitize_str(cfg->gl_version, sizeof(cfg->gl_version));

        write_lock_irqsave(&df_state_lock, flags);
        slot = df_find_uid_locked(cfg->uid);
        if (slot) {
            slot->cfg = *cfg;
            write_unlock_irqrestore(&df_state_lock, flags);
            put_cpu_ptr(&df_uid_scratch);
            df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
            return;
        }
        for (i = 0; i < DF_MAX_UID_RULES; i++) {
            if (!df_uids[i].used) {
                free_idx = i;
                break;
            }
        }
        if (free_idx >= 0) {
            df_uids[free_idx].used = true;
            df_uids[free_idx].cfg = *cfg;
            atomic_inc(&df_uid_count);
            write_unlock_irqrestore(&df_state_lock, flags);
            put_cpu_ptr(&df_uid_scratch);
            df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
            return;
        }
        write_unlock_irqrestore(&df_state_lock, flags);
        put_cpu_ptr(&df_uid_scratch);
        return;
    }

    case DF_GPU_CMD_DEL_UID_CFG: {
        u32 target_uid = 0;
        struct df_uid_slot *slot;

        if (!arg_uaddr || arg_len != sizeof(target_uid))
            return;
        if (copy_from_user_nofault(&target_uid, (const void __user *)arg_uaddr,
                                   sizeof(target_uid)))
            return;

        write_lock_irqsave(&df_state_lock, flags);
        slot = df_find_uid_locked(target_uid);
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            atomic_dec(&df_uid_count);
        }
        write_unlock_irqrestore(&df_state_lock, flags);
        df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
        return;
    }

    default:
        return;
    }
}

static void df_handle_get_self_cfg(unsigned long arg_uaddr,
                                   unsigned long arg_len,
                                   unsigned long reply_uaddr)
{
    struct df_gpu_self_cfg *out;
    struct df_uid_slot *slot;
    unsigned long flags;
    u32 uid;
    long copy_rc;

    if (!arg_uaddr || arg_len != sizeof(*out))
        return;
    if (atomic_read(&df_uid_count) == 0)
        return;

    uid = from_kuid_munged(current_user_ns(), current_uid());
    out = get_cpu_ptr(&df_self_scratch);

    read_lock_irqsave(&df_state_lock, flags);
    slot = df_find_uid_locked(uid);
    if (!slot) {
        read_unlock_irqrestore(&df_state_lock, flags);
        put_cpu_ptr(&df_self_scratch);
        /* Stay completely silent for untargeted UIDs */
        return;
    }

    memset(out, 0, sizeof(*out));
    out->abi_version = DF_GPU_ABI_VERSION;
    out->flags = slot->cfg.flags;
    out->vendor_id = slot->cfg.vendor_id;
    out->device_id = slot->cfg.device_id;
    out->driver_version = slot->cfg.driver_version;
    strscpy(out->renderer, slot->cfg.renderer, sizeof(out->renderer));
    strscpy(out->vendor, slot->cfg.vendor, sizeof(out->vendor));
    strscpy(out->gl_version, slot->cfg.gl_version, sizeof(out->gl_version));
    df_fill_orig_paths_locked(out);
    read_unlock_irqrestore(&df_state_lock, flags);

    copy_rc = copy_to_user_nofault((void __user *)arg_uaddr, out, sizeof(*out));
    put_cpu_ptr(&df_self_scratch);

    if (copy_rc == 0)
        df_write_reply(reply_uaddr, DF_GPU_REPLY_OK);
}

static int df_prctl_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
#if defined(CONFIG_ARM64)
    int option = (int)regs->regs[0];
    u32 cmd = (u32)regs->regs[1];
    unsigned long arg3 = regs->regs[2];
    unsigned long arg4 = regs->regs[3];
    unsigned long arg5 = regs->regs[4];
#elif defined(CONFIG_X86_64)
    int option = (int)regs->di;
    u32 cmd = (u32)regs->si;
    unsigned long arg3 = regs->dx;
    unsigned long arg4 = regs->cx;
    unsigned long arg5 = regs->r8;
#else
    return 0;
#endif

    if (likely(option != DF_GPU_PRCTL_MAGIC))
        return 0;

    if (cmd == DF_GPU_CMD_GET_SELF_CFG) {
        df_handle_get_self_cfg(arg3, arg4, arg5);
        return 0;
    }

    if (df_is_root())
        df_handle_admin_cmd(cmd, arg3, arg4, arg5);

    return 0;
}

static int df_getname_ret_handler(struct kretprobe_instance *ri,
                                  struct pt_regs *regs)
{
    struct filename *fn;
    const char *slash;
    const char *base;
    unsigned long flags;
    u32 uid;
    int i;

    if (likely(atomic_read(&df_uid_count) == 0))
        return 0;
    if (likely(atomic_read(&df_redirect_count) == 0))
        return 0;

    fn = (struct filename *)regs_return_value(regs);
    if (IS_ERR_OR_NULL(fn) || !fn->name || fn->name[0] != '/')
        return 0;

    uid = from_kuid_munged(current_user_ns(), current_uid());

    read_lock_irqsave(&df_state_lock, flags);
    if (!df_find_uid_locked(uid)) {
        read_unlock_irqrestore(&df_state_lock, flags);
        return 0;
    }

    /*
     * 1. Bypass prefix ".df_orig_": strip in-place so the proxy .so can dlopen
     *    the real underlying vendor driver inside Android's sphal namespace
     *    without colliding with Bionic's loaded-library path/inode cache.
     */
    slash = strrchr(fn->name, '/');
    base = slash ? slash + 1 : fn->name;
    if (strncmp(base, DF_GPU_ORIG_PREFIX, DF_GPU_ORIG_PREFIX_LEN) == 0) {
        size_t tail_len = strlen(base + DF_GPU_ORIG_PREFIX_LEN);
        char *dst = (char *)base;
        memmove(dst, base + DF_GPU_ORIG_PREFIX_LEN, tail_len + 1);
        read_unlock_irqrestore(&df_state_lock, flags);
        return 0;
    }

    /*
     * 2. Redirect registered vendor HAL/driver paths to thin proxy .so files.
     *    Only rewrite in-place when fn->name points to embedded fn->iname
     *    (guaranteed >= PATH_MAX - offsetof(struct filename, iname) bytes).
     */
    if (fn->name == fn->iname) {
        for (i = 0; i < DF_MAX_REDIRECT_RULES; i++) {
            if (!df_redirects[i].used)
                continue;
            if (strcmp(fn->name, df_redirects[i].rule.src_path) == 0) {
                size_t cap = PATH_MAX - offsetof(struct filename, iname);
                strscpy((char *)fn->iname, df_redirects[i].rule.dst_path, cap);
                break;
            }
        }
    }

    read_unlock_irqrestore(&df_state_lock, flags);
    return 0;
}

static struct kprobe df_prctl_kp = {
    .symbol_name = "security_task_prctl",
    .pre_handler = df_prctl_pre_handler,
};

static struct kretprobe df_getname_krp = {
    .kp.symbol_name = "getname_flags",
    .handler = df_getname_ret_handler,
    .maxactive = 64,
};

static int __init df_gpu_vfs_init(void)
{
    int ret;

    ret = register_kprobe(&df_prctl_kp);
    if (ret) {
        pr_err("df_gpu_vfs: register_kprobe(security_task_prctl) failed: %d\n", ret);
        return ret;
    }

    ret = register_kretprobe(&df_getname_krp);
    if (ret) {
        pr_err("df_gpu_vfs: register_kretprobe(getname_flags) failed: %d\n", ret);
        unregister_kprobe(&df_prctl_kp);
        return ret;
    }

    pr_info("df_gpu_vfs: loaded (abi=%u)\n", DF_GPU_ABI_VERSION);
    return 0;
}

static void __exit df_gpu_vfs_exit(void)
{
    unregister_kretprobe(&df_getname_krp);
    unregister_kprobe(&df_prctl_kp);
    pr_info("df_gpu_vfs: unloaded\n");
}

module_init(df_gpu_vfs_init);
module_exit(df_gpu_vfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lhf1a");
MODULE_DESCRIPTION("Device Faker GFX zero-residency per-UID VFS GPU proxy redirector");
MODULE_VERSION("1.0.0");
