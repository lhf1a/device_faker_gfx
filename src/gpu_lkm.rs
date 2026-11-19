//! LKM (`df_gpu_vfs.ko`) 零驻留 GPU 伪装控制端（运行在 companion root 进程内）。
//!
//! 通过隐藏 `prctl(DF_GPU_PRCTL_MAGIC, ...)` 通道与内核模块通信：
//! - 自动探测当前设备的 `/vendor/lib64/hw/vulkan.*.so` 与 `/vendor/lib64/egl/lib{EGL,GLESv2}_*.so`
//!   并将目标 UID 的访问按进程透明重定向到薄代理 `.so`；
//! - Zygisk 模块侧只下发 UID 规则并可立即 `DlCloseModuleLibrary`，实现零驻留、零 GOT/PLT 修改。

use std::path::Path;
use std::sync::Mutex;

use log::{info, warn};

use crate::config::ResolvedGpuConfig;

pub const DF_GPU_ABI_VERSION: u32 = 1;
pub const DF_GPU_PRCTL_MAGIC: i32 = 0x5F44_4647; // "_DFG"
pub const DF_GPU_REPLY_OK: u32 = 0x4446_4F4B; // "DFOK"

const DF_GPU_CMD_PING: u32 = 1;
const DF_GPU_CMD_CLEAR_REDIRECTS: u32 = 2;
const DF_GPU_CMD_SET_REDIRECT: u32 = 3;
const DF_GPU_CMD_CLEAR_UIDS: u32 = 4;
const DF_GPU_CMD_SET_UID_CFG: u32 = 5;
const DF_GPU_CMD_DEL_UID_CFG: u32 = 6;

const DF_GPU_MAX_STR: usize = 256;
const DF_GPU_MAX_PATH: usize = 256;
const DF_GPU_ORIG_PREFIX: &str = ".df_orig_";

const DF_GPU_FLAG_HAS_RENDERER: u32 = 1 << 0;
const DF_GPU_FLAG_HAS_VENDOR: u32 = 1 << 1;
const DF_GPU_FLAG_HAS_GL_VERSION: u32 = 1 << 2;
const DF_GPU_FLAG_HAS_VENDOR_ID: u32 = 1 << 3;
const DF_GPU_FLAG_HAS_DEVICE_ID: u32 = 1 << 4;
const DF_GPU_FLAG_HAS_DRIVER_VERSION: u32 = 1 << 5;
const DF_GPU_FLAG_DEBUG_LOG: u32 = 1 << 6;

const DF_GPU_KIND_VULKAN: u32 = 1;
const DF_GPU_KIND_GLES: u32 = 2;
const DF_GPU_KIND_EGL: u32 = 3;

const VULKAN_PROXY_PATH: &str = "/vendor/lib64/hw/vulkan.df_proxy.so";
const GLES_PROXY_PATH: &str = "/vendor/lib64/egl/libGLESv2_df_proxy.so";
const EGL_PROXY_PATH: &str = "/vendor/lib64/egl/libEGL_df_proxy.so";

