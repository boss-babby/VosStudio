#include "platform.h"
#include "version.h"

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

// Text + "HTML Format" (CF_HTML, UTF-8 bytes with the offsets header) + optionally a picture, in one transaction, so
// Word takes the HTML (formatting, fields, pictures), editors without HTML take the text and picture-only targets the DIB.
bool setClipboardRich(HWND owner, const string& utf8, const string& cfHtml, int w, int h, const uint8_t* bgra, const string* emf, const string* png) {
  if (!OpenClipboard(owner)) return false;
  EmptyClipboard();
  bool ok = false;
  if (!utf8.empty()) {
    std::wstring ws = widen(utf8);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (ws.size() + 1) * sizeof(wchar_t));
    if (g) { memcpy(GlobalLock(g), ws.c_str(), (ws.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); ok = true; }
  }
  // a vector picture first: Office pastes an Enhanced Metafile as a scalable drawing (Paste Special: Picture (Enhanced Metafile))
  if (emf && !emf->empty()) {
    HENHMETAFILE h = SetEnhMetaFileBits(UINT(emf->size()), reinterpret_cast<const BYTE*>(emf->data()));
    if (h) { SetClipboardData(CF_ENHMETAFILE, h); ok = true; }  // the clipboard owns the handle now
  }
  if (png && !png->empty()) {  // the "PNG" format Office applications register: the picture with its transparency
    UINT fmt = RegisterClipboardFormatW(L"PNG");
    HGLOBAL g = fmt ? GlobalAlloc(GMEM_MOVEABLE, png->size()) : nullptr;
    if (g) { memcpy(GlobalLock(g), png->data(), png->size()); GlobalUnlock(g); SetClipboardData(fmt, g); ok = true; }
  }
  if (!cfHtml.empty()) {
    UINT fmt = RegisterClipboardFormatW(L"HTML Format");
    HGLOBAL g = fmt ? GlobalAlloc(GMEM_MOVEABLE, cfHtml.size() + 1) : nullptr;
    if (g) { char* p = static_cast<char*>(GlobalLock(g)); memcpy(p, cfHtml.data(), cfHtml.size()); p[cfHtml.size()] = 0; GlobalUnlock(g); SetClipboardData(fmt, g); }
  }
  if (bgra && w > 0 && h > 0) {
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
      ok = true;
    }
  }
  CloseClipboard();
  return ok;
}

string getClipboardHtml(HWND owner) {
  UINT fmt = RegisterClipboardFormatW(L"HTML Format");
  if (!fmt || !IsClipboardFormatAvailable(fmt) || !OpenClipboard(owner)) return "";
  string out;
  HANDLE h = GetClipboardData(fmt);
  if (h) {
    const char* p = static_cast<const char*>(GlobalLock(h));
    SIZE_T n = GlobalSize(h);
    if (p) { out.assign(p, strnlen(p, n)); GlobalUnlock(h); }
  }
  CloseClipboard();
  return out;
}

string clipboardFilesDir() {
  wchar_t t[MAX_PATH] = {0};
  DWORD n = GetTempPathW(MAX_PATH, t);
  std::wstring d = (n && n < MAX_PATH ? std::wstring(t) : widen(appDataDir()) + L"\\");
  d += L"VOSStudio";
  CreateDirectoryW(d.c_str(), nullptr);
  d += L"\\clip";
  CreateDirectoryW(d.c_str(), nullptr);
  return narrow(d);
}

string fileUrl(const string& path) {
  string o = "file:///";
  for (char c : path) {
    if (c == '\\') o += '/';
    else if (isalnum(uint8_t(c)) || c == '/' || c == ':' || c == '.' || c == '-' || c == '_' || c == '~') o += c;
    else { char b[8]; snprintf(b, sizeof b, "%%%02X", uint8_t(c)); o += b; }
  }
  return o;
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

string attachmentsDir() {
  wchar_t p[MAX_PATH] = {0};
  std::wstring base;
  if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, p) == S_OK) base = p;
  else base = widen(appDataDir());
  std::wstring d = base + L"\\VOSStudio";
  CreateDirectoryW(d.c_str(), nullptr);
  d += L"\\Attachments";
  CreateDirectoryW(d.c_str(), nullptr);
  return narrow(d);
}

