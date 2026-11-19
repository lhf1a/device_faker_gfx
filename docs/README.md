**简体中文** | [English](en/README.md) | [Türkçe](tr/README.md)

# Device Faker GFX 📱🎮

<img src="logo.png" style="width: 96px;" alt="logo">

一个基于 Zygisk 的机型与 GPU 伪装模块（Fork 自 [Seyud/device_faker](https://github.com/Seyud/device_faker)），可以为不同的应用单独配置伪装的设备型号、系统属性与 **OpenGL ES / Vulkan GPU 信息**。

[![Repository](https://img.shields.io/badge/GitHub-lhf1a%2Fdevice__faker__gfx-blue?logo=github)](https://github.com/lhf1a/device_faker_gfx)
[![Language](https://img.shields.io/badge/language-Rust-orange?logo=rust&logoColor=orange)](https://www.rust-lang.org/)

## 特性 ✨

- 🎯 **精确控制**: 为每个应用单独配置设备与 GPU 信息，仅对配置的应用生效，不影响其他应用
- 🎮 **GPU 伪装 (GLES / Vulkan)**: 无需替换系统驱动 `.so`，按进程动态 Hook OpenGL ES 与 Vulkan 显卡查询接口，并兼容 Mesa/Turnip/Zink 环境变量
- 📁 **模板管理**: 多机型模板，便捷应用到多包名
- 🔄 **实时生效**: 修改配置后仅需重启应用，无需重启系统
- 🛡️ **安全可靠**: 基于 Zygisk 框架，模块化设计
- 📝 **简单配置**: 使用 TOML 格式配置文件，易于编辑
- 🎭 **统一执行流**: 自动调度 JNI 字段伪装、COW 属性伪造、companion 服务与按需挂载的运行时 GPU Hook
- 🔒 **COW 属性引擎**: 进程私有属性区副本伪造系统属性，per-process 隔离；未启用 GPU 伪装的应用仍保持模块零驻留、不留写映射指纹
- 🌐 **WebUI管理**: 提供图形化界面，方便配置设备与 GPU 伪装参数

## 新增：GPU 伪装说明 🎮

`device_faker_gfx` 在上游机型/属性伪装的基础上新增了**按应用生效的运行时 GPU 伪装**能力：

- **不替换系统/Vendor 驱动**：完全保留原有驱动 `.so` 与系统的 Vulkan 策略路由机制（如 `sq2-vulkan-policy.conf`）。
- **OpenGL ES 伪装**：
  - 拦截 `glGetString`，覆盖 `GL_RENDERER`（`gpu_renderer` / 别名 `gpu`）、`GL_VENDOR`（`gpu_vendor`）与 `GL_VERSION`（`gpu_gl_version`）。
  - 在 `pre_app_specialize` 阶段为 Mesa / Turnip / Zink 驱动自动注入 `force_gl_renderer` 与 `force_gl_vendor` 环境变量（不影响非 Mesa 驱动）。
- **Vulkan 伪装**：
  - 拦截 `vkGetPhysicalDeviceProperties`、`vkGetPhysicalDeviceProperties2` 与 `vkGetPhysicalDeviceProperties2KHR`，覆盖 `deviceName`（`gpu_renderer`）、`vendorID`（`gpu_vendor_id`）、`deviceID`（`gpu_device_id`）与 `driverVersion`（`gpu_driver_version`）。
- **零驻留 LKM 代理模式（推荐，方案 2）**：
  - 加载配套内核模块 `df_gpu_vfs.ko`（位于 `lkm/`）后，companion 通过隐藏 `prctl` 通道（无 `/dev`、`/proc`、`/sys` 节点，非目标 UID 探测零响应）将目标应用的 UID 与 GPU 配置同步进内核。
  - 内核仅在目标 UID 打开厂商图形驱动（`/vendor/lib64/hw/vulkan.*.so`、`/vendor/lib64/egl/lib{EGL,GLESv2}_*.so`）时，在 VFS `getname_flags` 阶段将其按进程透明重定向到随模块打包的薄代理 `.so`（`vulkan.df_proxy.so` / `libEGL_df_proxy.so` / `libGLESv2_df_proxy.so`）；代理再通过 `.df_orig_` 旁路前缀加载真实底层驱动（完整保留 `sq2-vulkan-policy.conf` 等路由机制）。
  - **Zygisk 模块在 `pre_app_specialize` 写完属性后照常立即 `DlClose` 卸载**：实现 **零 Zygisk 驻留、零 GOT/PLT 改写、零 `dlopen`/`dlsym` Hook、零 `/proc/self/mountinfo` 挂载痕迹**。
- **进程内 GOT Hook 回退模式（兼容无 `.ko` 内核）**：
  - 默认 `gpu_backend = "auto"`：未加载 `df_gpu_vfs.ko` 时自动回退到进程内 ELF64 GOT/PLT 钩子（支持 `DT_JMPREL`、`DT_RELA`、Android `APS2` 压缩重定位及 `dlopen`/`android_dlopen_ext`/`dlsym`/`eglGetProcAddress`/`vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` 追踪）。
  - 可在 `config.toml` 顶部配置 `gpu_backend = "lkm"` 强制严格零驻留（不回退），或 `gpu_backend = "hook"` 强制进程内 Hook。
  - 未配置任何 `gpu_*` 字段的应用始终零驻留。
- **厂商自动推断**：
  - 仅填写 `gpu_renderer`（例如 `Adreno (TM) 840`、`Mali-G720-Immortalis`）而留空 `gpu_vendor` / `gpu_vendor_id` 时，会自动按关键字推断常见厂商名称与 PCI/Vulkan Vendor ID（支持 Qualcomm `0x5143`、ARM `0x13B5`、Imagination `0x1010`、Samsung `0x144D`、Apple `0x106B`、NVIDIA `0x10DE`、Intel `0x8086`、AMD `0x1002`、Mesa `0x10005`）。

### GPU 配置字段速览

| 字段 | 别名 / 格式 | 作用 |
| --- | --- | --- |
| `gpu_renderer` | 别名 `gpu`；字符串 | 覆盖 OpenGL ES `GL_RENDERER` 与 Vulkan `deviceName` |
| `gpu_vendor` | 字符串（留空可自动推断） | 覆盖 OpenGL ES `GL_VENDOR` 与 Mesa `force_gl_vendor` |
| `gpu_vendor_id` | 整数或 `"0x..."` 十六进制字符串（留空可自动推断） | 覆盖 Vulkan `VkPhysicalDeviceProperties.vendorID` |
| `gpu_device_id` | 整数或 `"0x..."` 十六进制字符串 | 覆盖 Vulkan `VkPhysicalDeviceProperties.deviceID` |
| `gpu_driver_version` | 整数或 `"0x..."` 十六进制字符串 | 覆盖 Vulkan `VkPhysicalDeviceProperties.driverVersion` |
| `gpu_gl_version` | 字符串 | 覆盖 OpenGL ES `GL_VERSION` |

示例：

```toml
[[apps]]
package = "com.example.game"
manufacturer = "OnePlus"
brand = "OnePlus"
model = "PLK110"
gpu_renderer = "Adreno (TM) 840"
gpu_device_id = "0x44030000"
# gpu_vendor 与 gpu_vendor_id 留空时会自动推断为 "Qualcomm" 与 0x5143
```

> 所有 GPU 字段同样支持写在 `[templates.<模板名>]` 与 WebUI 表单中。更多细节请见 [配置文档](CONFIG.md#gpu-伪装gles--vulkan)。

## WebUI 功能 🖥️

Device Faker GFX 提供了现代化的 Web 管理界面

- 📋 **模板管理**: 创建、编辑和删除机型与 GPU 模板，批量应用到多个包名
- 📱 **应用管理**: 直观查看已安装应用及其配置状态，支持多用户应用显示
- 🖋️ **配置编辑**: 图形化界面编辑应用与 GPU 配置，支持模板应用和自定义配置
- 🌍 **多语言支持**: 简体中文、English、Türkçe

## 配置说明 ⚙️

详细的配置说明请参考 [配置文档](CONFIG.md)。

配置文件位于 `/data/adb/device_faker/config/config.toml`，使用 TOML 格式。修改配置后仅需重启对应应用即可生效，无需重启系统。

## 模板配置贡献 🎁

上游 Device Faker 配置仓库：

- 📦 [device_faker_config](https://github.com/Seyud/device_faker_config) - 机型模板配置参考

## 致谢 🙏

本项目基于以下优秀项目构建与参考：

- [Seyud/device_faker](https://github.com/Seyud/device_faker) - 上游 Device Faker 原作者（酷安@瓦力喀 / GitHub@Seyud）
- [zygisk-dump-dex](https://github.com/ri-char/zygisk-dump-dex) - 提供了 Rust 开发 Zygisk 模块的原型参考
- [zygisk-api-rs](https://github.com/rmnscnce/zygisk-api-rs) - 提供了 Zygisk API的 Rust 依赖支持
- [MiPushZygisk](https://github.com/wushidia/MiPushZygisk) - 提供了 Zygisk 机型伪装的方案参考

感谢这些项目的开发者！💖

---

**📱 让设备不为应用的机型与显卡限制所困！** 🚀