#[repr(C)]
#[derive(Clone, Copy)]
struct DfGpuRedirectRule {
    kind: u32,
    reserved: u32,
    src_path: [u8; DF_GPU_MAX_PATH],
    dst_path: [u8; DF_GPU_MAX_PATH],
    orig_path: [u8; DF_GPU_MAX_PATH],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DfGpuUidCfg {
    uid: u32,
    flags: u32,
    vendor_id: u32,
    device_id: u32,
    driver_version: u32,
    reserved: u32,
    renderer: [u8; DF_GPU_MAX_STR],
    vendor: [u8; DF_GPU_MAX_STR],
    gl_version: [u8; DF_GPU_MAX_STR],
}

const _: () = assert!(std::mem::size_of::<DfGpuRedirectRule>() == 8 + 3 * DF_GPU_MAX_PATH);
const _: () = assert!(std::mem::size_of::<DfGpuUidCfg>() == 24 + 3 * DF_GPU_MAX_STR);

static REDIRECTS_READY: Mutex<bool> = Mutex::new(false);

fn write_cstr_buf<const N: usize>(dst: &mut [u8; N], src: &str) {
    dst.fill(0);
    let bytes = src.as_bytes();
    let copy_len = bytes.len().min(N - 1);
    dst[..copy_len].copy_from_slice(&bytes[..copy_len]);
}

#[cfg(any(target_os = "android", target_os = "linux"))]
fn send_prctl_cmd(cmd: u32, arg_ptr: usize, arg_len: usize) -> bool {
    let mut reply: u32 = 0;
    for _ in 0..2 {
        // 预先触碰栈页，确保内核 copy_to_user_nofault 不会触发缺页失败
        unsafe { std::ptr::write_volatile(&mut reply, 0) };
        unsafe {
            libc::prctl(
                DF_GPU_PRCTL_MAGIC,
                cmd as libc::c_ulong,
                arg_ptr as libc::c_ulong,
                arg_len as libc::c_ulong,
                (&mut reply as *mut u32) as libc::c_ulong,
            );
        }
        if unsafe { std::ptr::read_volatile(&reply) } == DF_GPU_REPLY_OK {
            return true;
        }
    }
    false
}

#[cfg(not(any(target_os = "android", target_os = "linux")))]
fn send_prctl_cmd(_cmd: u32, _arg_ptr: usize, _arg_len: usize) -> bool {
    false
}

/// 探测 `df_gpu_vfs.ko` 是否已加载且 ABI 版本兼容。
pub fn ping_lkm() -> bool {
    let mut ver: u32 = 0;
    if !send_prctl_cmd(
        DF_GPU_CMD_PING,
        (&mut ver as *mut u32) as usize,
        std::mem::size_of::<u32>(),
    ) {
        return false;
    }
    ver == DF_GPU_ABI_VERSION
}

fn set_redirect_rule(kind: u32, src_path: &str, dst_path: &str, orig_path: &str) -> bool {
    let mut rule = DfGpuRedirectRule {
        kind,
        reserved: 0,
        src_path: [0; DF_GPU_MAX_PATH],
        dst_path: [0; DF_GPU_MAX_PATH],
        orig_path: [0; DF_GPU_MAX_PATH],
    };
    write_cstr_buf(&mut rule.src_path, src_path);
    write_cstr_buf(&mut rule.dst_path, dst_path);
    write_cstr_buf(&mut rule.orig_path, orig_path);
    send_prctl_cmd(
        DF_GPU_CMD_SET_REDIRECT,
        (&rule as *const DfGpuRedirectRule) as usize,
        std::mem::size_of::<DfGpuRedirectRule>(),
    )
}

fn read_sys_prop(key: &str) -> Option<String> {
    #[cfg(target_os = "android")]
    {
        prop_rs_android::sys_prop::get(key)
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = key;
        None
    }
}

fn make_orig_path(src_path: &str) -> Option<String> {
    let p = Path::new(src_path);
    let parent = p.parent()?.to_str()?;
    let file_name = p.file_name()?.to_str()?;
    Some(format!("{parent}/{DF_GPU_ORIG_PREFIX}{file_name}"))
}

fn discover_vulkan_driver() -> Option<String> {
    if let Some(hw) = read_sys_prop("ro.hardware.vulkan") {
        let cand = format!("/vendor/lib64/hw/vulkan.{hw}.so");
        if Path::new(&cand).is_file() {
            return Some(cand);
        }
    }
    let dir = std::fs::read_dir("/vendor/lib64/hw").ok()?;
    let mut candidates: Vec<String> = dir
        .filter_map(|e| e.ok())
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|name| {
            name.starts_with("vulkan.")
                && name.ends_with(".so")
                && name != "vulkan.df_proxy.so"
                && !name.starts_with(DF_GPU_ORIG_PREFIX)
                && name != "vulkan.turnip.so"
                && name != "vulkan.qti.so"
                && name != "vulkan.qti-policy.so"
                && name != "vulkan.qti-native.so"
        })
        .map(|name| format!("/vendor/lib64/hw/{name}"))
        .collect();
    candidates.sort();
    candidates.into_iter().next()
}

