#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <apiCore.h>
#include <apiDecoders.h>
#include <apiFileManager.h>
#include <apiObjects.h>
#include <apiPlugin.h>
#include <apiTypes.h>

#include <miniz.h>

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:AIMPPluginGetHeader=_AIMPPluginGetHeader@4")
#endif

namespace {

constexpr wchar_t kScheme[] = L"zip";
constexpr wchar_t kSchemePrefix[] = L"zip://";
// AIMP konteyner-ici dosyalari "<konteyner>:<parca>" olarak yazar
// (IAIMPServiceFileURI::Build ile dogrulandi). Ayrac ':' olmazsa AIMP URI'yi
// bolemiyor, parca adini bos goruyor ve girdiyi playlist'e almiyor.
constexpr wchar_t kSeparator = L':';

IAIMPCore* g_core = nullptr;

// --- Teshis logu -------------------------------------------------------------
// AIMP'in eklentinin hangi metotlarini cagirdigini gormek icin.
// %AppData%\AIMP\aimp_zip.log

std::wstring AppDataFile(const wchar_t* name) {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return name;
  }
  return std::wstring(buffer) + L"\\AIMP\\" + name;
}

std::wstring LogPath() {
  return AppDataFile(L"aimp_zip.log");
}

// Log varsayilan olarak kapali: acikken her cagride dosya acip kapatiyor ve
// AIMP calarken saniyede birkac kez cagriliyor. Acmak icin
// %AppData%\AIMP\aimp_zip.debug adinda bos bir dosya olustur.
bool LoggingEnabled() {
  static const bool enabled = [] {
    const std::wstring marker = AppDataFile(L"aimp_zip.debug");
    return GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
  }();
  return enabled;
}

void Log(const wchar_t* format, ...) {
  if (!LoggingEnabled()) {
    return;
  }

  static const std::wstring path = LogPath();

  FILE* file = _wfopen(path.c_str(), L"a+, ccs=UTF-8");
  if (!file) {
    return;
  }

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  fwprintf(file, L"[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

  va_list args;
  va_start(args, format);
  vfwprintf(file, format, args);
  va_end(args);

  fwprintf(file, L"\n");
  fclose(file);
}

std::wstring GuidToString(REFIID riid) {
  wchar_t buffer[64] = {};
  swprintf(buffer, 64, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
           riid.Data1, riid.Data2, riid.Data3,
           riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
           riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
  return buffer;
}

template <typename T>
void SafeRelease(T*& value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

IAIMPString* MakeAimpString(const std::wstring& value) {
  if (!g_core) {
    return nullptr;
  }

  IAIMPString* result = nullptr;
  if (Succeeded(g_core->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(&result)))) {
    result->SetData(const_cast<wchar_t*>(value.c_str()), static_cast<INT32>(value.size()));
    return result;
  }
  return nullptr;
}

std::wstring ToWString(IAIMPString* value) {
  if (!value || !value->GetData()) {
    return {};
  }
  return std::wstring(value->GetData(), value->GetLength());
}

std::wstring MultiByteToWide(const char* value, UINT codePage) {
  if (!value) {
    return {};
  }

  const int required = MultiByteToWideChar(codePage, 0, value, -1, nullptr, 0);
  if (required <= 1) {
    return {};
  }

  std::wstring result(static_cast<size_t>(required - 1), L'\0');
  MultiByteToWideChar(codePage, 0, value, -1, result.data(), required);
  return result;
}

std::wstring Utf8ToWide(const char* value) {
  return MultiByteToWide(value, CP_UTF8);
}

// Zip girdi adlari yalnizca "general purpose" bayraginin 11. biti isaretliyse
// UTF-8'dir; aksi halde OEM kod sayfasindadir (TR icin CP857). Hepsini UTF-8
// varsaymak Turkce karakterleri bozuyordu.
constexpr mz_uint kZipUtf8NameFlag = 0x800;

std::wstring ZipEntryNameToWide(const mz_zip_archive_file_stat& stat) {
  const bool isUtf8 = (stat.m_bit_flag & kZipUtf8NameFlag) != 0;
  return MultiByteToWide(stat.m_filename, isUtf8 ? CP_UTF8 : CP_OEMCP);
}


std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1) {
    return {};
  }

  std::string result(static_cast<size_t>(required - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
  return result;
}

bool StartsWithNoCase(const std::wstring& value, const wchar_t* prefix) {
  const size_t prefixLength = wcslen(prefix);
  if (value.size() < prefixLength) {
    return false;
  }

  for (size_t i = 0; i < prefixLength; ++i) {
    if (std::towlower(value[i]) != std::towlower(prefix[i])) {
      return false;
    }
  }
  return true;
}

// AIMP bazen semayi hala takili birakiyor ("zip://C:\...\a.zip"). Bunu duz
// dosya yolu sanip acmaya calisirsak basarisiz oluyoruz.
std::wstring StripScheme(const std::wstring& path) {
  return StartsWithNoCase(path, kSchemePrefix) ? path.substr(wcslen(kSchemePrefix)) : path;
}

std::wstring LowerExt(std::wstring name) {
  const size_t slash = name.find_last_of(L"/\\");
  const size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
    return {};
  }

  std::wstring ext = name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
  });
  return ext;
}

std::wstring StripExt(const std::wstring& name) {
  const size_t slash = name.find_last_of(L"/\\");
  const size_t dot = name.find_last_of(L'.');
  const size_t start = (slash == std::wstring::npos) ? 0 : slash + 1;
  if (dot == std::wstring::npos || dot < start) {
    return name.substr(start);
  }
  return name.substr(start, dot - start);
}


// Yedek liste: AIMP'in format servisine ulasilamazsa kullanilir.
bool LooksLikeAudio(const std::wstring& name) {
  static const wchar_t* kAudioExts[] = {
      L"mp3", L"flac", L"wav", L"ogg", L"oga", L"m4a", L"mp4", L"aac",
      L"wma", L"opus", L"ape", L"mpc", L"tta", L"wv", L"aif", L"aiff", L"alac"};
  const std::wstring ext = LowerExt(name);
  return std::find(std::begin(kAudioExts), std::end(kAudioExts), ext) != std::end(kAudioExts);
}

// Hangi girdinin calinabilir oldugunu sabit listeye gore degil AIMP'e sorarak
// belirliyoruz: boylece kullanicida kurulu olan kod cozuculerin tamami
// (ac3, mka, spx, tak, m4b, mid, mlp, webm, ts ...) otomatik kapsama giriyor
// ve yeni bir kod cozucu eklentisi kurulunca liste kendiliginden genisliyor.
bool IsPlayableAudio(const std::wstring& name) {
  if (!g_core) {
    return LooksLikeAudio(name);
  }

  IAIMPServiceFileFormats* formats = nullptr;
  if (Failed(g_core->QueryInterface(IID_IAIMPServiceFileFormats,
                                    reinterpret_cast<void**>(&formats)))) {
    return LooksLikeAudio(name);
  }

  bool supported = false;
  if (IAIMPString* value = MakeAimpString(name)) {
    supported = Succeeded(formats->IsSupported(value, AIMP_SERVICE_FILEFORMATS_CATEGORY_AUDIO));
    value->Release();
  }
  formats->Release();
  return supported;
}

