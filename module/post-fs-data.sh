#!/system/bin/sh
# Device Faker GFX post-fs-data hook:
# 1. Label thin user-space graphics proxy libraries as same_process_hal_file for sphal/SELinux.
# 2. If KernelSU has no metamodule mounting vendor overlays, stage proxy libraries directly
#    into writable /vendor/lib64/{hw,egl} overlays (e.g. SQ2 hybrid).
# 3. Load df_gpu_vfs.ko (per-UID VFS redirector) if available for the running kernel.

MODDIR="${0%/*}"

stage_proxy() {
    rel="$1"
    dst="/vendor/lib64/$rel"
    src=""
    for cand in "$MODDIR/vendor/lib64/$rel" "$MODDIR/system/vendor/lib64/$rel"; do
        if [ -f "$cand" ]; then
            src="$cand"
            chmod 0644 "$cand" 2>/dev/null
            chcon u:object_r:same_process_hal_file:s0 "$cand" 2>/dev/null
            break
        fi
    done
    [ -n "$src" ] || return 0
    if [ ! -f "$dst" ] || ! cmp -s "$src" "$dst" 2>/dev/null; then
        if cp -f "$src" "$dst" 2>/dev/null; then
            chmod 0644 "$dst" 2>/dev/null
            chcon u:object_r:same_process_hal_file:s0 "$dst" 2>/dev/null
        fi
    fi
}

stage_proxy "hw/vulkan.df_proxy.so"
stage_proxy "egl/libEGL_df_proxy.so"
stage_proxy "egl/libGLESv2_df_proxy.so"

if [ ! -d /sys/module/df_gpu_vfs ]; then
    for ko in \
        "/data/adb/device_faker/lkm/df_gpu_vfs.ko" \
        "$MODDIR/lkm/df_gpu_vfs.ko"
    do
        if [ -f "$ko" ]; then
            insmod "$ko" 2>/dev/null && break
        fi
    done
fi