fn discover_egl_tag() -> Option<String> {
    if let Some(hw) = read_sys_prop("ro.hardware.egl") {
        let egl = format!("/vendor/lib64/egl/libEGL_{hw}.so");
        let gles = format!("/vendor/lib64/egl/libGLESv2_{hw}.so");
        if Path::new(&egl).is_file() || Path::new(&gles).is_file() {
            return Some(hw);
        }
    }
    let dir = std::fs::read_dir("/vendor/lib64/egl").ok()?;
    let mut tags: Vec<String> = dir
        .filter_map(|e| e.ok())
        .filter_map(|e| e.file_name().into_string().ok())
        .filter_map(|name| {
            let tag = name
                .strip_prefix("libGLESv2_")
                .and_then(|rest| rest.strip_suffix(".so"))?;
            if tag == "df_proxy" || tag == "angle" || tag == "swiftshader" {
                return None;
            }
            Some(tag.to_string())
        })
        .collect();
    tags.sort();
    tags.into_iter().next()
}

/// 检查 `libEGL_<tag>.so` 是否直接 `DT_NEEDED` 了 `libGLESv2_<tag>.so`（如 ANGLE）。
/// 若存在直接依赖，跳过 `libGLESv2_<tag>.so` 重定向以免遮蔽厂商私有内部符号，
/// 仅重定向 `libEGL_<tag>.so`。
fn elf_references_soname(elf_path: &str, needle_soname: &str) -> bool {
    let Ok(bytes) = std::fs::read(elf_path) else {
        return false;
    };
    let mut pat = needle_soname.as_bytes().to_vec();
    pat.push(0);
    bytes.windows(pat.len()).any(|w| w == pat.as_slice())
}

fn ensure_redirects_configured() -> bool {
    let mut guard = REDIRECTS_READY
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if *guard {
        return true;
    }

    let _ = send_prctl_cmd(DF_GPU_CMD_CLEAR_REDIRECTS, 0, 0);
    let mut registered = 0usize;

    if Path::new(VULKAN_PROXY_PATH).is_file()
        && let Some(vk_src) = discover_vulkan_driver()
        && let Some(vk_orig) = make_orig_path(&vk_src)
        && set_redirect_rule(DF_GPU_KIND_VULKAN, &vk_src, VULKAN_PROXY_PATH, &vk_orig)
    {
        info!("LKM redirect registered: Vulkan {vk_src} -> {VULKAN_PROXY_PATH}");
        registered += 1;
    }

    if let Some(tag) = discover_egl_tag() {
        let egl_src = format!("/vendor/lib64/egl/libEGL_{tag}.so");
        let gles_src = format!("/vendor/lib64/egl/libGLESv2_{tag}.so");
        let gles_soname = format!("libGLESv2_{tag}.so");

        let egl_needs_gles =
            Path::new(&egl_src).is_file() && elf_references_soname(&egl_src, &gles_soname);

        if Path::new(EGL_PROXY_PATH).is_file()
            && Path::new(&egl_src).is_file()
            && let Some(egl_orig) = make_orig_path(&egl_src)
            && set_redirect_rule(DF_GPU_KIND_EGL, &egl_src, EGL_PROXY_PATH, &egl_orig)
        {
            info!("LKM redirect registered: EGL {egl_src} -> {EGL_PROXY_PATH}");
            registered += 1;
        }

        if !egl_needs_gles
            && Path::new(GLES_PROXY_PATH).is_file()
            && Path::new(&gles_src).is_file()
            && let Some(gles_orig) = make_orig_path(&gles_src)
            && set_redirect_rule(DF_GPU_KIND_GLES, &gles_src, GLES_PROXY_PATH, &gles_orig)
        {
            info!("LKM redirect registered: GLESv2 {gles_src} -> {GLES_PROXY_PATH}");
            registered += 1;
        } else if egl_needs_gles {
            warn!(
                "LKM skipped GLESv2 redirect for {gles_src}: {egl_src} directly references {gles_soname}"
            );
        }
    }

    if registered > 0 {
        *guard = true;
        true
    } else {
        warn!("df_gpu_vfs.ko is loaded, but no proxy .so + vendor driver redirect pair was staged");
        false
    }
}