bool ParseZipUri(const std::wstring& uri, std::wstring& archivePath, std::wstring& entryName) {
  if (!StartsWithNoCase(uri, kSchemePrefix)) {
    return false;
  }

  const std::wstring payload = uri.substr(wcslen(kSchemePrefix));

  // Sondan ara: "C:" surucu iki noktasi ile ayraci karistirmamak icin.
  // Zip girdi adlarinda ':' ve '|' Windows'ta zaten gecersiz oldugu icin
  // sonuncusu her zaman ayractir.
  const size_t sep = payload.find_last_of(L":|");
  if (sep == std::wstring::npos || sep <= 1 || sep + 1 >= payload.size()) {
    return false;
  }

  archivePath = payload.substr(0, sep);
  entryName = payload.substr(sep + 1);
  return true;
}

std::wstring BuildZipUri(const std::wstring& archivePath, const std::wstring& entryName) {
  return std::wstring(kSchemePrefix) + archivePath + kSeparator + entryName;
}

struct ZipHandle {
  FILE* file = nullptr;
  mz_zip_archive zip = {};
  bool initialized = false;

  ~ZipHandle() {
    if (initialized) {
      mz_zip_reader_end(&zip);
    }
    if (file) {
      fclose(file);
    }
  }
};

// Adi yeniden kodlayip mz_zip_reader_locate_file'a vermek yerine girdileri
// gezip cozulmus (wide) adlarla karsilastiriyoruz; boylece kodlama gidis-donusu
// hicbir yerde gerekmiyor.
//
// Her arama icin bastan taramak buyuk arsivlerde kareli maliyet cikariyordu
// (5000 girdilik bir arsivde parca basina 5000 karsilastirma). Bu yuzden ad->
// indeks eslemesini bir kez cikarip onbellege aliyoruz. Onbellek arsivin yolu,
// boyutu ve degistirilme zamaniyla dogrulaniyor; arsiv degisirse kendiliginden
// yeniden kuruluyor.
struct ZipIndexCache {
  std::wstring path;
  INT64 size = -1;
  INT64 writeTime = -1;
  std::unordered_map<std::wstring, int> entries;
};

std::mutex g_indexMutex;
ZipIndexCache g_index;

bool ArchiveStamp(const std::wstring& path, INT64& size, INT64& writeTime) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return false;
  }
  size = (static_cast<INT64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  writeTime = (static_cast<INT64>(data.ftLastWriteTime.dwHighDateTime) << 32) |
              data.ftLastWriteTime.dwLowDateTime;
  return true;
}

int FindZipEntry(mz_zip_archive& zip, const std::wstring& archivePath,
                 const std::wstring& entryName) {
  const std::wstring realPath = StripScheme(archivePath);
  INT64 size = -1;
  INT64 writeTime = -1;
  ArchiveStamp(realPath, size, writeTime);

  std::lock_guard<std::mutex> lock(g_indexMutex);
  if (g_index.path != realPath || g_index.size != size || g_index.writeTime != writeTime) {
    g_index.entries.clear();
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
      mz_zip_archive_file_stat stat = {};
      if (mz_zip_reader_file_stat(&zip, i, &stat)) {
        g_index.entries.emplace(ZipEntryNameToWide(stat), static_cast<int>(i));
      }
    }
    g_index.path = realPath;
    g_index.size = size;
    g_index.writeTime = writeTime;
    Log(L"  indeks kuruldu: %zu girdi", g_index.entries.size());
  }

  const auto found = g_index.entries.find(entryName);
  return found == g_index.entries.end() ? -1 : found->second;
}

bool OpenZip(const std::wstring& archivePath, ZipHandle& handle) {
  handle.file = _wfopen(StripScheme(archivePath).c_str(), L"rb");
  if (!handle.file) {
    return false;
  }

  _fseeki64(handle.file, 0, SEEK_END);
  const auto size = static_cast<mz_uint64>(_ftelli64(handle.file));
  _fseeki64(handle.file, 0, SEEK_SET);

  handle.initialized = mz_zip_reader_init_cfile(&handle.zip, handle.file, size, 0) != 0;
  return handle.initialized;
}

class RefCounted {
 public:
  DWORD WINAPI AddRef() {
    return InterlockedIncrement(&refCount_);
  }

  DWORD WINAPI Release() {
    const DWORD count = InterlockedDecrement(&refCount_);
    if (count == 0) {
      delete this;
    }
    return count;
  }

 protected:
  RefCounted() = default;
  virtual ~RefCounted() = default;

 private:
  volatile LONG refCount_ = 1;
};

class MemoryStream final : public IAIMPStream, public RefCounted {
 public:
  explicit MemoryStream(std::vector<uint8_t> data) : data_(std::move(data)) {}

  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }
    if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPStream)) {
      *out = static_cast<IAIMPStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  INT64 WINAPI GetSize() override { return static_cast<INT64>(data_.size()); }
  HRESULT WINAPI SetSize(const INT64) override { return E_NOTIMPL; }
  INT64 WINAPI GetPosition() override { return position_; }

  HRESULT WINAPI Seek(const INT64 offset, INT32 mode) override {
    INT64 next = 0;
    if (mode == AIMP_STREAM_SEEKMODE_FROM_BEGINNING) {
      next = offset;
    } else if (mode == AIMP_STREAM_SEEKMODE_FROM_CURRENT) {
      next = position_ + offset;
    } else if (mode == AIMP_STREAM_SEEKMODE_FROM_END) {
      next = static_cast<INT64>(data_.size()) + offset;
    } else {
      return E_INVALIDARG;
    }

    if (next < 0) {
      return E_INVALIDARG;
    }
    position_ = std::min<INT64>(next, static_cast<INT64>(data_.size()));
    return S_OK;
  }

  INT32 WINAPI Read(void* buffer, DWORD count) override {
    if (!buffer || count == 0 || position_ >= static_cast<INT64>(data_.size())) {
      return 0;
    }

    const auto available = static_cast<DWORD>(
        std::min<INT64>(count, static_cast<INT64>(data_.size()) - position_));
    memcpy(buffer, data_.data() + position_, available);
    position_ += available;
    return static_cast<INT32>(available);
  }

  HRESULT WINAPI Write(void*, DWORD, DWORD*) override { return E_NOTIMPL; }

 private:
  std::vector<uint8_t> data_;
  INT64 position_ = 0;
};

// Zip girdisini bellege acar.
bool ExtractZipEntry(const std::wstring& archivePath, const std::wstring& entryName,
                     std::vector<uint8_t>& out) {
  ZipHandle handle;
  if (!OpenZip(archivePath, handle)) {
    Log(L"  HATA: arsiv acilamadi \"%s\"", archivePath.c_str());
    return false;
  }

  const int index = FindZipEntry(handle.zip, archivePath, entryName);
  if (index < 0) {
    Log(L"  HATA: girdi bulunamadi \"%s\"", entryName.c_str());
    return false;
  }

  size_t extractedSize = 0;
  void* data = mz_zip_reader_extract_to_heap(&handle.zip, static_cast<mz_uint>(index),
                                             &extractedSize, 0);
  if (!data) {
    Log(L"  HATA: girdi acilamadi \"%s\"", entryName.c_str());
    return false;
  }

  std::unique_ptr<void, decltype(&mz_free)> dataGuard(data, mz_free);
  auto* bytes = static_cast<uint8_t*>(data);
  out.assign(bytes, bytes + extractedSize);
  return true;
}

