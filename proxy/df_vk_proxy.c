/*
 * vulkan.df_proxy.so - Thin Android Vulkan HAL (hwvulkan_module_t) proxy for
 * Device Faker GFX LKM zero-residency mode.
 *
 * Loaded transparently via df_gpu_vfs.ko in place of /vendor/lib64/hw/vulkan.<hw>.so
 * only for targeted UIDs. Opens the real underlying HAL via ".df_orig_" bypass path
 * inside Android's sphal linker namespace (preserving dual-Vulkan routers such as
 * sq2-vulkan-policy.conf) and overrides VkPhysicalDeviceProperties fields.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "df_proxy_common.h"

#define DF_EXPORT __attribute__((visibility("default")))
#define DF_TAG "df_vk_proxy"

/* ---- Binary ABI types matching Android hardware/hardware.h + hwvulkan.h ---- */

#define DF_MAKE_TAG(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define DF_HARDWARE_MODULE_TAG DF_MAKE_TAG('H', 'W', 'M', 'T')
#define DF_HARDWARE_DEVICE_TAG DF_MAKE_TAG('H', 'W', 'D', 'T')
#define DF_HARDWARE_HAL_API_VERSION 0x0100u
#define DF_HWVULKAN_MODULE_API_VERSION_0_1 0x0001u
#define DF_HWVULKAN_HARDWARE_MODULE_ID "vulkan"
#define HAL_MODULE_INFO_SYM HMI

#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256

struct df_hw_module_t;
struct df_hw_device_t;

typedef struct df_hw_module_methods_t {
    int (*open)(const struct df_hw_module_t *module, const char *id,
                struct df_hw_device_t **device);
} df_hw_module_methods_t;

typedef struct df_hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    df_hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
} df_hw_module_t;

typedef struct df_hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct df_hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct df_hw_device_t *device);
} df_hw_device_t;

typedef struct df_hwvulkan_module_t {
    df_hw_module_t common;
} df_hwvulkan_module_t;

typedef void *(*PFN_vkGetInstanceProcAddr)(void *instance, const char *pName);
typedef void (*PFN_vkGetPhysicalDeviceProperties)(void *physDev, void *pProps);
typedef void (*PFN_vkGetPhysicalDeviceProperties2)(void *physDev, void *pProps2);

typedef struct df_hwvulkan_device_t {
    df_hw_device_t common;
    void *EnumerateInstanceExtensionProperties;
    void *CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} df_hwvulkan_device_t;

typedef struct df_vk_props_prefix {
    uint32_t api_version;
    uint32_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t device_type;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
} df_vk_props_prefix;

typedef struct df_vk_props2_prefix {
    uint32_t s_type;
    void *p_next;
    df_vk_props_prefix properties;
} df_vk_props2_prefix;

_Static_assert(sizeof(void *) == 8, "Expected LP64");
_Static_assert(sizeof(df_hw_module_t) == 248, "hw_module_t size mismatch");
_Static_assert(sizeof(df_hw_device_t) == 120, "hw_device_t size mismatch");
_Static_assert(sizeof(df_hwvulkan_device_t) == 144, "hwvulkan_device_t size mismatch");

typedef struct df_wrapped_vk_device {
    df_hwvulkan_device_t dev;
    df_hwvulkan_device_t *real_dev;
} df_wrapped_vk_device;

static pthread_mutex_t g_vk_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cfg_loaded = 0;
static struct df_gpu_self_cfg g_cfg;
static void *g_real_handle = NULL;
static df_hwvulkan_module_t *g_real_hmi = NULL;
static PFN_vkGetInstanceProcAddr g_real_gipa = NULL;
static PFN_vkGetPhysicalDeviceProperties g_real_get_props = NULL;
static PFN_vkGetPhysicalDeviceProperties2 g_real_get_props2 = NULL;
static PFN_vkGetPhysicalDeviceProperties2 g_real_get_props2_khr = NULL;

