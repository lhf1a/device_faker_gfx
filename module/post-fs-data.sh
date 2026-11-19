#!/system/bin/sh
# Device Faker GFX post-fs-data hook:
# 1. Label thin user-space graphics proxy libraries as same_process_hal_file for sphal/SELinux.
# 2. Load df_gpu_vfs.ko (per-UID VFS redirector) if available for the running kernel.

MODDIR="${0%/*}"

for so in \
    "$MODDIR/system/vendor/lib64/hw/vulkan.df_proxy.so" \
    "$MODDIR/system/vendor/lib64/egl/libEGL_df_proxy.so" \
    "$MODDIR/system/vendor/lib64/egl/libGLESv2_df_proxy.so"
do
    if [ -f "$so" ]; then
        chmod 0644 "$so" 2>/dev/null
        chcon u:object_r:same_process_hal_file:s0 "$so" 2>/dev/null
    fi
done

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