// Zip girdisini IAIMPStream olarak verir.
// offset/size negatifse girdinin tamami dondurulur.
HRESULT CreateZipEntryStream(const std::wstring& archivePath, const std::wstring& entryName,
                             INT64 offset, INT64 size, IAIMPStream** stream) {
  if (!stream) {
    return E_POINTER;
  }
  *stream = nullptr;

  std::vector<uint8_t> data;
  if (!ExtractZipEntry(archivePath, entryName, data)) {
    return E_FAIL;
  }

  INT64 begin = std::max<INT64>(0, offset);
  INT64 end = static_cast<INT64>(data.size());
  if (offset >= 0 && size >= 0) {
    end = std::min<INT64>(end, begin + size);
  }
  if (begin > end) {
    begin = end;
  }

  *stream = new MemoryStream(std::vector<uint8_t>(data.begin() + begin, data.begin() + end));
  return S_OK;
}

// ID3v2 etiketinin uzunlugu: "ID3" + surum(2) + bayrak(1) + syncsafe boyut(4).
// AIMP ham akista MP3'u taniyamiyor; ilk MPEG cercevesinden baslatmak
// tanimayi mumkun kilabilir.
size_t Id3v2Length(const std::vector<uint8_t>& data) {
  if (data.size() < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') {
    return 0;
  }
  const size_t size = (static_cast<size_t>(data[6] & 0x7F) << 21) |
                      (static_cast<size_t>(data[7] & 0x7F) << 14) |
                      (static_cast<size_t>(data[8] & 0x7F) << 7) |
                      static_cast<size_t>(data[9] & 0x7F);
  const size_t footer = (data[5] & 0x10) ? 10 : 0;
  return std::min(data.size(), 10 + size + footer);
}

bool HasTitle(IAIMPFileInfo* info) {
  IAIMPString* title = nullptr;
  if (Failed(info->GetValueAsObject(AIMP_FILEINFO_PROPID_TITLE, IID_IAIMPString,
                                    reinterpret_cast<void**>(&title))) ||
      !title) {
    return false;
  }
  const bool hasTitle = title->GetLength() > 0;
  title->Release();
  return hasTitle;
}

