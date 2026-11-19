/*
 * Common helpers for Device Faker GFX thin user-space graphics proxies.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DF_PROXY_COMMON_H
#define DF_PROXY_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>

#ifdef __ANDROID__
#include <android/log.h>
#define DF_PROXY_LOGI(tag, ...) __android_log_print(ANDROID_LOG_INFO, (tag), __VA_ARGS__)
#define DF_PROXY_LOGE(tag, ...) __android_log_print(ANDROID_LOG_ERROR, (tag), __VA_ARGS__)
#else
#define DF_PROXY_LOGI(tag, ...) ((void)0)
#define DF_PROXY_LOGE(tag, ...) ((void)0)
#endif

#include "../include/df_gpu_abi.h"

static inline int df_query_self_cfg(struct df_gpu_self_cfg *out)
{
    volatile uint32_t reply = 0;
    int attempt;

    if (!out)
        return 0;

    for (attempt = 0; attempt < 2; attempt++) {
        /* Pre-fault user pages so kernel copy_to_user_nofault never hits a page fault. */
        memset(out, 0, sizeof(*out));
        reply = 0;
        (void)prctl(DF_GPU_PRCTL_MAGIC,
                    (unsigned long)DF_GPU_CMD_GET_SELF_CFG,
                    (unsigned long)out,
                    (unsigned long)sizeof(*out),
                    (unsigned long)&reply);
        if (reply == DF_GPU_REPLY_OK && out->abi_version == DF_GPU_ABI_VERSION)
            return 1;
    }
    return 0;
}

#endif /* DF_PROXY_COMMON_H */
