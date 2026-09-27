#include "platform.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <cstdio>

namespace vs {
namespace win {

std::wstring widen(const string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}
string narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

static std::wstring filterString(const vector<FileFilter>& f) {
  std::wstring o;
  for (auto& x : f) { o += widen(x.name + " (" + x.pattern + ")"); o.push_back(L'\0'); o += widen(x.pattern); o.push_back(L'\0'); }
  o.push_back(L'\0');
  return o;
}

vector<string> openFileDialog(HWND owner, const string& title, const vector<FileFilter>& filters, bool multi) {
  vector<wchar_t> buf(65536, 0);
  std::wstring flt = filterString(filters), ttl = widen(title);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = flt.c_str();
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = DWORD(buf.size());
  ofn.lpstrTitle = ttl.c_str();
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (multi ? OFN_ALLOWMULTISELECT : 0);
  vector<string> out;
  if (!GetOpenFileNameW(&ofn)) return out;
  std::wstring first(buf.data());
  const wchar_t* p = buf.data() + first.size() + 1;
  if (!multi || *p == 0) { out.push_back(narrow(first)); return out; }
  while (*p) { std::wstring name(p); out.push_back(narrow(first + L"\\" + name)); p += name.size() + 1; }
  return out;
}

string saveFileDialog(HWND owner, const string& title, const vector<FileFilter>& filters, const string& defName, const string& defExt) {
  vector<wchar_t> buf(4096, 0);
  std::wstring dn = widen(defName);
  wcsncpy(buf.data(), dn.c_str(), buf.size() - 1);
  std::wstring flt = filterString(filters), ttl = widen(title), ext = widen(defExt);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = flt.c_str();
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = DWORD(buf.size());
  ofn.lpstrTitle = ttl.c_str();
  ofn.lpstrDefExt = ext.c_str();
  ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return "";
  return narrow(buf.data());
}

bool setClipboardText(HWND owner, const string& utf8) {
  if (!OpenClipboard(owner)) return false;
  EmptyClipboard();
  std::wstring w = widen(utf8);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (h) {
    memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
  }
  CloseClipboard();
  return h != nullptr;
}

string getClipboardText(HWND owner) {
  if (!OpenClipboard(owner)) return "";
  string out;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) { const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h)); if (p) out = narrow(p); GlobalUnlock(h); }
  CloseClipboard();
  return out;
}

bool setClipboardImage(HWND owner, int w, int h, const uint8_t* bgra) {
  if (!OpenClipboard(owner)) return false;
  EmptyClipboard();
  size_t sz = sizeof(BITMAPINFOHEADER) + size_t(w) * size_t(h) * 4;
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sz);
  if (g) {
    uint8_t* p = static_cast<uint8_t*>(GlobalLock(g));
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    memcpy(p, &bi, sizeof bi);
    for (int y = 0; y < h; y++) memcpy(p + sizeof bi + size_t(h - 1 - y) * size_t(w) * 4, bgra + size_t(y) * size_t(w) * 4, size_t(w) * 4);
    GlobalUnlock(g);
    SetClipboardData(CF_DIB, g);
  }
  CloseClipboard();
  return g != nullptr;
}

string readFileU(const string& path, bool* ok) {
  FILE* f = _wfopen(widen(path).c_str(), L"rb");
  if (!f) { if (ok) *ok = false; return ""; }
  string d;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.append(buf, n);
  fclose(f);
  if (ok) *ok = true;
  return d;
}

bool writeFileU(const string& path, const string& data) {
  FILE* f = _wfopen(widen(path).c_str(), L"wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  return ok;
}

string appDataDir() {
  wchar_t p[MAX_PATH] = {0};
  if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, p) != S_OK) return exeDir();
  std::wstring d = std::wstring(p) + L"\\VOSStudio";
  CreateDirectoryW(d.c_str(), nullptr);
  return narrow(d);
}

string reportsDir() {
  wchar_t p[MAX_PATH] = {0};
  std::wstring base;
  if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, p) == S_OK) base = p;
  else base = widen(appDataDir());
  std::wstring d = base + L"\\VOSStudio";
  CreateDirectoryW(d.c_str(), nullptr);
  d += L"\\Reports";
  CreateDirectoryW(d.c_str(), nullptr);
  return narrow(d);
}