// AIMP, FileExpander'dan donen listedeki her elemandan IAIMPVirtualFile
// istiyor (logda dogrulandi: liste IAIMPString iken 7 elemanin 7'sine de
// E_NOINTERFACE dondu ve hepsi atildi). Zip icindeki bir parcayi temsil eder.
class ZipVirtualFile final : public IAIMPVirtualFile, public RefCounted {
 public:
  ZipVirtualFile(std::wstring archivePath, std::wstring entryName, INT32 indexInSet)
      : archivePath_(std::move(archivePath)),
        entryName_(std::move(entryName)),
        indexInSet_(indexInSet) {}

  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }
    if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPVirtualFile) ||
        EqualGUID(riid, IID_IAIMPPropertyList)) {
      *out = static_cast<IAIMPVirtualFile*>(this);
      AddRef();
      return S_OK;
    }
    Log(L"    [sanal dosya] istenen arayuz yok: %s", GuidToString(riid).c_str());
    *out = nullptr;
    return E_NOINTERFACE;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  // --- IAIMPPropertyList ---

  void WINAPI BeginUpdate() override {}
  void WINAPI EndUpdate() override {}
  HRESULT WINAPI Reset() override { return S_OK; }

  HRESULT WINAPI GetValueAsFloat(INT32 propertyId, DOUBLE* value) override {
    Log(L"    [sanal dosya] GetValueAsFloat propId=%d", propertyId);
    if (!value) {
      return E_POINTER;
    }
    // Zip girdisi bir ses dosyasinin parcasi degil, tam dosya: kirpma yok.
    if (propertyId == AIMP_VIRTUALFILE_PROPID_CLIPSTART ||
        propertyId == AIMP_VIRTUALFILE_PROPID_CLIPFINISH) {
      *value = 0;
      return S_OK;
    }
    return E_INVALIDARG;
  }

  HRESULT WINAPI GetValueAsInt32(INT32 propertyId, INT32* value) override {
    Log(L"    [sanal dosya] GetValueAsInt32 propId=%d", propertyId);
    if (!value) {
      return E_POINTER;
    }
    if (propertyId == AIMP_VIRTUALFILE_PROPID_INDEXINSET) {
      *value = indexInSet_;
      return S_OK;
    }
    return E_INVALIDARG;
  }

  HRESULT WINAPI GetValueAsInt64(INT32 propertyId, INT64*) override {
    Log(L"    [sanal dosya] GetValueAsInt64 propId=%d", propertyId);
    return E_INVALIDARG;
  }

  HRESULT WINAPI GetValueAsObject(INT32 propertyId, CONSTIID iid, void** value) override {
    Log(L"    [sanal dosya] GetValueAsObject propId=%d iid=%s", propertyId,
        GuidToString(iid).c_str());
    if (!value) {
      return E_POINTER;
    }
    *value = nullptr;
    if (!EqualGUID(iid, IID_IAIMPString)) {
      return E_INVALIDARG;
    }

    switch (propertyId) {
      case AIMP_VIRTUALFILE_PROPID_FILEURI:
        *value = MakeAimpString(BuildZipUri(archivePath_, entryName_));
        break;
      // Fiziksel kaynak dosya: arsivin kendisi.
      // NOT: Tracker modullerinde (xm/it/mod...) buradan gercek bir gecici
      // dosya yolu vermeyi denedim, AIMP kullanmadi - calarken yine zip://
      // URI'sini BASS'a veriyor ve hata 48 (BASS_ERROR_PROTOCOL) aliyor.
      case AIMP_VIRTUALFILE_PROPID_AUDIOSOURCEFILE:
        *value = MakeAimpString(archivePath_);
        break;
      // Kod cozucuyu bu belirliyor. URI'nin uzantisi ".zip" gorundugu icin
      // (AIMP URI'yi konteyner+parca olarak boluyor) girdinin gercek
      // uzantisini burada acikca vermek zorundayiz.
      case AIMP_VIRTUALFILE_PROPID_FILEFORMAT:
        *value = MakeAimpString(L"." + LowerExt(entryName_));
        break;
      default:
        return E_INVALIDARG;
    }
    return *value ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT WINAPI SetValueAsFloat(INT32, const DOUBLE) override { return E_NOTIMPL; }
  HRESULT WINAPI SetValueAsInt32(INT32, INT32) override { return E_NOTIMPL; }
  HRESULT WINAPI SetValueAsInt64(INT32, const INT64) override { return E_NOTIMPL; }
  HRESULT WINAPI SetValueAsObject(INT32, IUnknown*) override { return E_NOTIMPL; }

  // --- IAIMPVirtualFile ---

  HRESULT WINAPI CreateStream(IAIMPStream** stream) override {
    Log(L"    [sanal dosya] CreateStream \"%s\"", entryName_.c_str());
    return CreateZipEntryStream(archivePath_, entryName_, -1, -1, stream);
  }

  HRESULT WINAPI GetFileInfo(IAIMPFileInfo* info) override {
    Log(L"    [sanal dosya] GetFileInfo \"%s\"", entryName_.c_str());
    if (!info) {
      return E_POINTER;
    }

    ZipHandle handle;
    if (!OpenZip(archivePath_, handle)) {
      return E_FAIL;
    }
    const int index = FindZipEntry(handle.zip, archivePath_, entryName_);
    if (index < 0) {
      return E_FAIL;
    }
    mz_zip_archive_file_stat stat = {};
    if (!mz_zip_reader_file_stat(&handle.zip, static_cast<mz_uint>(index), &stat)) {
      return E_FAIL;
    }

    // Ornekleme hizi / bit hizi / kanal / sure ve etiketleri AIMP'in kendi
    // kod cozuculerine okutuyoruz; biz sadece akisi/yolu veriyoruz. Bunu
    // yapmazsak playlist'te "0 kHz, 0 kbps" gorunuyor.
    //
    // Akis yolu WAV'da calisiyor ama MP3'te E_FAIL donuyor: AIMP ham akistan
    // MP3'u taniyamiyor. Bu yuzden birkac yolu sirayla deniyoruz.
    IAIMPServiceFileInfo* service = nullptr;
    if (g_core && Succeeded(g_core->QueryInterface(IID_IAIMPServiceFileInfo,
                                                   reinterpret_cast<void**>(&service)))) {
      std::vector<uint8_t> data;
      ExtractZipEntry(archivePath_, entryName_, data);
      HRESULT hr = E_FAIL;

      // A) Ham akis. wav/flac/aac/alac/ogg/opus/wavpack'te calisiyor,
      //    MP3'te E_FAIL donuyor.
      if (!data.empty()) {
        auto* stream = new MemoryStream(data);
        hr = service->GetFileInfoFromStream(stream, 0, info);
        Log(L"      [A] ham akis        = 0x%08lX", static_cast<unsigned long>(hr));
        stream->Release();
      }

      // B) ID3v2 etiketini atlayip ilk MPEG cercevesinden basla.
      const size_t id3 = Id3v2Length(data);
      if (Failed(hr) && id3 > 0 && id3 < data.size()) {
        auto* stream = new MemoryStream(std::vector<uint8_t>(data.begin() + id3, data.end()));
        hr = service->GetFileInfoFromStream(stream, 0, info);
        Log(L"      [B] ID3v2 atlanmis  = 0x%08lX (%zu bayt atlandi)",
            static_cast<unsigned long>(hr), id3);
        stream->Release();
      }

      // C) Dogru uzantili gecici dosya: AIMP'in normal dosya yolu.
      if (Failed(hr) && !data.empty()) {
        hr = InfoFromTempFile(service, info, data);
      }

      SafeRelease(service);
    }

    info->SetValueAsInt64(AIMP_FILEINFO_PROPID_FILESIZE, static_cast<INT64>(stat.m_uncomp_size));

    IAIMPString* name = MakeAimpString(BuildZipUri(archivePath_, entryName_));
    if (name) {
      info->SetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, name);
      name->Release();
    }

    // Etiketten baslik gelmediyse (ornegin etiketsiz wav) AIMP dosya adina
    // duser, o da URI'yi konteyner+parca diye bolup konteyner adini
    // ("TURKCE") gosterir. Girdi adini acikca veriyoruz.
    if (!HasTitle(info)) {
      IAIMPString* title = MakeAimpString(StripExt(entryName_));
      if (title) {
        info->SetValueAsObject(AIMP_FILEINFO_PROPID_TITLE, title);
        title->Release();
      }
    }

    return S_OK;
  }

  // DIKKAT: Kendi zip:// URI'mizi GetFileInfoFromFileURI'ye vermek sonsuz
  // ozyineleme yapiyor (AIMP dosya sistemimize donuyor, o da buraya).
  // Bu yuzden gecici dosyayi gercek bir yol olarak yaziyoruz.
  HRESULT InfoFromTempFile(IAIMPServiceFileInfo* service, IAIMPFileInfo* info,
                           const std::vector<uint8_t>& data) {
    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) {
      return E_FAIL;
    }

    const std::wstring path =
        std::wstring(tempDir) + L"aimp_zip_" + std::to_wstring(GetCurrentThreadId()) + L"_" +
        std::to_wstring(GetTickCount64()) + L"." + LowerExt(entryName_);

    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) {
      return E_FAIL;
    }
    fwrite(data.data(), 1, data.size(), file);
    fclose(file);

    HRESULT hr = E_FAIL;
    IAIMPString* pathString = MakeAimpString(path);
    if (pathString) {
      hr = service->GetFileInfoFromFileURI(pathString, 0, info);
      pathString->Release();
    }
    Log(L"      [C] gecici dosya    = 0x%08lX", static_cast<unsigned long>(hr));
    _wremove(path.c_str());
    return hr;
  }

  HRESULT WINAPI IsExists() override {
    ZipHandle handle;
    if (!OpenZip(archivePath_, handle)) {
      return E_FAIL;
    }
    return FindZipEntry(handle.zip, archivePath_, entryName_) >= 0 ? S_OK : E_FAIL;
  }

  // Her girdi bagimsiz olarak aciliyor; cue-sheet gibi tek akisi paylasmiyorlar.
  HRESULT WINAPI IsInSameStream(IAIMPVirtualFile*) override { return E_FAIL; }

  HRESULT WINAPI Synchronize() override { return S_OK; }


 private:
  std::wstring archivePath_;
  std::wstring entryName_;

  INT32 indexInSet_ = 0;
};

// --- Tracker modulu cozucusu -------------------------------------------------
//
// AIMP modul formatlarini (xm/it/mod/s3m...) BASS'a URL uzerinden devrediyor;
// zip:// semasini BASS tanimadigi icin hata 48 (BASS_ERROR_PROTOCOL) aliniyordu
// ve arsiv icindeki moduller calmiyordu. AIMP_VIRTUALFILE_PROPID_AUDIOSOURCEFILE
// ile gercek bir dosya yolu sunmak da ise yaramadi (denendi, AIMP kullanmiyor).
//
// Cozum: kendi ses cozucumuzu kaydetmek. IAIMPExtensionAudioDecoder'a AIMP URI
// degil dogrudan IAIMPStream veriyor; akisi bellege alip BASS_MusicLoad'a
// mem=TRUE ile veriyoruz. Boylece URL yolu hic devreye girmiyor.
//
// bass.dll AIMP'in kendi klasorunde ve surecte zaten yuklu; yanimizda
// tasimiyoruz, sadece isaretcilerini aliyoruz.

using HMUSIC = DWORD;

struct BassChannelInfo {
  DWORD freq;
  DWORD chans;
  DWORD flags;
  DWORD ctype;
  DWORD origres;
  DWORD plugin;
  DWORD sample;
  const char* filename;
};

constexpr DWORD kBassSampleFloat = 0x100;
constexpr DWORD kBassStreamDecode = 0x200000;
constexpr DWORD kBassStreamPrescan = 0x20000;
constexpr DWORD kBassPosByte = 0;
constexpr DWORD kBassMusicRamps = 0x400;

