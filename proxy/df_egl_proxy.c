/*
 * libEGL_df_proxy.so - Thin Android vendor EGL proxy for Device Faker GFX LKM mode.
 *
 * Uses lazy initialization (first actual egl* call, never in constructor) so Android's
 * libEGL.so completes loading libEGL_<tag>.so and libGLESv2_<tag>.so cleanly before
 * the proxy opens the real underlying vendor EGL library via ".df_orig_" bypass path.
 * Intercepts eglGetProcAddress("glGetString") to return the spoofed GL string wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "df_proxy_common.h"
#include "df_egl_syms.inc"

#define DF_EXPORT __attribute__((visibility("default")))
#define DF_HIDDEN __attribute__((visibility("hidden")))
#define DF_TAG "df_egl_proxy"

#define GL_VENDOR   0x1F00u
#define GL_RENDERER 0x1F01u
#define GL_VERSION  0x1F02u

typedef void *(*PFN_eglGetProcAddress)(const char *procname);
typedef const unsigned char *(*PFN_glGetString)(uint32_t name);

DF_HIDDEN void *g_df_egl_real_ptrs[DF_EGL_SYM_COUNT];

static pthread_once_t g_egl_init_once = PTHREAD_ONCE_INIT;
static int g_cfg_loaded = 0;
static struct df_gpu_self_cfg g_cfg;
static void *g_real_egl_handle = NULL;
static PFN_eglGetProcAddress g_real_gpa = NULL;
static PFN_glGetString g_real_gl_get_string = NULL;

static void df_egl_do_init(void)
{
    const char *real_path;
    int i;

    g_cfg_loaded = df_query_self_cfg(&g_cfg);
    real_path = g_cfg.real_egl_path[0]
                    ? g_cfg.real_egl_path
                    : "/vendor/lib64/egl/" DF_GPU_ORIG_PREFIX "libEGL_freedreno.so";

    g_real_egl_handle = dlopen(real_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_real_egl_handle) {
        DF_PROXY_LOGE(DF_TAG, "dlopen(%s) failed: %s", real_path, dlerror());
        return;
    }

    g_real_gpa = (PFN_eglGetProcAddress)dlsym(g_real_egl_handle, "eglGetProcAddress");
    for (i = 0; i < DF_EGL_SYM_COUNT; i++) {
        void *fn = dlsym(g_real_egl_handle, g_df_egl_sym_names[i]);
        if (!fn && g_real_gpa)
            fn = g_real_gpa(g_df_egl_sym_names[i]);
        __atomic_store_n(&g_df_egl_real_ptrs[i], fn, __ATOMIC_RELEASE);
    }

    if (g_cfg.flags & DF_GPU_FLAG_DEBUG_LOG) {
        DF_PROXY_LOGI(DF_TAG, "loaded real EGL via %s (renderer=%s, vendor=%s)",
                      real_path, g_cfg.renderer, g_cfg.vendor);
    }
}

DF_HIDDEN void *df_egl_resolve_slot(uint32_t idx)
{
    pthread_once(&g_egl_init_once, df_egl_do_init);
    if (idx >= DF_EGL_SYM_COUNT)
        return NULL;
    return __atomic_load_n(&g_df_egl_real_ptrs[idx], __ATOMIC_ACQUIRE);
}

static const unsigned char *df_egl_spoof_glGetString(uint32_t name)
{
    if (g_cfg_loaded) {
        if (name == GL_RENDERER && (g_cfg.flags & DF_GPU_FLAG_HAS_RENDERER) && g_cfg.renderer[0])
            return (const unsigned char *)g_cfg.renderer;
        if (name == GL_VENDOR && (g_cfg.flags & DF_GPU_FLAG_HAS_VENDOR) && g_cfg.vendor[0])
            return (const unsigned char *)g_cfg.vendor;
        if (name == GL_VERSION && (g_cfg.flags & DF_GPU_FLAG_HAS_GL_VERSION) && g_cfg.gl_version[0])
            return (const unsigned char *)g_cfg.gl_version;
    }
    PFN_glGetString real_fn = __atomic_load_n(&g_real_gl_get_string, __ATOMIC_ACQUIRE);
    return real_fn ? real_fn(name) : NULL;
}

DF_EXPORT void *eglGetProcAddress(const char *procname)
{
    void *fn;

    pthread_once(&g_egl_init_once, df_egl_do_init);
    if (!g_real_gpa)
        return NULL;

    fn = g_real_gpa(procname);
    if (fn && procname && strcmp(procname, "glGetString") == 0) {
        __atomic_store_n(&g_real_gl_get_string, (PFN_glGetString)fn, __ATOMIC_RELEASE);
        return (void *)df_egl_spoof_glGetString;
    }
    return fn;
}
