// Loading the PDF engine. pdfium.dll travels inside VOSStudio.exe as a deflate-compressed RCDATA resource
// (res/vosstudio.rc, built by tools/packres from the checksum-verified third_party/pdfium/win-x64 release). On first
// use it is unpacked to %LOCALAPPDATA%\VOSStudio\pdfium\<build>\pdfium.dll — a per-build folder, so an update never
// overwrites a DLL that another running copy may have mapped — checked against the container CRC and loaded with
// LoadLibrary. The normal Make/CI build requires the resource; a development build without it may use a side-by-side
// pdfium.dll. Everything here runs on the PDF worker thread and reports through PdfEngine.
#include "pdfload.h"

#include <shlobj.h>

#include <cstdio>

#include "../core/inflate.h"
#include "../core/pdfium.h"
#include "version.h"

namespace vs {
namespace win {

namespace {
struct Res { const uint8_t* p = nullptr; size_t n = 0; };
Res resource(const wchar_t* name) {
  Res r;
  HRSRC h = FindResourceW(nullptr, name, RT_RCDATA);
  if (!h) return r;
  HGLOBAL g = LoadResource(nullptr, h);
  if (!g) return r;
  r.p = static_cast<const uint8_t*>(LockResource(g));
  r.n = SizeofResource(nullptr, h);
  return r;
}

string localAppData() {
  wchar_t p[MAX_PATH] = {0};
  if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p) != S_OK) return appDataDir();
  std::wstring d = std::wstring(p) + L"\\VOSStudio";
  CreateDirectoryW(d.c_str(), nullptr);
  return narrow(d);
}

bool fileSize(const string& path, long long& n) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fa)) return false;
  n = ((long long)fa.nFileSizeHigh << 32) | (long long)fa.nFileSizeLow;
  return true;
}

void* resolve(void* ctx, const char* name) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(ctx), name)); }

HMODULE loaded = nullptr;
}  // namespace

string engineThirdPartyNoticesText() {
  Res r = resource(L"ENGINE_LICENSES");
  if (!r.p) return string();
  string out;
  if (!vs::unpackBlob(r.p, r.n, out)) return string();
  return out;
}

PdfEngine pdfEngineLoad() {
  PdfEngine e;
  if (loaded && pdf::bound()) { e.ready = true; e.path = "(loaded)"; return e; }
  string dllPath;
  Res r = resource(L"PDFIUM_DLL");
  uint32_t tag = 0, rawSize = 0;
  bool haveBlob = r.p && vs::blobInfo(r.p, r.n, &tag, &rawSize) && tag == 0x5044464Du /* PDFM */ && rawSize > 0;
  if (haveBlob) {
    e.embedded = true;
    // the CRC stored in the container names the folder: a different DLL never reuses an old file
    uint32_t crc = 0;
    if (r.n >= 4) memcpy(&crc, r.p + r.n - 4, 4);
    char id[64];
    snprintf(id, sizeof id, "%u-%08x", unsigned(rawSize), unsigned(crc));
    string dir = localAppData() + "\\pdfium";
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    dir += string("\\") + id;
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    dllPath = dir + "\\pdfium.dll";
    long long have = 0;
    if (!fileSize(dllPath, have) || have != (long long)rawSize) {
      string raw, err;
      if (!vs::unpackBlob(r.p, r.n, raw, nullptr, &err)) { e.error = "The embedded PDF engine is damaged: " + err; return e; }
      string tmp = dllPath + ".part";
      if (!writeFileU(tmp, raw)) { e.error = "Cannot write " + dllPath; return e; }
      if (!MoveFileExW(widen(tmp).c_str(), widen(dllPath).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // another instance may have won the race; use whatever is there when it has the right size
        DeleteFileW(widen(tmp).c_str());
        if (!fileSize(dllPath, have) || have != (long long)rawSize) { e.error = "Cannot place the PDF engine in " + dir; return e; }
      }
      string lic = engineThirdPartyNoticesText();
      if (!lic.empty()) writeFileU(dir + "\\LICENSES.txt", lic);
      e.unpacked = true;
    }
    e.licensesPath = dir + "\\LICENSES.txt";
  } else {
    dllPath = exeDir() + "\\pdfium.dll";
    long long n = 0;
    if (!fileSize(dllPath, n)) {
      e.error = "This build carries no PDF engine. Put pdfium.dll (https://github.com/bblanchon/pdfium-binaries, win-x64) next to VOSStudio.exe.";
      return e;
    }
  }
  e.path = dllPath;
  HMODULE h = LoadLibraryExW(widen(dllPath).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) {
    char buf[64];
    snprintf(buf, sizeof buf, " (error %lu)", GetLastError());
    e.error = "Could not load the PDF engine from " + dllPath + buf;
    return e;
  }
  string err;
  if (!pdf::bind(&resolve, h, &err)) { FreeLibrary(h); e.error = "The PDF engine is not the expected build: " + err; return e; }
  loaded = h;
  e.ready = true;
  return e;
}

}  // namespace win
}  // namespace vs