struct BassApi {
  HMUSIC(WINAPI* MusicLoad)
  (BOOL mem, const void* file, UINT64 offset, DWORD length, DWORD flags, DWORD freq) = nullptr;
  BOOL(WINAPI* MusicFree)(HMUSIC handle) = nullptr;
  BOOL(WINAPI* ChannelGetInfo)(DWORD handle, BassChannelInfo* info) = nullptr;
  UINT64(WINAPI* ChannelGetLength)(DWORD handle, DWORD mode) = nullptr;
  UINT64(WINAPI* ChannelGetPosition)(DWORD handle, DWORD mode) = nullptr;
  BOOL(WINAPI* ChannelSetPosition)(DWORD handle, UINT64 pos, DWORD mode) = nullptr;
  DWORD(WINAPI* ChannelGetData)(DWORD handle, void* buffer, DWORD length) = nullptr;
  const char*(WINAPI* ChannelGetTags)(DWORD handle, DWORD tags) = nullptr;
  double(WINAPI* ChannelBytes2Seconds)(DWORD handle, UINT64 pos) = nullptr;
  int(WINAPI* ErrorGetCode)() = nullptr;
  bool ready = false;
};

constexpr DWORD kBassTagMusicName = 0x10000;  // ANSI
constexpr DWORD kBassTagMusicAuth = 0x10003;  // UTF-8

const BassApi& Bass() {
  static const BassApi api = [] {
    BassApi result;
    // AIMP zaten yuklemis olmali; degilse kendi klasorunden yukle.
    HMODULE module = GetModuleHandleW(L"bass.dll");
    if (!module) {
      module = LoadLibraryW(L"bass.dll");
    }
    if (!module) {
      Log(L"BASS: bass.dll bulunamadi");
      return result;
    }

    auto bind = [module](const char* name) { return GetProcAddress(module, name); };
    result.MusicLoad = reinterpret_cast<decltype(result.MusicLoad)>(bind("BASS_MusicLoad"));
    result.MusicFree = reinterpret_cast<decltype(result.MusicFree)>(bind("BASS_MusicFree"));
    result.ChannelGetInfo =
        reinterpret_cast<decltype(result.ChannelGetInfo)>(bind("BASS_ChannelGetInfo"));
    result.ChannelGetLength =
        reinterpret_cast<decltype(result.ChannelGetLength)>(bind("BASS_ChannelGetLength"));
    result.ChannelGetPosition =
        reinterpret_cast<decltype(result.ChannelGetPosition)>(bind("BASS_ChannelGetPosition"));
    result.ChannelSetPosition =
        reinterpret_cast<decltype(result.ChannelSetPosition)>(bind("BASS_ChannelSetPosition"));
    result.ChannelGetData =
        reinterpret_cast<decltype(result.ChannelGetData)>(bind("BASS_ChannelGetData"));
    result.ChannelGetTags =
        reinterpret_cast<decltype(result.ChannelGetTags)>(bind("BASS_ChannelGetTags"));
    result.ChannelBytes2Seconds =
        reinterpret_cast<decltype(result.ChannelBytes2Seconds)>(bind("BASS_ChannelBytes2Seconds"));
    result.ErrorGetCode = reinterpret_cast<decltype(result.ErrorGetCode)>(bind("BASS_ErrorGetCode"));

    result.ready = result.MusicLoad && result.MusicFree && result.ChannelGetInfo &&
                   result.ChannelGetLength && result.ChannelGetPosition &&
                   result.ChannelSetPosition && result.ChannelGetData;
    Log(L"BASS: baglandi=%d", result.ready ? 1 : 0);
    return result;
  }();
  return api;
}

class ModuleDecoder final : public IAIMPAudioDecoder, public RefCounted {
 public:
  ModuleDecoder(HMUSIC handle, std::vector<uint8_t> data)
      : handle_(handle), data_(std::move(data)) {}

  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }
    if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPAudioDecoder)) {
      *out = static_cast<IAIMPAudioDecoder*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  // Calma basladiginda AIMP bilgiyi cozucuden tazeliyor. Burasi bos kalirsa
  // playlist'teki baslik silinip dosya adina duruyor - modulun ic adini
  // BASS'tan okuyup geri veriyoruz.
  BOOL WINAPI GetFileInfo(IAIMPFileInfo* info) override {
    if (!info) {
      return FALSE;
    }

    BassChannelInfo channel = {};
    if (Bass().ChannelGetInfo(handle_, &channel)) {
      info->SetValueAsInt32(AIMP_FILEINFO_PROPID_SAMPLERATE, static_cast<INT32>(channel.freq));
      info->SetValueAsInt32(AIMP_FILEINFO_PROPID_CHANNELS, static_cast<INT32>(channel.chans));
    }

    if (Bass().ChannelBytes2Seconds) {
      const double duration =
          Bass().ChannelBytes2Seconds(handle_, Bass().ChannelGetLength(handle_, kBassPosByte));
      if (duration > 0) {
        info->SetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, duration);
      }
    }

    if (Bass().ChannelGetTags) {
      SetTag(info, AIMP_FILEINFO_PROPID_TITLE, kBassTagMusicName, CP_ACP);
      SetTag(info, AIMP_FILEINFO_PROPID_ARTIST, kBassTagMusicAuth, CP_UTF8);
    }
    return TRUE;
  }

  BOOL WINAPI GetStreamInfo(INT32* sampleRate, INT32* channels, INT32* sampleFormat) override {
    BassChannelInfo info = {};
    if (!Bass().ChannelGetInfo(handle_, &info)) {
      return FALSE;
    }
    if (sampleRate) {
      *sampleRate = static_cast<INT32>(info.freq);
    }
    if (channels) {
      *channels = static_cast<INT32>(info.chans);
    }
    if (sampleFormat) {
      // BASS_MUSIC_DECODE'u float ile actik.
      *sampleFormat = AIMP_DECODER_SAMPLEFORMAT_32BITFLOAT;
    }
    return TRUE;
  }

  BOOL WINAPI IsSeekable() override { return TRUE; }
  BOOL WINAPI IsRealTimeStream() override { return FALSE; }
  INT64 WINAPI GetAvailableData() override { return GetSize() - GetPosition(); }

  INT64 WINAPI GetSize() override {
    return static_cast<INT64>(Bass().ChannelGetLength(handle_, kBassPosByte));
  }

  INT64 WINAPI GetPosition() override {
    return static_cast<INT64>(Bass().ChannelGetPosition(handle_, kBassPosByte));
  }

  BOOL WINAPI SetPosition(const INT64 value) override {
    return Bass().ChannelSetPosition(handle_, static_cast<UINT64>(value), kBassPosByte);
  }

  INT32 WINAPI Read(void* buffer, INT32 count) override {
    if (!buffer || count <= 0) {
      return 0;
    }
    const DWORD read = Bass().ChannelGetData(handle_, buffer, static_cast<DWORD>(count));
    if (!logged_) {
      logged_ = true;
      Log(L"ModuleDecoder: ilk Read %d istendi -> %lu bayt cozuldu", count,
          read == static_cast<DWORD>(-1) ? 0UL : read);
    }
    return read == static_cast<DWORD>(-1) ? 0 : static_cast<INT32>(read);
  }

 protected:
  ~ModuleDecoder() override {
    if (handle_) {
      Bass().MusicFree(handle_);
    }
  }

 private:
  // Bos etiketi yazmiyoruz: yazarsak AIMP'in ZipVirtualFile'dan aldigi
  // (girdi adindan turetilmis) basligi silmis oluruz.
  void SetTag(IAIMPFileInfo* info, INT32 propertyId, DWORD tagId, UINT codePage) {
    const char* raw = Bass().ChannelGetTags(handle_, tagId);
    if (!raw) {
      return;
    }
    std::wstring value = MultiByteToWide(raw, codePage);
    while (!value.empty() && std::iswspace(value.back())) {
      value.pop_back();
    }
    if (value.empty()) {
      return;
    }
    Log(L"ModuleDecoder: etiket propId=%d -> \"%s\"", propertyId, value.c_str());
    if (IAIMPString* text = MakeAimpString(value)) {
      info->SetValueAsObject(propertyId, text);
      text->Release();
    }
  }

 private:
  HMUSIC handle_ = 0;
  bool logged_ = false;
  // BASS bellekten yuklerken veriyi kopyalamayabiliyor; tamponu kanal
  // yasadigi surece elimizde tutuyoruz.
  std::vector<uint8_t> data_;
};