static void df_patch_vk_props(df_vk_props_prefix *props)
{
    if (!props || !g_cfg_loaded)
        return;

    if (g_cfg.flags & DF_GPU_FLAG_HAS_DRIVER_VERSION)
        props->driver_version = g_cfg.driver_version;
    if (g_cfg.flags & DF_GPU_FLAG_HAS_VENDOR_ID)
        props->vendor_id = g_cfg.vendor_id;
    if (g_cfg.flags & DF_GPU_FLAG_HAS_DEVICE_ID)
        props->device_id = g_cfg.device_id;
    if ((g_cfg.flags & DF_GPU_FLAG_HAS_RENDERER) && g_cfg.renderer[0] != '\0') {
        size_t len = strnlen(g_cfg.renderer, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
        memset(props->device_name, 0, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
        memcpy(props->device_name, g_cfg.renderer, len);
    }
}

static void df_vkGetPhysicalDeviceProperties(void *physDev, void *pProps)
{
    PFN_vkGetPhysicalDeviceProperties fn = __atomic_load_n(&g_real_get_props, __ATOMIC_ACQUIRE);
    if (!fn)
        return;
    fn(physDev, pProps);
    df_patch_vk_props((df_vk_props_prefix *)pProps);
}

static void df_vkGetPhysicalDeviceProperties2(void *physDev, void *pProps2)
{
    PFN_vkGetPhysicalDeviceProperties2 fn = __atomic_load_n(&g_real_get_props2, __ATOMIC_ACQUIRE);
    if (!fn)
        return;
    fn(physDev, pProps2);
    if (pProps2)
        df_patch_vk_props(&((df_vk_props2_prefix *)pProps2)->properties);
}

static void df_vkGetPhysicalDeviceProperties2KHR(void *physDev, void *pProps2)
{
    PFN_vkGetPhysicalDeviceProperties2 fn =
        __atomic_load_n(&g_real_get_props2_khr, __ATOMIC_ACQUIRE);
    if (!fn)
        return;
    fn(physDev, pProps2);
    if (pProps2)
        df_patch_vk_props(&((df_vk_props2_prefix *)pProps2)->properties);
}

static void *df_vkGetInstanceProcAddr(void *instance, const char *pName)
{
    PFN_vkGetInstanceProcAddr real_gipa = __atomic_load_n(&g_real_gipa, __ATOMIC_ACQUIRE);
    void *fn;

    if (!real_gipa)
        return NULL;

    fn = real_gipa(instance, pName);
    if (!fn || !pName)
        return fn;

    if (strcmp(pName, "vkGetPhysicalDeviceProperties") == 0) {
        __atomic_store_n(&g_real_get_props, (PFN_vkGetPhysicalDeviceProperties)fn, __ATOMIC_RELEASE);
        return (void *)df_vkGetPhysicalDeviceProperties;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0) {
        __atomic_store_n(&g_real_get_props2, (PFN_vkGetPhysicalDeviceProperties2)fn, __ATOMIC_RELEASE);
        return (void *)df_vkGetPhysicalDeviceProperties2;
    }
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) {
        __atomic_store_n(&g_real_get_props2_khr, (PFN_vkGetPhysicalDeviceProperties2)fn,
                         __ATOMIC_RELEASE);
        return (void *)df_vkGetPhysicalDeviceProperties2KHR;
    }
    return fn;
}

static int df_vk_close(df_hw_device_t *device)
{
    df_wrapped_vk_device *wrapped = (df_wrapped_vk_device *)device;
    int rc = 0;

    if (!wrapped)
        return 0;
    if (wrapped->real_dev && wrapped->real_dev->common.close)
        rc = wrapped->real_dev->common.close(&wrapped->real_dev->common);
    free(wrapped);
    return rc;
}

static int df_vk_open(const df_hw_module_t *module, const char *id,
                      df_hw_device_t **device)
{
    df_hw_device_t *real_raw = NULL;
    df_hwvulkan_device_t *real_dev;
    df_wrapped_vk_device *wrapped;
    int rc;

    if (!device)
        return -EINVAL;
    *device = NULL;

    pthread_mutex_lock(&g_vk_lock);
    if (!g_cfg_loaded)
        g_cfg_loaded = df_query_self_cfg(&g_cfg);

    if (!g_real_hmi) {
        const char *real_path = g_cfg.real_vulkan_path[0]
                                    ? g_cfg.real_vulkan_path
                                    : "/vendor/lib64/hw/" DF_GPU_ORIG_PREFIX "vulkan.freedreno.so";
        g_real_handle = dlopen(real_path, RTLD_NOW | RTLD_LOCAL);
        if (!g_real_handle) {
            DF_PROXY_LOGE(DF_TAG, "dlopen(%s) failed: %s", real_path, dlerror());
            pthread_mutex_unlock(&g_vk_lock);
            return -ENOENT;
        }
        g_real_hmi = (df_hwvulkan_module_t *)dlsym(g_real_handle, "HMI");
        if (!g_real_hmi || !g_real_hmi->common.methods || !g_real_hmi->common.methods->open) {
            DF_PROXY_LOGE(DF_TAG, "real HMI missing in %s", real_path);
            pthread_mutex_unlock(&g_vk_lock);
            return -EINVAL;
        }
        if (g_cfg.flags & DF_GPU_FLAG_DEBUG_LOG) {
            DF_PROXY_LOGI(DF_TAG, "loaded real Vulkan HAL via %s (renderer=%s, vendor_id=0x%04x)",
                          real_path, g_cfg.renderer, g_cfg.vendor_id);
        }
    }
    pthread_mutex_unlock(&g_vk_lock);

    rc = g_real_hmi->common.methods->open(&g_real_hmi->common, id, &real_raw);
    if (rc != 0 || !real_raw)
        return rc ? rc : -ENODEV;

    real_dev = (df_hwvulkan_device_t *)real_raw;
    wrapped = (df_wrapped_vk_device *)calloc(1, sizeof(*wrapped));
    if (!wrapped) {
        if (real_dev->common.close)
            real_dev->common.close(&real_dev->common);
        return -ENOMEM;
    }

    wrapped->real_dev = real_dev;
    wrapped->dev = *real_dev;
    wrapped->dev.common.module = (df_hw_module_t *)module;
    wrapped->dev.common.close = df_vk_close;
    __atomic_store_n(&g_real_gipa, real_dev->GetInstanceProcAddr, __ATOMIC_RELEASE);
    wrapped->dev.GetInstanceProcAddr = df_vkGetInstanceProcAddr;

    *device = &wrapped->dev.common;
    return 0;
}

static df_hw_module_methods_t g_vk_methods = {
    .open = df_vk_open,
};

DF_EXPORT df_hwvulkan_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = DF_HARDWARE_MODULE_TAG,
        .module_api_version = DF_HWVULKAN_MODULE_API_VERSION_0_1,
        .hal_api_version = DF_HARDWARE_HAL_API_VERSION,
        .id = DF_HWVULKAN_HARDWARE_MODULE_ID,
        .name = "Vulkan HAL Proxy",
        .author = "Device Faker GFX",
        .methods = &g_vk_methods,
    },
};
