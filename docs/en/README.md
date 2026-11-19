**English** | [简体中文](../README.md) | [Türkçe](../tr/README.md)

# Device Faker GFX 📱🎮

<img src="../logo.png" style="width: 96px;" alt="logo">

A Zygisk-based device & GPU spoofing module (forked from [Seyud/device_faker](https://github.com/Seyud/device_faker)) that configures different device profiles, system properties, and **OpenGL ES / Vulkan GPU identities** per app.

[![Repository](https://img.shields.io/badge/GitHub-lhf1a%2Fdevice__faker__gfx-blue?logo=github)](https://github.com/lhf1a/device_faker_gfx)
[![Language](https://img.shields.io/badge/language-Rust-orange?logo=rust&logoColor=orange)](https://www.rust-lang.org/)

## Features ✨

- 🎯 **Precise Control**: Configure device and GPU information individually for each application, only takes effect on configured apps
- 🎮 **GPU Spoofing (GLES / Vulkan)**: Per-process runtime hooking of OpenGL ES and Vulkan device query APIs without replacing any system/vendor driver `.so`, plus Mesa/Turnip/Zink environment variables
- 📁 **Template Management**: Multiple device templates, easily apply to multiple package names
- 🔄 **Real-time Effect**: After modifying configuration, just restart the application, no need to restart the system
- 🛡️ **Safe and Reliable**: Based on Zygisk framework, modular design
- 📝 **Simple Configuration**: Using TOML format configuration files, easy to edit
- 🎭 **Unified Execution Flow**: Automatically schedules JNI field spoofing, COW property spoofing, companion services, and on-demand runtime GPU hooks
- 🔒 **COW Property Engine**: Process-private property-area copies for spoofing with per-process isolation; apps without GPU spoofing still unload the module with zero residency and no write-mapping fingerprint
- 🌐 **WebUI Management**: Provides graphical interface for convenient device and GPU configuration management

## What's New: GPU Spoofing 🎮

`device_faker_gfx` adds **per-app runtime GPU spoofing** on top of upstream device/property spoofing:

- **No system/vendor driver replacement**: Leaves all system and vendor driver `.so` files untouched (preserving system Vulkan policy routing such as `sq2-vulkan-policy.conf`).
- **OpenGL ES spoofing**:
  - Intercepts `glGetString` to override `GL_RENDERER` (`gpu_renderer` / alias `gpu`), `GL_VENDOR` (`gpu_vendor`), and `GL_VERSION` (`gpu_gl_version`).
  - Sets `force_gl_renderer` and `force_gl_vendor` environment variables in `pre_app_specialize` for Mesa / Turnip / Zink drivers (no-op for non-Mesa drivers).
- **Vulkan spoofing**:
  - Intercepts `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceProperties2`, and `vkGetPhysicalDeviceProperties2KHR` to override `deviceName` (`gpu_renderer`), `vendorID` (`gpu_vendor_id`), `deviceID` (`gpu_device_id`), and `driverVersion` (`gpu_driver_version`).
- **Dynamic library tracking & on-demand residency**:
  - Only stays resident in processes that configure at least one `gpu_*` field, patching ELF64 GOT/PLT entries (`DT_JMPREL`, `DT_RELA`, and Android packed `APS2` `DT_ANDROID_RELA`) and tracking late-loaded graphics libraries via `dlopen`, `android_dlopen_ext`, `dlsym`, `eglGetProcAddress`, `vkGetInstanceProcAddr`, and `vkGetDeviceProcAddr`.
  - Apps with no `gpu_*` fields do not activate GPU hooks and still unload the module via `DlClose` immediately.
- **Automatic vendor inference**:
  - When `gpu_renderer` is set (e.g. `Adreno (TM) 840`, `Mali-G720-Immortalis`) and `gpu_vendor` / `gpu_vendor_id` are left blank, common vendor names and Vulkan/PCI vendor IDs are inferred automatically (Qualcomm `0x5143`, ARM `0x13B5`, Imagination `0x1010`, Samsung `0x144D`, Apple `0x106B`, NVIDIA `0x10DE`, Intel `0x8086`, AMD `0x1002`, Mesa `0x10005`).

### GPU Field Quick Reference

| Field | Alias / Format | Effect |
| --- | --- | --- |
| `gpu_renderer` | Alias `gpu`; string | Overrides OpenGL ES `GL_RENDERER` and Vulkan `deviceName` |
| `gpu_vendor` | String (auto-inferred if blank) | Overrides OpenGL ES `GL_VENDOR` and Mesa `force_gl_vendor` |
| `gpu_vendor_id` | Integer or `"0x..."` hex string (auto-inferred if blank) | Overrides Vulkan `VkPhysicalDeviceProperties.vendorID` |
| `gpu_device_id` | Integer or `"0x..."` hex string | Overrides Vulkan `VkPhysicalDeviceProperties.deviceID` |
| `gpu_driver_version` | Integer or `"0x..."` hex string | Overrides Vulkan `VkPhysicalDeviceProperties.driverVersion` |
| `gpu_gl_version` | String | Overrides OpenGL ES `GL_VERSION` |

Example:

```toml
[[apps]]
package = "com.example.game"
manufacturer = "OnePlus"
brand = "OnePlus"
model = "PLK110"
gpu_renderer = "Adreno (TM) 840"
gpu_device_id = "0x44030000"
# gpu_vendor and gpu_vendor_id are auto-inferred as "Qualcomm" and 0x5143
```

> All GPU fields are also supported in `[templates.<name>]` and in the WebUI editor. See [Configuration Guide](CONFIG.md#gpu-spoofing-gles--vulkan) for full details.

## WebUI Features 🖥️

Device Faker GFX provides a modern web management interface.

- 📋 **Template Management**: Create, edit and delete device & GPU templates, batch apply to multiple package names
- 📱 **Application Management**: Intuitive view of installed applications and their configuration status, supports multi-user apps display
- 🖋️ **Configuration Editing**: Graphical interface for editing device and GPU configurations, supporting template application and custom configuration
- 🌍 **Multilingual**: 简体中文, English, Türkçe

## Configuration Guide ⚙️

For detailed configuration instructions, please refer to the [Configuration Documentation](CONFIG.md).

The configuration file is located at `/data/adb/device_faker/config/config.toml` and uses TOML format. After modifying the configuration, just restart the corresponding application to take effect, no need to restart the system.

## Template Configuration Contribution 🎁

Upstream Device Faker configuration repository:

- 📦 [device_faker_config](https://github.com/Seyud/device_faker_config) - Device template configurations reference

## Acknowledgments 🙏

This project is built upon and references the following excellent projects:

- [Seyud/device_faker](https://github.com/Seyud/device_faker) - Upstream Device Faker (酷安@瓦力喀 / GitHub@Seyud)
- [zygisk-dump-dex](https://github.com/ri-char/zygisk-dump-dex) - Provides prototype reference for Rust Zygisk module development
- [zygisk-api-rs](https://github.com/rmnscnce/zygisk-api-rs) - Provides Rust dependency support for Zygisk API
- [MiPushZygisk](https://github.com/wushidia/MiPushZygisk) - Provides reference for Zygisk device spoofing solutions

Thanks to the developers of these projects! 💖

---

**📱 Let devices not be limited by application model or GPU restrictions!** 🚀