class ModuleDecoderExtension final : public IAIMPExtensionAudioDecoder, public RefCounted {
 public:
  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }
    if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPExtensionAudioDecoder)) {
      *out = static_cast<IAIMPExtensionAudioDecoder*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  HRESULT WINAPI CreateDecoder(IAIMPStream* stream, DWORD, IAIMPErrorInfo*,
                               IAIMPAudioDecoder** decoder) override {
    if (!stream || !decoder) {
      return E_POINTER;
    }
    *decoder = nullptr;

    if (!Bass().ready) {
      return E_FAIL;
    }

    const INT64 size = stream->GetSize();
    if (size <= 0 || size > 128 * 1024 * 1024) {
      return E_FAIL;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    stream->Seek(0, AIMP_STREAM_SEEKMODE_FROM_BEGINNING);
    INT64 total = 0;
    while (total < size) {
      const INT32 read = stream->Read(data.data() + total, static_cast<DWORD>(size - total));
      if (read <= 0) {
        break;
      }
      total += read;
    }
    if (total != size) {
      return E_FAIL;
    }

    // Modul degilse MusicLoad basarisiz olur ve AIMP diger cozuculere gecer.
    const HMUSIC handle = Bass().MusicLoad(
        TRUE, data.data(), 0, static_cast<DWORD>(data.size()),
        kBassStreamDecode | kBassStreamPrescan | kBassSampleFloat | kBassMusicRamps, 0);
    if (!handle) {
      return E_FAIL;
    }

    Log(L"ModuleDecoder: BASS_MusicLoad basarili (%lld bayt)", size);
    *decoder = new ModuleDecoder(handle, std::move(data));
    return S_OK;
  }
};

class ZipFileFormat final : public IAIMPExtensionFileFormat, public RefCounted {
 public:
  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }
    if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPExtensionFileFormat)) {
      *out = static_cast<IAIMPExtensionFileFormat*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  HRESULT WINAPI GetDescription(IAIMPString** s) override {
    Log(L"ZipFileFormat::GetDescription");
    if (!s) {
      return E_POINTER;
    }
    *s = MakeAimpString(L"ZIP archive");
    return *s ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT WINAPI GetExtList(IAIMPString** s) override {
    Log(L"ZipFileFormat::GetExtList");
    if (!s) {
      return E_POINTER;
    }
    *s = MakeAimpString(L"*.zip;");
    return *s ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT WINAPI GetFlags(DWORD* flags) override {
    Log(L"ZipFileFormat::GetFlags");
    if (!flags) {
      return E_POINTER;
    }
    *flags = AIMP_SERVICE_FILEFORMATS_CATEGORY_PLAYLISTS;
    return S_OK;
  }
};

class ZipExpander final : public IAIMPExtensionFileExpander, public RefCounted {
 public:
  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }
    if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPExtensionFileExpander)) {
      *out = static_cast<IAIMPExtensionFileExpander*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  HRESULT WINAPI Expand(IAIMPString* fileName, IAIMPObjectList** list, IAIMPProgressCallback* progress) override {
    if (!fileName || !list) {
      return E_POINTER;
    }

    *list = nullptr;
    const std::wstring archivePath = StripScheme(ToWString(fileName));
    Log(L"ZipExpander::Expand -> \"%s\"", archivePath.c_str());
    if (LowerExt(archivePath) != L"zip") {
      Log(L"  reddedildi: uzanti zip degil");
      return E_INVALIDARG;
    }

    ZipHandle handle;
    if (!OpenZip(archivePath, handle)) {
      Log(L"  HATA: arsiv acilamadi");
      return E_FAIL;
    }

    IAIMPObjectList* result = nullptr;
    if (Failed(g_core->CreateObject(IID_IAIMPObjectList, reinterpret_cast<void**>(&result)))) {
      return E_FAIL;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&handle.zip);
    for (mz_uint i = 0; i < count; ++i) {
      mz_zip_archive_file_stat stat = {};
      if (!mz_zip_reader_file_stat(&handle.zip, i, &stat) || mz_zip_reader_is_file_a_directory(&handle.zip, i)) {
        continue;
      }

      const std::wstring entryName = ZipEntryNameToWide(stat);
      if (!IsPlayableAudio(entryName)) {
        continue;
      }

      auto* virtualFile = new ZipVirtualFile(archivePath, entryName, result->GetCount());
      result->Add(static_cast<IAIMPVirtualFile*>(virtualFile));
      virtualFile->Release();

      if (progress) {
        BOOL canceled = FALSE;
        progress->Process(count ? static_cast<float>(i + 1) / static_cast<float>(count) : 1.0f, &canceled);
        if (canceled) {
          result->Release();
          return E_ABORT;
        }
      }
    }

    Log(L"  tamam: %d girdi eklendi (arsivde %u dosya)", result->GetCount(), count);

    *list = result;
    return S_OK;
  }
};

