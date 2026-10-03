// Tectonic is shipped as a compressed RCDATA resource, not a separate download or PATH dependency. Extract the
// checksum-pinned executable atomically under LocalAppData, in a content-specific directory so parallel app versions
// never overwrite one another while Tectonic is running.
#include "tectonic.h"

#include <shlobj.h>

#include "../core/inflate.h"

namespace vs {
namespace win {
namespace {

struct Resource {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

Resource tectonicResource() {
  Resource r;
  HRSRC resource = FindResourceW(nullptr, L"TECTONIC_EXE", RT_RCDATA);
  if (!resource) return r;
  HGLOBAL loaded = LoadResource(nullptr, resource);
  if (!loaded) return r;
  r.data = static_cast<const uint8_t*>(LockResource(loaded));
  r.size = SizeofResource(nullptr, resource);
  return r;
}

uint32_t read32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool ensureDirectory(const std::wstring& path) {
  if (CreateDirectoryW(path.c_str(), nullptr)) return true;
  if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
  DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring localAppDataRoot() {
  wchar_t path[MAX_PATH] = {0};
  std::wstring root;
  if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path) == S_OK)
    root = std::wstring(path) + L"\\VOSStudio";
  else
    root = widen(appDataDir());
  if (root.empty() || !ensureDirectory(root)) return L"";
  return root;
}

bool fileHasSize(const std::wstring& path, uint32_t expected) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
  const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  return size == expected;
}

}  // namespace

std::wstring tectonicPath(string* error) {
  if (error) error->clear();
  const Resource resource = tectonicResource();
  uint32_t tag = 0, rawSize = 0;
  if (!resource.data || !blobInfo(resource.data, resource.size, &tag, &rawSize) ||
      tag != 0x54454354u /* TECT */ || rawSize == 0) {
    if (error) *error = "This VOSStudio build does not contain its required Tectonic engine.";
    return L"";
  }
  const uint32_t compressedSize = read32(resource.data + 12);
  if (uint64_t(compressedSize) + 20 > resource.size) {
    if (error) *error = "The embedded Tectonic engine resource is incomplete.";
    return L"";
  }
  const uint32_t crc = read32(resource.data + 16 + compressedSize);  // CRC-32 immediately follows the deflate stream

  const std::wstring root = localAppDataRoot();
  if (root.empty()) {
    if (error) *error = "VOSStudio could not access LocalAppData to extract its Tectonic engine.";
    return L"";
  }
  const std::wstring parent = root + L"\\tectonic";
  if (!ensureDirectory(parent)) {
    if (error) *error = "VOSStudio could not create the Tectonic engine folder in LocalAppData.";
    return L"";
  }
  const string leaf = std::to_string(rawSize) + "-" + std::to_string(crc);
  const std::wstring dir = parent + L"\\" + widen(leaf);
  if (!ensureDirectory(dir)) {
    if (error) *error = "VOSStudio could not create a versioned Tectonic engine folder.";
    return L"";
  }
  const std::wstring target = dir + L"\\tectonic.exe";
  if (fileHasSize(target, rawSize)) return target;

  string binary, unpackError;
  uint32_t unpackedTag = 0;
  if (!unpackBlob(resource.data, resource.size, binary, &unpackedTag, &unpackError) ||
      unpackedTag != 0x54454354u || binary.size() != rawSize) {
    if (error) *error = "The embedded Tectonic engine failed its integrity check" +
                        (unpackError.empty() ? string(".") : string(": ") + unpackError);
    return L"";
  }

  const std::wstring temporary = target + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                                 std::to_wstring(GetCurrentThreadId()) + L".part";
  if (!writeFileU(narrow(temporary), binary)) {
    if (error) *error = "VOSStudio could not write the Tectonic engine to LocalAppData.";
    return L"";
  }
  if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD code = GetLastError();
    DeleteFileW(temporary.c_str());
    // A second VOSStudio instance may have extracted the same content concurrently.
    if (!fileHasSize(target, rawSize)) {
      if (error) *error = "VOSStudio could not install its Tectonic engine (Windows error " + std::to_string(code) + ").";
      return L"";
    }
  }
  if (!fileHasSize(target, rawSize)) {
    if (error) *error = "The extracted Tectonic engine has an unexpected file size.";
    return L"";
  }
  return target;
}

}  // namespace win
}  // namespace vs
