/*
 * Shared ABI between df_gpu_vfs.ko (LKM), user-space graphics proxies
 * (vulkan/GLES/EGL), and device_faker_gfx companion (Rust).
 *
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 */
#ifndef DF_GPU_ABI_H
#define DF_GPU_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 df_u32;
#else
#include <stdint.h>
typedef uint32_t df_u32;
#endif

#define DF_GPU_ABI_VERSION          1u
#define DF_GPU_PRCTL_MAGIC          0x5F444647 /* "_DFG" */
#define DF_GPU_REPLY_OK             0x44464F4Bu /* "DFOK" */

/* Control commands passed as prctl arg2 */
#define DF_GPU_CMD_PING             1u /* root only */
#define DF_GPU_CMD_CLEAR_REDIRECTS  2u /* root only */
#define DF_GPU_CMD_SET_REDIRECT     3u /* root only: struct df_gpu_redirect_rule */
#define DF_GPU_CMD_CLEAR_UIDS       4u /* root only */
#define DF_GPU_CMD_SET_UID_CFG      5u /* root only: struct df_gpu_uid_cfg */
#define DF_GPU_CMD_DEL_UID_CFG      6u /* root only: df_u32 uid */
#define DF_GPU_CMD_GET_SELF_CFG     7u /* targeted uid only: struct df_gpu_self_cfg */

#define DF_GPU_MAX_STR              256
#define DF_GPU_MAX_PATH             256
#define DF_GPU_ORIG_PREFIX          ".df_orig_"
#define DF_GPU_ORIG_PREFIX_LEN      9

/* Bitflags for df_gpu_uid_cfg.flags / df_gpu_self_cfg.flags */
#define DF_GPU_FLAG_HAS_RENDERER        (1u << 0)
#define DF_GPU_FLAG_HAS_VENDOR          (1u << 1)
#define DF_GPU_FLAG_HAS_GL_VERSION      (1u << 2)
#define DF_GPU_FLAG_HAS_VENDOR_ID       (1u << 3)
#define DF_GPU_FLAG_HAS_DEVICE_ID       (1u << 4)
#define DF_GPU_FLAG_HAS_DRIVER_VERSION  (1u << 5)
#define DF_GPU_FLAG_DEBUG_LOG           (1u << 6)

/* Redirect rule kind */
#define DF_GPU_KIND_VULKAN          1u
#define DF_GPU_KIND_GLES            2u
#define DF_GPU_KIND_EGL             3u
#define DF_GPU_KIND_SYSFS           4u

struct df_gpu_redirect_rule {
    df_u32 kind;
    df_u32 reserved;
    char src_path[DF_GPU_MAX_PATH];
    char dst_path[DF_GPU_MAX_PATH];
    char orig_path[DF_GPU_MAX_PATH];
};

struct df_gpu_uid_cfg {
    df_u32 uid;
    df_u32 flags;
    df_u32 vendor_id;
    df_u32 device_id;
    df_u32 driver_version;
    df_u32 reserved;
    char renderer[DF_GPU_MAX_STR];
    char vendor[DF_GPU_MAX_STR];
    char gl_version[DF_GPU_MAX_STR];
};

struct df_gpu_self_cfg {
    df_u32 abi_version;
    df_u32 flags;
    df_u32 vendor_id;
    df_u32 device_id;
    df_u32 driver_version;
    df_u32 reserved;
    char renderer[DF_GPU_MAX_STR];
    char vendor[DF_GPU_MAX_STR];
    char gl_version[DF_GPU_MAX_STR];
    char real_vulkan_path[DF_GPU_MAX_PATH];
    char real_gles_path[DF_GPU_MAX_PATH];
    char real_egl_path[DF_GPU_MAX_PATH];
};

#ifndef __KERNEL__
_Static_assert(sizeof(struct df_gpu_redirect_rule) == 8 + 3 * DF_GPU_MAX_PATH,
               "df_gpu_redirect_rule layout mismatch");
_Static_assert(sizeof(struct df_gpu_uid_cfg) == 24 + 3 * DF_GPU_MAX_STR,
               "df_gpu_uid_cfg layout mismatch");
_Static_assert(sizeof(struct df_gpu_self_cfg) == 24 + 3 * DF_GPU_MAX_STR + 3 * DF_GPU_MAX_PATH,
               "df_gpu_self_cfg layout mismatch");
#endif

#endif /* DF_GPU_ABI_H */