class ZipFileSystem final : public IAIMPExtensionFileSystem,
                            public IAIMPFileSystemCommandStreaming,
                            public IAIMPFileSystemCommandDropSource,
                            public IAIMPFileSystemCommandFileInfo,
                            public RefCounted {
 public:
  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }

    Log(L"ZipFileSystem::QueryInterface %s", GuidToString(riid).c_str());

    if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPExtensionFileSystem) ||
        EqualGUID(riid, IID_IAIMPPropertyList)) {
      *out = static_cast<IAIMPExtensionFileSystem*>(this);
    } else if (EqualGUID(riid, IID_IAIMPFileSystemCommandStreaming)) {
      *out = static_cast<IAIMPFileSystemCommandStreaming*>(this);
    } else if (EqualGUID(riid, IID_IAIMPFileSystemCommandDropSource)) {
      *out = static_cast<IAIMPFileSystemCommandDropSource*>(this);
    } else if (EqualGUID(riid, IID_IAIMPFileSystemCommandFileInfo)) {
      *out = static_cast<IAIMPFileSystemCommandFileInfo*>(this);
    } else {
      Log(L"  -> E_NOINTERFACE");
      *out = nullptr;
      return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  void WINAPI BeginUpdate() override {}
  void WINAPI EndUpdate() override {}
  HRESULT WINAPI Reset() override { return S_OK; }
  HRESULT WINAPI GetValueAsFloat(INT32, DOUBLE*) override { return E_INVALIDARG; }
  HRESULT WINAPI GetValueAsInt64(INT32, INT64*) override { return E_INVALIDARG; }
  HRESULT WINAPI SetValueAsFloat(INT32, const DOUBLE) override { return E_NOTIMPL; }
  HRESULT WINAPI SetValueAsInt32(INT32, INT32) override { return E_NOTIMPL; }
  HRESULT WINAPI SetValueAsInt64(INT32, const INT64) override { return E_NOTIMPL; }
  HRESULT WINAPI SetValueAsObject(INT32, IUnknown*) override { return E_NOTIMPL; }

  HRESULT WINAPI GetValueAsInt32(INT32 propertyId, INT32* value) override {
    Log(L"ZipFileSystem::GetValueAsInt32 propId=%d", propertyId);
    if (!value) {
      return E_POINTER;
    }
    if (propertyId == AIMP_FILESYSTEM_PROPID_READONLY) {
      *value = 1;
      return S_OK;
    }
    return E_INVALIDARG;
  }

  HRESULT WINAPI GetValueAsObject(INT32 propertyId, CONSTIID iid, void** value) override {
    Log(L"ZipFileSystem::GetValueAsObject propId=%d iid=%s", propertyId, GuidToString(iid).c_str());
    if (!value) {
      return E_POINTER;
    }
    *value = nullptr;
    if (propertyId != AIMP_FILESYSTEM_PROPID_SCHEME || !EqualGUID(iid, IID_IAIMPString)) {
      return E_INVALIDARG;
    }
    *value = MakeAimpString(kScheme);
    return *value ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT WINAPI CreateStream(IAIMPString* fileName, IAIMPStream** stream) override {
    return CreateZipStream(fileName, -1, -1, stream);
  }

  HRESULT WINAPI CreateStream(IAIMPString* fileName, const INT64 offset, const INT64 size, DWORD, IAIMPStream** stream) override {
    return CreateZipStream(fileName, offset, size, stream);
  }

  HRESULT WINAPI GetFileAttrs(IAIMPString* fileName, TAIMPFileAttributes* attrs) override {
    if (!attrs) {
      return E_POINTER;
    }

    INT64 size = 0;
    const HRESULT hr = GetFileSize(fileName, &size);
    if (Failed(hr)) {
      return hr;
    }

    *attrs = {};
    attrs->Attributes = FILE_ATTRIBUTE_READONLY;
    return S_OK;
  }

  HRESULT WINAPI GetFileSize(IAIMPString* fileName, INT64* size) override {
    if (!fileName || !size) {
      return E_POINTER;
    }

    std::wstring archivePath;
    std::wstring entryName;
    Log(L"ZipFileSystem::GetFileSize \"%s\"", ToWString(fileName).c_str());
    if (!ParseZipUri(ToWString(fileName), archivePath, entryName)) {
      return E_INVALIDARG;
    }

    ZipHandle handle;
    if (!OpenZip(archivePath, handle)) {
      return E_FAIL;
    }

    const int index = FindZipEntry(handle.zip, archivePath, entryName);
    if (index < 0) {
      Log(L"  HATA: girdi bulunamadi \"%s\"", entryName.c_str());
      return E_FAIL;
    }

    mz_zip_archive_file_stat stat = {};
    if (!mz_zip_reader_file_stat(&handle.zip, static_cast<mz_uint>(index), &stat)) {
      return E_FAIL;
    }

    *size = static_cast<INT64>(stat.m_uncomp_size);
    return S_OK;
  }

  HRESULT WINAPI IsFileExists(IAIMPString* fileName) override {
    INT64 ignored = 0;
    return GetFileSize(fileName, &ignored);
  }

 private:
  HRESULT CreateZipStream(IAIMPString* fileName, INT64 offset, INT64 size, IAIMPStream** stream) {
    if (!fileName || !stream) {
      return E_POINTER;
    }
    *stream = nullptr;

    std::wstring archivePath;
    std::wstring entryName;
    Log(L"ZipFileSystem::CreateStream \"%s\" offset=%lld size=%lld",
        ToWString(fileName).c_str(), offset, size);
    if (!ParseZipUri(ToWString(fileName), archivePath, entryName)) {
      Log(L"  HATA: URI ayristirilamadi");
      return E_INVALIDARG;
    }

    ZipHandle handle;
    if (!OpenZip(archivePath, handle)) {
      Log(L"  HATA: arsiv acilamadi \"%s\"", archivePath.c_str());
      return E_FAIL;
    }

    const int index = FindZipEntry(handle.zip, archivePath, entryName);
    if (index < 0) {
      Log(L"  HATA: girdi bulunamadi \"%s\"", entryName.c_str());
      return E_FAIL;
    }

    size_t extractedSize = 0;
    void* data = mz_zip_reader_extract_to_heap(&handle.zip, static_cast<mz_uint>(index), &extractedSize, 0);
    if (!data) {
      return E_FAIL;
    }

    std::unique_ptr<void, decltype(&mz_free)> dataGuard(data, mz_free);
    auto* bytes = static_cast<uint8_t*>(data);
    INT64 begin = std::max<INT64>(0, offset);
    INT64 end = static_cast<INT64>(extractedSize);
    if (offset >= 0 && size >= 0) {
      end = std::min<INT64>(end, begin + size);
    }
    if (begin > end) {
      begin = end;
    }

    std::vector<uint8_t> buffer(bytes + begin, bytes + end);
    *stream = new MemoryStream(std::move(buffer));
    return S_OK;
  }
};

// --- Oz-test ----------------------------------------------------------------
// %AppData%\AIMP\aimp_zip.selftest dosyasi varsa, icindeki yolu (UTF-8, tek
// satir) arsiv kabul edip her ses girdisi icin GetFileInfo'yu calistirir ve
// sonucu loglar. Boylece format davranisini olcmek icin AIMP arayuzunde
// surukle-birak yapmak gerekmiyor.

std::wstring ReadTextFile(const std::wstring& path) {
  FILE* file = _wfopen(path.c_str(), L"rb");
  if (!file) {
    return {};
  }
  std::string bytes;
  char buffer[512];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    bytes.append(buffer, read);
  }
  fclose(file);

  if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) {
    bytes.erase(0, 3);
  }
  const size_t end = bytes.find_first_of("\r\n");
  if (end != std::string::npos) {
    bytes.erase(end);
  }
  return Utf8ToWide(bytes.c_str());
}

std::wstring InfoText(IAIMPFileInfo* info, INT32 propertyId) {
  IAIMPString* value = nullptr;
  if (Failed(info->GetValueAsObject(propertyId, IID_IAIMPString,
                                    reinterpret_cast<void**>(&value))) ||
      !value) {
    return L"-";
  }
  std::wstring result = ToWString(value);
  value->Release();
  return result.empty() ? L"-" : result;
}

INT32 InfoInt(IAIMPFileInfo* info, INT32 propertyId) {
  INT32 value = 0;
  return Succeeded(info->GetValueAsInt32(propertyId, &value)) ? value : 0;
}