/// 将指定 UID 的 GPU 伪装参数同步到 `df_gpu_vfs.ko`；返回 `true` 表示 LKM 零驻留路径已就绪。
pub fn sync_uid_gpu_cfg(uid: u32, cfg: &ResolvedGpuConfig, debug: bool) -> bool {
    if !ping_lkm() {
        return false;
    }
    if !ensure_redirects_configured() {
        return false;
    }

    let mut flags = 0u32;
    let mut raw = DfGpuUidCfg {
        uid,
        flags: 0,
        vendor_id: 0,
        device_id: 0,
        driver_version: 0,
        reserved: 0,
        renderer: [0; DF_GPU_MAX_STR],
        vendor: [0; DF_GPU_MAX_STR],
        gl_version: [0; DF_GPU_MAX_STR],
    };

    if let Some(r) = &cfg.renderer {
        write_cstr_buf(&mut raw.renderer, r);
        flags |= DF_GPU_FLAG_HAS_RENDERER;
    }
    if let Some(v) = &cfg.vendor {
        write_cstr_buf(&mut raw.vendor, v);
        flags |= DF_GPU_FLAG_HAS_VENDOR;
    }
    if let Some(gv) = &cfg.gl_version {
        write_cstr_buf(&mut raw.gl_version, gv);
        flags |= DF_GPU_FLAG_HAS_GL_VERSION;
    }
    if let Some(vid) = cfg.vendor_id {
        raw.vendor_id = vid;
        flags |= DF_GPU_FLAG_HAS_VENDOR_ID;
    }
    if let Some(did) = cfg.device_id {
        raw.device_id = did;
        flags |= DF_GPU_FLAG_HAS_DEVICE_ID;
    }
    if let Some(dv) = cfg.driver_version {
        raw.driver_version = dv;
        flags |= DF_GPU_FLAG_HAS_DRIVER_VERSION;
    }
    if debug {
        flags |= DF_GPU_FLAG_DEBUG_LOG;
    }
    raw.flags = flags;

    let ok = send_prctl_cmd(
        DF_GPU_CMD_SET_UID_CFG,
        (&raw as *const DfGpuUidCfg) as usize,
        std::mem::size_of::<DfGpuUidCfg>(),
    );
    if ok && debug {
        info!(
            "LKM synced GPU cfg for uid={uid} (renderer={:?}, vendor={:?}, vendor_id={:?})",
            cfg.renderer,
            cfg.vendor,
            cfg.vendor_id.map(|id| format!("0x{id:04X}"))
        );
    }
    ok
}

/// 删除单个 UID 的内核规则（应用取消 GPU 伪装时）。
#[allow(dead_code)]
pub fn del_uid_gpu_cfg(uid: u32) {
    if !ping_lkm() {
        return;
    }
    let _ = send_prctl_cmd(
        DF_GPU_CMD_DEL_UID_CFG,
        (&uid as *const u32) as usize,
        std::mem::size_of::<u32>(),
    );
}

/// 配置文件热重载时清空内核 UID 表，避免已删除的包继续走代理重定向。
pub fn clear_all_uid_cfgs() {
    if !ping_lkm() {
        return;
    }
    let _ = send_prctl_cmd(DF_GPU_CMD_CLEAR_UIDS, 0, 0);
}
