# AIMP Zip Plugin
n[English](README.md) | **Türkçe**

AIMP 6 (64-bit) için C++ eklentisi. `.zip` arşivlerini playlist'e açar ve içindeki ses dosyalarını `zip://<arşiv>:<girdi>` sanal yolu üzerinden çaldırır. Arşivi önceden çıkarmanız gerekmez.

## Özellikler

- Çalınabilir girdileri AIMP kendisi seçer, bu yüzden kurulu tüm kod çözücüler desteklenir (mp3, flac, wav, ogg, opus, m4a, ape, wv, ...).
- Tracker modülleri (xm/it/mod/s3m/mtm) AIMP'in BASS kütüphanesiyle bellekten çalınır.
- Türkçe (OEM kod sayfası) ve UTF-8 girdi adları doğru gösterilir.
- Salt okunur: arşiv değiştirilmez.

## Derleme

Visual Studio 2022+ ve CMake gerekir:

```powershell
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release
```

Çıktı: `build-x64\bin\Release\aimp_zip.dll`

## Kurulum

AIMP'i kapatın ve DLL'i `C:\Program Files\AIMP\Plugins\aimp_zip\aimp_zip.dll` konumuna kopyalayın (ya da `install_x64.bat` dosyasını yönetici olarak çalıştırın). Sonra AIMP'i açın.

## Lisans

MIT. Ayrıntılar için [LICENSE](LICENSE); kullanılan diğer bileşenler için [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