void RunSelfTest() {
  const std::wstring configPath = AppDataFile(L"aimp_zip.selftest");
  if (GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return;
  }

  const std::wstring archivePath = ReadTextFile(configPath);
  Log(L"===== OZ-TEST: \"%s\" =====", archivePath.c_str());

  // IsSupported gercekten ayirt ediyor mu? Ses olmayan adlar da "destekleniyor"
  // donerse IsPlayableAudio her seyi listeye alir.
  Log(L"--- IsSupported kontrolu ---");
  for (const wchar_t* name : {L"x.mp3", L"x.ac3", L"x.mid", L"x.zip", L"x.jpg", L"x.txt",
                              L"x.m3u", L"x.nfo", L"x.exe"}) {
    Log(L"  %-8s ses=%d", name, IsPlayableAudio(name) ? 1 : 0);
  }

  ZipHandle handle;
  if (!OpenZip(archivePath, handle)) {
    Log(L"  HATA: arsiv acilamadi");
    return;
  }

  const mz_uint count = mz_zip_reader_get_num_files(&handle.zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat = {};
    if (!mz_zip_reader_file_stat(&handle.zip, i, &stat) ||
        mz_zip_reader_is_file_a_directory(&handle.zip, i)) {
      continue;
    }
    const std::wstring entryName = ZipEntryNameToWide(stat);
    // Oz-testte eleme yapmiyoruz: IsSupported "hayir" dese bile AIMP dosyayi
    // okuyabiliyor mu, onu gormek istiyoruz.
    const bool claimed = IsPlayableAudio(entryName);

    IAIMPFileInfo* info = nullptr;
    if (Failed(g_core->CreateObject(IID_IAIMPFileInfo, reinterpret_cast<void**>(&info)))) {
      continue;
    }

    auto* file = new ZipVirtualFile(archivePath, entryName, static_cast<INT32>(i));
    const HRESULT hr = file->GetFileInfo(info);

    DOUBLE duration = 0;
    info->GetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, &duration);

    Log(L"  >>> %-16s IsSupported=%d hr=0x%08lX  %d Hz  %d kbps  %d kanal  %.2f sn  baslik=\"%s\"",
        entryName.c_str(), claimed ? 1 : 0, static_cast<unsigned long>(hr),
        InfoInt(info, AIMP_FILEINFO_PROPID_SAMPLERATE),
        InfoInt(info, AIMP_FILEINFO_PROPID_BITRATE),
        InfoInt(info, AIMP_FILEINFO_PROPID_CHANNELS), duration,
        InfoText(info, AIMP_FILEINFO_PROPID_TITLE).c_str());

    file->Release();
    info->Release();
  }

  Log(L"===== OZ-TEST SONU =====");
}

class ZipPlugin final : public IAIMPPlugin, public RefCounted {
 public:
  HRESULT WINAPI QueryInterface(REFIID riid, LPVOID* out) override {
    if (!out) {
      return E_POINTER;
    }
    if (EqualGUID(riid, IID_IUnknown)) {
      *out = static_cast<IAIMPPlugin*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  DWORD WINAPI AddRef() override { return RefCounted::AddRef(); }
  DWORD WINAPI Release() override { return RefCounted::Release(); }

  PChar WINAPI InfoGet(INT32 index) override {
    switch (index) {
      case AIMP_PLUGIN_INFO_NAME:
        return const_cast<PChar>(L"ZIP Archive Reader");
      case AIMP_PLUGIN_INFO_AUTHOR:
        return const_cast<PChar>(L"AIMP-Zip");
      case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:
        return const_cast<PChar>(L"Reads audio files from ZIP archives");
      case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:
        return const_cast<PChar>(L"Expands .zip archives and streams supported audio entries to AIMP.");
      default:
        return const_cast<PChar>(L"");
    }
  }

  DWORD WINAPI InfoGetCategories() override {
    return AIMP_PLUGIN_CATEGORY_ADDONS;
  }

  HRESULT WINAPI Initialize(IAIMPCore* core) override {
    if (!core) {
      return E_POINTER;
    }

    Log(L"===== ZipPlugin::Initialize =====");

    g_core = core;
    g_core->AddRef();

    fileSystem_ = new ZipFileSystem();
    expander_ = new ZipExpander();
    format_ = new ZipFileFormat();

    const HRESULT fsResult = g_core->RegisterExtension(
        IID_IAIMPServiceFileSystems, static_cast<IAIMPExtensionFileSystem*>(fileSystem_));
    Log(L"RegisterExtension(FileSystems)  = 0x%08lX", static_cast<unsigned long>(fsResult));

    const HRESULT expResult = g_core->RegisterExtension(
        IID_IAIMPServiceFileManager, static_cast<IAIMPExtensionFileExpander*>(expander_));
    Log(L"RegisterExtension(FileManager)  = 0x%08lX", static_cast<unsigned long>(expResult));

    const HRESULT fmtResult = g_core->RegisterExtension(
        IID_IAIMPServiceFileFormats, static_cast<IAIMPExtensionFileFormat*>(format_));
    Log(L"RegisterExtension(FileFormats)  = 0x%08lX", static_cast<unsigned long>(fmtResult));

    moduleDecoder_ = new ModuleDecoderExtension();
    const HRESULT decResult = g_core->RegisterExtension(
        IID_IAIMPServiceAudioDecoders, static_cast<IAIMPExtensionAudioDecoder*>(moduleDecoder_));
    Log(L"RegisterExtension(AudioDecoders)= 0x%08lX", static_cast<unsigned long>(decResult));

    // Servis gercekten var mi, ayri kontrol et.
    for (const auto& entry : {
             std::pair<const wchar_t*, GUID>{L"ServiceFileSystems", IID_IAIMPServiceFileSystems},
             std::pair<const wchar_t*, GUID>{L"ServiceFileManager", IID_IAIMPServiceFileManager},
             std::pair<const wchar_t*, GUID>{L"ServiceFileFormats", IID_IAIMPServiceFileFormats},
         }) {
      IUnknown* service = nullptr;
      const HRESULT hr = g_core->QueryInterface(entry.second, reinterpret_cast<void**>(&service));
      Log(L"QueryInterface(%s) = 0x%08lX %s", entry.first, static_cast<unsigned long>(hr),
          service ? L"(var)" : L"(YOK)");
      SafeRelease(service);
    }

    Log(L"Initialize tamam");
    RunSelfTest();
    return Failed(fsResult) ? fsResult : S_OK;
  }

  HRESULT WINAPI Finalize() override {
    if (g_core) {
      if (fileSystem_) {
        g_core->UnregisterExtension(static_cast<IAIMPExtensionFileSystem*>(fileSystem_));
      }
      if (expander_) {
        g_core->UnregisterExtension(static_cast<IAIMPExtensionFileExpander*>(expander_));
      }
      if (format_) {
        g_core->UnregisterExtension(static_cast<IAIMPExtensionFileFormat*>(format_));
      }
      if (moduleDecoder_) {
        g_core->UnregisterExtension(static_cast<IAIMPExtensionAudioDecoder*>(moduleDecoder_));
      }
    }

    SafeRelease(fileSystem_);
    SafeRelease(expander_);
    SafeRelease(format_);
    SafeRelease(moduleDecoder_);
    SafeRelease(g_core);
    return S_OK;
  }

  void WINAPI SystemNotification(INT32, IUnknown*) override {}

 private:
  ZipFileSystem* fileSystem_ = nullptr;
  ZipExpander* expander_ = nullptr;
  ZipFileFormat* format_ = nullptr;
  ModuleDecoderExtension* moduleDecoder_ = nullptr;
};

}  // namespace

extern "C" EXPORT HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
  if (!header) {
    return E_POINTER;
  }

  Log(L"AIMPPluginGetHeader cagrildi (%zu-bit)", sizeof(void*) * 8);
  *header = new ZipPlugin();
  return S_OK;
}