bool ensureDir(const string& path) {
  std::wstring w = widen(path);
  if (CreateDirectoryW(w.c_str(), nullptr)) return true;
  DWORD a = GetFileAttributesW(w.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

string exportsDir() {
  wchar_t p[MAX_PATH] = {0};
  std::wstring base;
  if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, p) == S_OK) base = p;
  else base = widen(appDataDir());
  std::wstring d = base + L"\\VOSStudio";
  CreateDirectoryW(d.c_str(), nullptr);
  d += L"\\Exports";
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
  HINTERNET s = WinHttpOpen(L"VOSStudio/" VOS_VERSION_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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
  HINTERNET s = WinHttpOpen(L"VOSStudio/" VOS_VERSION_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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

bool httpDownload(const string& url, const vector<std::pair<string, string>>& headers, const string& path, HttpDownload& out,
                  const std::function<bool(const char*, size_t)>& accept, const std::function<bool(long long, long long)>& progress,
                  long long maxBytes, int timeoutMs, const vector<string>& wantHeaders) {
  out = HttpDownload();
  std::wstring wu = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  std::vector<wchar_t> pathB(wu.size() + 16, 0), extraB(wu.size() + 16, 0);
  wchar_t host[256] = {0};
  uc.lpszHostName = host; uc.dwHostNameLength = 255;
  uc.lpszUrlPath = pathB.data(); uc.dwUrlPathLength = DWORD(pathB.size() - 1);
  uc.lpszExtraInfo = extraB.data(); uc.dwExtraInfoLength = DWORD(extraB.size() - 1);
  if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) { out.err = "invalid address"; return false; }
  HINTERNET s = WinHttpOpen(L"VOSStudio/" VOS_VERSION_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) { out.err = "WinHTTP unavailable"; return false; }
  WinHttpSetTimeouts(s, 10000, 15000, 30000, timeoutMs);
  DWORD redirects = 10;
  WinHttpSetOption(s, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof redirects);
  HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
  std::wstring full = std::wstring(pathB.data()) + extraB.data();
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                  : nullptr;
  std::wstring hdr;
  for (auto& h : headers) hdr += widen(h.first) + L": " + widen(h.second) + L"\r\n";
  bool ok = false;
  bool sent = r && WinHttpSendRequest(r, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
  if (sent && WinHttpReceiveResponse(r, nullptr)) {
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    out.status = int(code);
    auto strHeader = [&](DWORD which, const wchar_t* name) {
      DWORD n = 0;
      WinHttpQueryHeaders(r, which, name, WINHTTP_NO_OUTPUT_BUFFER, &n, WINHTTP_NO_HEADER_INDEX);
      if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || n == 0) return string();
      std::wstring buf(n / sizeof(wchar_t) + 1, L'\0');
      if (!WinHttpQueryHeaders(r, which, name, &buf[0], &n, WINHTTP_NO_HEADER_INDEX)) return string();
      buf.resize(n / sizeof(wchar_t));
      return narrow(buf);
    };
    out.contentType = lower(strHeader(WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX));
    size_t semi = out.contentType.find(';');
    if (semi != string::npos) out.contentType = trim(out.contentType.substr(0, semi));
    string len = strHeader(WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX);
    if (!len.empty()) out.length = atoll(len.c_str());
    for (auto& w : wantHeaders) { string v = strHeader(WINHTTP_QUERY_CUSTOM, widen(w).c_str()); if (!v.empty()) out.headers[lower(w)] = trim(v); }
    {
      DWORD n = 0;
      WinHttpQueryOption(r, WINHTTP_OPTION_URL, nullptr, &n);
      if (n > 0) { std::wstring u(n / sizeof(wchar_t) + 1, L'\0'); if (WinHttpQueryOption(r, WINHTTP_OPTION_URL, &u[0], &n)) { u.resize(wcslen(u.c_str())); out.finalUrl = narrow(u); } }
    }
    HANDLE f = INVALID_HANDLE_VALUE;
    string first;
    bool refused = false, cancelled = false, failedWrite = false;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(r, &avail) && avail) {
      string chunk(avail, '\0');
      DWORD got = 0;
      if (!WinHttpReadData(r, &chunk[0], avail, &got)) break;
      if (got == 0) continue;
      if (!out.accepted) {  // the first bytes decide (buffer until a kilobyte is there or the body is smaller)
        first.append(chunk.data(), got);
        if (first.size() < 1024 && (out.length < 0 || (long long)first.size() < out.length)) continue;
        if (!accept(first.data(), first.size())) { refused = true; out.first = first.substr(0, 256); break; }
        out.accepted = true;
        f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { failedWrite = true; break; }
        DWORD wr = 0;
        if (!WriteFile(f, first.data(), DWORD(first.size()), &wr, nullptr) || wr != first.size()) { failedWrite = true; break; }
        out.received = (long long)first.size();
      } else {
        DWORD wr = 0;
        if (!WriteFile(f, chunk.data(), got, &wr, nullptr) || wr != got) { failedWrite = true; break; }
        out.received += got;
      }
      if (out.received > maxBytes) { out.err = "larger than the limit"; break; }
      if (progress && !progress(out.received, out.length)) { cancelled = true; break; }
    }
    if (!out.accepted && !refused && !first.empty() && !failedWrite) {  // a body smaller than a kilobyte that ended: judge it now
      if (accept(first.data(), first.size())) {
        f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD wr = 0;
        if (f != INVALID_HANDLE_VALUE && WriteFile(f, first.data(), DWORD(first.size()), &wr, nullptr) && wr == first.size()) { out.accepted = true; out.received = (long long)first.size(); }
        else failedWrite = true;
      } else { refused = true; out.first = first.substr(0, 256); }
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!out.accepted && !refused && first.empty() && out.err.empty() && !failedWrite && !cancelled) out.err = "empty answer";
    if (refused) out.err = "refused";
    else if (failedWrite) out.err = "the file could not be written";
    else if (cancelled) out.err = "cancelled";
    else if (out.err.empty() && out.accepted) {
      out.complete = out.length < 0 || out.received >= out.length;
      if (!out.complete) out.err = "the transfer stopped early";
      ok = out.complete;
    }
    if (!ok && out.accepted) DeleteFileW(widen(path).c_str());
  } else {
    DWORD e = GetLastError();
    out.err = e == ERROR_WINHTTP_TIMEOUT ? "the request timed out" : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "the server name could not be resolved"
            : e == ERROR_WINHTTP_CANNOT_CONNECT ? "the connection was refused" : e == ERROR_WINHTTP_SECURE_FAILURE ? "the secure connection failed" : "network error " + std::to_string(e);
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok;
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
