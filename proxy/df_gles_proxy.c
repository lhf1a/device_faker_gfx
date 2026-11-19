/*
 * libGLESv2_df_proxy.so - Thin Android vendor GLESv2/v3 proxy for Device Faker GFX LKM mode.
 *
 * Exports glGetString (returning spoofed GL_RENDERER / GL_VENDOR / GL_VERSION from
 * df_gpu_vfs.ko) and forwards all other standard Android GLES 2.0-3.2 entry points via
 * zero-frame AArch64 BTI-safe tail-call thunks to the real underlying vendor driver
 * opened via ".df_orig_" bypass path inside Android's sphal linker namespace.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "df_proxy_common.h"
#include "df_gles_syms.inc"

#define DF_EXPORT __attribute__((visibility("default")))
#define DF_HIDDEN __attribute__((visibility("hidden")))
#define DF_TAG "df_gles_proxy"

#define GL_VENDOR   0x1F00u
#define GL_RENDERER 0x1F01u
#define GL_VERSION  0x1F02u

typedef const unsigned char *(*PFN_glGetString)(uint32_t name);

DF_HIDDEN void *g_df_gles_real_ptrs[DF_GLES_SYM_COUNT];

static pthread_once_t g_gles_init_once = PTHREAD_ONCE_INIT;
static int g_cfg_loaded = 0;
static struct df_gpu_self_cfg g_cfg;
static void *g_real_gles_handle = NULL;
static PFN_glGetString g_real_gl_get_string = NULL;

static void df_gles_do_init(void)
{
    const char *real_path;
    int i;

    g_cfg_loaded = df_query_self_cfg(&g_cfg);
    real_path = g_cfg.real_gles_path[0]
                    ? g_cfg.real_gles_path
                    : "/vendor/lib64/egl/" DF_GPU_ORIG_PREFIX "libGLESv2_freedreno.so";

    g_real_gles_handle = dlopen(real_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_real_gles_handle) {
        DF_PROXY_LOGE(DF_TAG, "dlopen(%s) failed: %s", real_path, dlerror());
        return;
    }

    g_real_gl_get_string = (PFN_glGetString)dlsym(g_real_gles_handle, "glGetString");
    for (i = 0; i < DF_GLES_SYM_COUNT; i++) {
        void *fn = dlsym(g_real_gles_handle, g_df_gles_sym_names[i]);
        __atomic_store_n(&g_df_gles_real_ptrs[i], fn, __ATOMIC_RELEASE);
    }

    if (g_cfg.flags & DF_GPU_FLAG_DEBUG_LOG) {
        DF_PROXY_LOGI(DF_TAG, "loaded real GLESv2 via %s (renderer=%s, vendor=%s)",
                      real_path, g_cfg.renderer, g_cfg.vendor);
    }
}

DF_HIDDEN void *df_gles_resolve_slot(uint32_t idx)
{
    pthread_once(&g_gles_init_once, df_gles_do_init);
    if (idx >= DF_GLES_SYM_COUNT)
        return NULL;
    return __atomic_load_n(&g_df_gles_real_ptrs[idx], __ATOMIC_ACQUIRE);
}

DF_EXPORT const unsigned char *glGetString(uint32_t name)
{
    pthread_once(&g_gles_init_once, df_gles_do_init);
    if (g_cfg_loaded) {
        if (name == GL_RENDERER && (g_cfg.flags & DF_GPU_FLAG_HAS_RENDERER) && g_cfg.renderer[0])
            return (const unsigned char *)g_cfg.renderer;
        if (name == GL_VENDOR && (g_cfg.flags & DF_GPU_FLAG_HAS_VENDOR) && g_cfg.vendor[0])
            return (const unsigned char *)g_cfg.vendor;
        if (name == GL_VERSION && (g_cfg.flags & DF_GPU_FLAG_HAS_GL_VERSION) && g_cfg.gl_version[0])
            return (const unsigned char *)g_cfg.gl_version;
    }
    return g_real_gl_get_string ? g_real_gl_get_string(name) : NULL;
}
