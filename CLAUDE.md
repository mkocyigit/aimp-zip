# AIMP Zip Plugin — çalışma notları

`.zip` arşivlerini AIMP playlist'ine açan, içindeki ses dosyalarını
`zip://<arşiv>:<girdi>` sanal yolu üzerinden çaldıran C++ eklentisi.
Tek kaynak dosya: [src/ZipPlugin.cpp](src/ZipPlugin.cpp)

## Derleme ve kurulum

AIMP **64-bit**. Doğru çıktı `build-x64\bin\Release\aimp_zip.dll`.
`build\` (Win32) çıktısını kurmak eklentinin hiç yüklenmemesine yol açar —
AIMP sessizce atlar, hata vermez.

```powershell
cmake --build build-x64 --config Release
```

Kurulum hedefi: `C:\Program Files\AIMP\Plugins\aimp_zip\aimp_zip.dll`
(yönetici izni gerekir; [install_x64.bat](install_x64.bat) bunu yapar).
AIMP kapalı olmalı.

CMake ve MSBuild PATH'te değil, tam yol:
`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

## SDK'da belgelenmemiş olan, deneyerek bulunanlar

AIMP SDK'sında bu yol için ne dokümantasyon ne demo var; aşağıdakiler
AIMP'in davranışı gözlemlenerek çıkarıldı. Değiştirmeden önce sebebini oku.

- **Expander'ın döndürdüğü liste `IAIMPVirtualFile` içermeli.** `IAIMPString`
  koyunca AIMP her elemandan `IAIMPVirtualFile` isteyip `E_NOINTERFACE` alıyor
  ve girdilerin tamamını sessizce eliyor — playlist boş kalıyor, hata çıkmıyor.
- **URI ayracı `:` olmalı**, `|` değil. AIMP'in kendi biçimi
  `<konteyner>:<parça>` (`IAIMPServiceFileURI::Build` ile doğrulandı).
  `|` ile AIMP parçayı ayıramıyor.
- **`IAIMPExtensionFileExpander`, `IID_IAIMPServiceFileManager`'a kaydediliyor.**
  Belgelenmemiş; deneyerek bulundu.
- **AIMP bazen şemayı takılı bırakıyor** (`zip://C:\...\a.zip`). `StripScheme`
  bunun içindir, kaldırma.
- **Zip girdi adları**, "general purpose" bayrağının 11. biti yoksa UTF-8
  değil OEM kod sayfasındadır (TR'de CP857). Hepsini UTF-8 saymak Türkçe
  karakterleri bozuyordu.

## Format desteği

Hangi girdinin çalınabilir olduğuna **AIMP karar veriyor**
(`IAIMPServiceFileFormats::IsSupported`), sabit uzantı listesi değil. Böylece
kullanıcıda kurulu tüm kod çözücüler otomatik kapsamda. `LooksLikeAudio`
yalnızca servise ulaşılamazsa devreye giren yedek.

Test edilen 29 formattan 25'i tam çalışıyor. Bilinen boşluklar (ikisi de
AIMP kaynaklı, **düzeltilmesi istenmedi**):
- `.mid` — bass_midi ses fontu olmadan süre/bit hızı vermiyor
- `.ac3` — AIMP'in kendisi desteklemiyor (bass_ac3 kurulu ve etkin olmasına rağmen)

## Bilgi okuma (`ZipVirtualFile::GetFileInfo`)

İki aşamalı, sırayla:
1. **Ham akış** (`GetFileInfoFromStream`) — wav/flac/aac/alac/ogg/opus/wavpack.
2. **Doğru uzantılı geçici dosya** — MP3 bu yolu gerektiriyor; AIMP ham akıştan
   MP3'ü tanımıyor (`E_FAIL`). ID3v2'yi atlayıp denemek de çözmüyor, denendi.