string exeDir() {
  wchar_t p[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s(p);
  size_t k = s.find_last_of(L"\\/");
  return narrow(k == std::wstring::npos ? s : s.substr(0, k));
}

void openUrl(const string& url) { ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void revealInExplorer(const string& path) { ShellExecuteW(nullptr, L"open", L"explorer.exe", widen("/select,\"" + path + "\"").c_str(), nullptr, SW_SHOWNORMAL); }

void Settings::load() {
  bool ok = false;
  string t = readFileU(appDataDir() + "\\settings.json", &ok);
  if (ok) { Json p = Json::parse(t); if (p.t == Json::Obj) j = p; }
}
void Settings::save() const { writeFileU(appDataDir() + "\\settings.json", j.dump(1)); }
vector<string> Settings::recent() const { vector<string> r; for (auto& x : j["recent"].a) r.push_back(x.str()); return r; }
void Settings::addRecent(const string& path) {
  vector<string> r = recent();
  r.erase(std::remove(r.begin(), r.end(), path), r.end());
  r.insert(r.begin(), path);
  if (r.size() > 8) r.resize(8);
  Json a = Json::array();
  for (auto& s : r) a.push(s);
  j.set("recent", a);
  save();
}

string httpGet(const string& url, int* status, string* err) {
  std::wstring wu = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  // long semantic-search queries can make URLs of several thousand characters
  std::vector<wchar_t> pathB(wu.size() + 16, 0), extraB(wu.size() + 16, 0);
  wchar_t host[256] = {0};
  wchar_t* path = pathB.data();
  wchar_t* extra = extraB.data();
  uc.lpszHostName = host; uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path; uc.dwUrlPathLength = DWORD(pathB.size() - 1);
  uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = DWORD(extraB.size() - 1);
  if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) { if (err) *err = "Invalid URL"; return ""; }
  HINTERNET s = WinHttpOpen(L"VOSStudio/1.5", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) { if (err) *err = "WinHTTP unavailable"; return ""; }
  WinHttpSetTimeouts(s, 8000, 8000, 15000, 30000);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  std::wstring full = std::wstring(path) + extra;
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  string body;
  if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status) *status = int(code);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(r, &avail) && avail) {
      string chunk(avail, '\0');
      DWORD got = 0;
      if (!WinHttpReadData(r, &chunk[0], avail, &got)) break;
      body.append(chunk.data(), got);
    }
  } else if (err) *err = "Network request failed (error " + std::to_string(GetLastError()) + ")";
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return body;
}

string httpRequest(const string& method, const string& url, const vector<std::pair<string, string>>& headers, const string& body, int* status, string* err,
                   const std::function<bool(const char*, size_t)>& onData, int timeoutMs) {
  if (status) *status = 0;
  std::wstring wu = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  std::vector<wchar_t> pathB(wu.size() + 16, 0), extraB(wu.size() + 16, 0);
  wchar_t host[256] = {0};
  uc.lpszHostName = host; uc.dwHostNameLength = 255;
  uc.lpszUrlPath = pathB.data(); uc.dwUrlPathLength = DWORD(pathB.size() - 1);
  uc.lpszExtraInfo = extraB.data(); uc.dwExtraInfoLength = DWORD(extraB.size() - 1);
  if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) { if (err) *err = "Invalid address"; return ""; }
  HINTERNET s = WinHttpOpen(L"VOSStudio/1.5", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) { if (err) *err = "WinHTTP unavailable"; return ""; }
  WinHttpSetTimeouts(s, 10000, 15000, 30000, timeoutMs);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  std::wstring full = std::wstring(pathB.data()) + extraB.data();
  HINTERNET r = c ? WinHttpOpenRequest(c, widen(method).c_str(), full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                  : nullptr;
  std::wstring hdr;
  for (auto& h : headers) hdr += widen(h.first) + L": " + widen(h.second) + L"\r\n";
  string out;
  bool sent = r && WinHttpSendRequest(r, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : DWORD(-1L),
                                      body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0);
  if (sent && WinHttpReceiveResponse(r, nullptr)) {
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status) *status = int(code);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(r, &avail) && avail) {
      string chunk(avail, '\0');
      DWORD got = 0;
      if (!WinHttpReadData(r, &chunk[0], avail, &got)) break;
      out.append(chunk.data(), got);
      if (onData && !onData(chunk.data(), got)) break;
    }
  } else if (err) {
    DWORD e = GetLastError();
    *err = e == ERROR_WINHTTP_TIMEOUT ? "the request timed out" : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "the server name could not be resolved"
         : e == ERROR_WINHTTP_CANNOT_CONNECT ? "the connection was refused" : "network error " + std::to_string(e);
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return out;
}

string protectSecret(const string& plain) {
  if (plain.empty()) return "";
  DATA_BLOB in{DWORD(plain.size()), (BYTE*)plain.data()}, out{};
  if (!CryptProtectData(&in, L"VOSStudio", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
  string b(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return base64(b);
}

string unprotectSecret(const string& b64) {
  if (b64.empty()) return "";
  string raw = base64Decode(b64);
  DATA_BLOB in{DWORD(raw.size()), (BYTE*)raw.data()}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
  string s(reinterpret_cast<char*>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return s;
}

void Job::start(std::function<void(Job&)> fn) {
  join();
  progress = 0;
  cancel = false;
  finished = false;
  error.clear();
  started_ = true;
  th_ = std::thread([this, fn]() {
    try { fn(*this); } catch (std::exception& e) { error = e.what(); } catch (...) { error = "Unexpected error"; }
    finished = true;
    if (notify) PostMessageW(notify, WM_NULL, 0, 0);
  });
}

void Job::join() {
  if (th_.joinable()) th_.join();
}

double nowSeconds() {
  static LARGE_INTEGER f{};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) / double(f.QuadPart);
}

}  // namespace win
}  // namespace vs
