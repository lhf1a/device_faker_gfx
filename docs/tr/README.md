**Türkçe** | [简体中文](../README.md) | [English](../en/README.md)

# Device Faker GFX 📱🎮

<img src="../logo.png" style="width: 96px;" alt="logo">

Zygisk tabanlı, farklı uygulamalar için farklı cihaz modelleri, sistem özellikleri ve **OpenGL ES / Vulkan GPU kimlikleri** yapılandırabilen bir cihaz ve GPU taklit modülü ([Seyud/device_faker](https://github.com/Seyud/device_faker) çatallaması).

[![Repository](https://img.shields.io/badge/GitHub-lhf1a%2Fdevice__faker__gfx-blue?logo=github)](https://github.com/lhf1a/device_faker_gfx)
[![Language](https://img.shields.io/badge/language-Rust-orange?logo=rust&logoColor=orange)](https://www.rust-lang.org/)

## Özellikler ✨

* 🎯 **Hassas Kontrol**: Her uygulama için cihaz ve GPU bilgilerini ayrı ayrı yapılandırın; yalnızca yapılandırılmış uygulamalarda etkili olur.
* 🎮 **GPU Taklidi (GLES / Vulkan)**: Sistem/vendor sürücü `.so` dosyalarını değiştirmeden OpenGL ES ve Vulkan GPU sorgularını çalışma zamanında yakalar; Mesa/Turnip/Zink ortam değişkenlerini de ayarlar.
* 📁 **Şablon Yönetimi**: Birden fazla cihaz şablonu oluşturun ve bunları kolayca birden fazla paket adına uygulayın.
* 🔄 **Anında Etki**: Yapılandırmayı değiştirdikten sonra yalnızca uygulamayı yeniden başlatmanız yeterlidir, sistemi yeniden başlatmanıza gerek yoktur.
* 🛡️ **Güvenli ve Kararlı**: Zygisk altyapısı üzerine kurulmuştur ve modüler tasarıma sahiptir.
* 📝 **Basit Yapılandırma**: Düzenlemesi kolay TOML formatındaki yapılandırma dosyalarını kullanır.
* 🎭 **Birleşik Yürütme Akışı**: JNI alan sahteleştirmesi, COW özellik sahteleştirmesi, companion hizmetleri ve isteğe bağlı çalışma zamanı GPU kancalarını otomatik olarak yönetir.
* 🔒 **COW Özellik Motoru**: Sistem özelliklerini sürece özel özellik alanı kopyalarıyla sahteler; GPU taklidi kullanmayan uygulamalarda sıfır kalıcılık ve yazma eşlemesi izi bırakmaz.
* 🌐 **WebUI Yönetimi**: Cihaz ve GPU yapılandırmalarını kolayca yönetebilmeniz için grafiksel bir web arayüzü sunar.

## Yenilik: GPU Taklidi (OpenGL ES / Vulkan) 🎮

`device_faker_gfx`, yukarı akış cihaz/özellik sahteleştirmesine ek olarak **uygulama bazlı çalışma zamanı GPU taklidi** ekler:

- **Sürücü `.so` değişimi yok**: Sistem ve vendor grafik sürücülerine dokunulmaz (`sq2-vulkan-policy.conf` gibi yönlendirmeler korunur).
- **OpenGL ES**: `glGetString` üzerinden `GL_RENDERER` (`gpu_renderer` / `gpu`), `GL_VENDOR` (`gpu_vendor`) ve `GL_VERSION` (`gpu_gl_version`) değerlerini değiştirir; ayrıca `pre_app_specialize` aşamasında Mesa/Turnip/Zink için `force_gl_renderer` ve `force_gl_vendor` ortam değişkenlerini ayarlar.
- **Vulkan**: `vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceProperties2` ve `vkGetPhysicalDeviceProperties2KHR` üzerinden `deviceName` (`gpu_renderer`), `vendorID` (`gpu_vendor_id`), `deviceID` (`gpu_device_id`) ve `driverVersion` (`gpu_driver_version`) alanlarını günceller.
- **Dinamik yükleme takibi ve otomatik çıkarım**: Yalnızca herhangi bir `gpu_*` alanı ayarlanan uygulamalarda ELF64 GOT/PLT kancaları ve `dlopen`/`dlsym`/`eglGetProcAddress`/`vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` takibi etkin kalır; `gpu_vendor` ve `gpu_vendor_id` boş bırakılırsa `gpu_renderer` adına göre otomatik tahmin edilir (Qualcomm, ARM, Imagination, Samsung, Apple, NVIDIA, Intel, AMD, Mesa).

## WebUI Özellikleri 🖥️

Device Faker GFX modern, web tabanlı bir yönetim arayüzü sunar.

- 📋 **Şablon Yönetimi**: Cihaz ve GPU şablonları oluşturun, düzenleyin ve silin; birden fazla paket adına toplu olarak uygulayın
- 📱 **Uygulama Yönetimi**: Yüklü uygulamalar ve bunların yapılandırma durumuna ilişkin sezgisel bir görünüm; çok kullanıcılı uygulamaların görüntülenmesini destekler
- 🖋️ **Yapılandırma Düzenleme**: Cihaz ve GPU yapılandırmasını düzenlemek için grafik arayüz; şablon uygulamaları ve özel yapılandırmaları destekler
- 🌍 **Çoklu Dil Desteği**: 简体中文, English, Türkçe

## Yapılandırma Rehberi ⚙️

Ayrıntılı yapılandırma talimatları için lütfen [Yapılandırma Belgeleri](../en/CONFIG.md) bölümüne bakın.

Yapılandırma dosyası `/data/adb/device_faker/config/config.toml` konumunda bulunur ve TOML biçimini kullanır. Yapılandırmayı değiştirdikten sonra, değişikliklerin yürürlüğe girmesi için ilgili uygulamayı yeniden başlatmanız yeterlidir; sistemi yeniden başlatmanıza gerek yoktur.

## Şablon Yapılandırması Katkıları 🎁

Yukarı akış yapılandırma deposu:

- 📦 [device_faker_config](https://github.com/Seyud/device_faker_config) - Cihaz şablonu yapılandırmaları referansı

## Teşekkürler 🙏

Bu proje, geliştirme aşamasında aşağıdaki mükemmel projelerden yararlanmaktadır:

- [Seyud/device_faker](https://github.com/Seyud/device_faker) - Yukarı akış Device Faker (酷安@瓦力喀 / GitHub@Seyud)
- [zygisk-dump-dex](https://github.com/ri-char/zygisk-dump-dex) - Rust Zygisk modülü geliştirme için prototip referansı sunar
- [zygisk-api-rs](https://github.com/rmnscnce/zygisk-api-rs) - Zygisk API için Rust bağımlılık desteği sağlar
- [MiPushZygisk](https://github.com/wushidia/MiPushZygisk) - Zygisk cihaz aldatma çözümleri için referans sağlar

Bu projelerin geliştiricilerine teşekkürler! 💖

---

**📱 Cihazlar, uygulama model ve GPU kısıtlamalarıyla sınırlanmasın!** 🚀