**Kendi `zip://` URI'mizi `GetFileInfoFromFileURI`'ye verme** — AIMP dosya
sistemimize geri dönüyor, o da tekrar `GetFileInfo` çağırıyor: sonsuz özyineleme.

## Tracker modülleri (xm/it/mod/s3m/mtm)

AIMP modülleri BASS'a **URL üzerinden** devrediyor; `zip://` şemasını BASS
tanımadığı için hata 48 (`BASS_ERROR_PROTOCOL`) alınıyor ve arşiv içindeki
modüller çalmıyordu.

Çözüm: kendi ses çözücümüz (`ModuleDecoder` + `ModuleDecoderExtension`).
`IAIMPExtensionAudioDecoder::CreateDecoder` bize URI değil **`IAIMPStream`**
veriyor; akışı belleğe alıp `BASS_MusicLoad(mem=TRUE, ...)` ile açıyoruz.
`bass.dll` AIMP'in klasöründe ve süreçte zaten yüklü — `GetModuleHandle` ile
işaretçileri alınıyor, yanımızda DLL taşınmıyor.

Denenip **işe yaramayan** yol: `AIMP_VIRTUALFILE_PROPID_AUDIOSOURCEFILE`'dan
gerçek geçici dosya yolu vermek. Çıkarma çalışıyor ama AIMP yolu kullanmıyor.

`ModuleDecoder::GetFileInfo` boş bırakılırsa çalma başlayınca playlist'teki
başlık silinip dosya adına düşüyor — AIMP bilgiyi çözücüden tazeliyor.
Başlık/sanatçı `BASS_TAG_MUSIC_NAME` / `BASS_TAG_MUSIC_AUTH`'tan okunuyor.
Boş etiket **yazılmıyor**; yazılırsa `ZipVirtualFile`'ın girdi adından
türettiği başlık silinir.

## Teşhis

Varsayılan olarak tamamen kapalı, disk erişimi yok.

- **Log:** `%AppData%\AIMP\aimp_zip.debug` adında boş dosya oluştur, AIMP'i
  yeniden başlat. Log `%AppData%\AIMP\aimp_zip.log`'a yazılır. Anahtar süreç
  başına bir kez okunur — AIMP açıkken dosyayı oluşturmak işe yaramaz.
- **Öz-test:** `%AppData%\AIMP\aimp_zip.selftest` dosyasına bir zip yolu yaz
  (UTF-8, tek satır). Eklenti açılışta o arşivdeki her girdi için
  `GetFileInfo` çalıştırıp sonucu loglar. Format davranışını ölçmek için
  sürükle-bırak gerekmez.

### İzole test ortamı

AIMP **tek örnekli**: kullanıcının AIMP'i açıkken başlatılan ikinci örnek ona
yönlenir. Test için kullanıcının AIMP'i kapalı olmalı.

`scratchpad\aimp-test` altında AIMP'in taşınabilir bir kopyası var (kendi
profiliyle). `$env:APPDATA` test profiline ayarlanıp başlatılırsa log ve ayarlar
kullanıcının kurulumundan tamamen ayrışır. Örnekler ffmpeg
(`C:\ffmpeg\ffmpeg.exe`) ve AIMP'in kendi kodlayıcılarıyla (`enc_mpc`, `enc_tak`)
üretiliyor.

AIMP komut satırından verilen dosya expander'ı **tetiklemiyor**; sürükle-bırak
tetikliyor. Format testleri bu yüzden öz-test üzerinden yapılıyor.

## Açık maddeler (aciliyet yok)

- Çözücü aynı dosya için iki kez `MusicLoad` yapıyor (AIMP iki çözücü
  oluşturuyor). Küçük modüllerde önemsiz, büyük dosyada iki kat bellek.
- Modül çözücümüz arşiv dışındaki `.xm` dosyalarına da karışabilir;
  `IAIMPExtensionAudioDecoderPriority` uygulanmadı.
- `CreateStream` girdinin tamamını belleğe açıyor; büyük dosyalarda maliyetli.
