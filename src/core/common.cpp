#include "common.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace vs {

Color Color::parse(const string& s) {
  string h = s;
  if (!h.empty() && h[0] == '#') h = h.substr(1);
  if (h.size() == 3) h = string{h[0], h[0], h[1], h[1], h[2], h[2]};
  uint32_t v = 0;
  for (char c : h.substr(0, 6)) {
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
  }
  return hex(v);
}
string Color::hexStr() const {
  char buf[8];
  auto q = [](float x) { return int(std::lround(clampv(x, 0.f, 1.f) * 255)); };
  snprintf(buf, sizeof buf, "#%02x%02x%02x", q(r), q(g), q(b));
  return buf;
}

string trim(const string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') a++;
  while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
  // non-breaking space (UTF-8 C2 A0) at ends
  while (b - a >= 2 && (unsigned char)s[a] == 0xC2 && (unsigned char)s[a + 1] == 0xA0) a += 2;
  while (b - a >= 2 && (unsigned char)s[b - 2] == 0xC2 && (unsigned char)s[b - 1] == 0xA0) b -= 2;
  return s.substr(a, b - a);
}
string lower(const string& s) {
  string r = s;
  for (auto& c : r) if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return r;
}
string upper(const string& s) {
  string r = s;
  for (auto& c : r) if (c >= 'a' && c <= 'z') c = char(c - 32);
  return r;
}
vector<string> split(const string& s, char sep, bool keepEmpty) {
  vector<string> out;
  string cur;
  for (char c : s) {
    if (c == sep) {
      if (keepEmpty || !cur.empty()) out.push_back(cur);
      cur.clear();
    } else cur += c;
  }
  if (keepEmpty || !cur.empty()) out.push_back(cur);
  return out;
}
vector<string> splitAny(const string& s, const string& seps) {
  vector<string> out;
  string cur;
  for (char c : s) {
    if (seps.find(c) != string::npos) {
      string t = trim(cur);
      if (!t.empty()) out.push_back(t);
      cur.clear();
    } else cur += c;
  }
  string t = trim(cur);
  if (!t.empty()) out.push_back(t);
  return out;
}
string join(const vector<string>& v, const string& sep) {
  string r;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) r += sep;
    r += v[i];
  }
  return r;
}
bool startsWith(const string& s, const string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const string& s, const string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool iequals(const string& a, const string& b) { return lower(a) == lower(b); }
bool contains(const string& s, const string& sub) { return s.find(sub) != string::npos; }
bool icontains(const string& s, const string& sub) { return lower(s).find(lower(sub)) != string::npos; }
string replaceAll(string s, const string& from, const string& to) {
  if (from.empty()) return s;
  size_t p = 0;
  while ((p = s.find(from, p)) != string::npos) {
    s.replace(p, from.size(), to);
    p += to.size();
  }
  return s;
}
string collapseWs(const string& s) {
  string r;
  bool ws = false;
  for (char c : s) {
    if ((unsigned char)c <= ' ') {
      ws = true;
      continue;
    }
    if (ws && !r.empty()) r += ' ';
    ws = false;
    r += c;
  }
  return r;
}
string titleCase(const string& s) {
  string r = s;
  bool start = true;
  for (auto& c : r) {
    if (start && c >= 'a' && c <= 'z') c = char(c - 32);
    start = (c == ' ' || c == '-' || c == '(');
  }
  return r;
}
string fmtInt(long long v) {
  bool neg = v < 0;
  unsigned long long u = neg ? -(unsigned long long)v : v;
  string d = std::to_string(u), r;
  int k = 0;
  for (int i = int(d.size()) - 1; i >= 0; i--) {
    r += d[i];
    if (++k % 3 == 0 && i) r += ',';
  }
  if (neg) r += '-';
  std::reverse(r.begin(), r.end());
  return r;
}
string fmtFixed(double v, int dec) {
  char b[64];
  snprintf(b, sizeof b, "%.*f", dec, v);
  return b;
}
string fmtNum(double v, int dec) {
  if (!std::isfinite(v)) return "–";
  if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) return fmtInt((long long)std::llround(v));
  return fmtFixed(v, dec);
}
size_t utf8Len(const string& s) {
  size_t n = 0;
  for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
  return n;
}
string truncate(const string& s, size_t n) {
  if (utf8Len(s) <= n) return s;
  size_t cnt = 0, i = 0;
  for (; i < s.size(); i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80) {
      if (cnt == n - 1) break;
      cnt++;
    }
  }
  return trim(s.substr(0, i)) + "\xE2\x80\xA6";
}
string asciiFold(const string& s) {
  // map common 2-byte UTF-8 Latin-1 letters to ASCII
  static const char* from[] = {"á","à","â","ä","ã","å","é","è","ê","ë","í","ì","î","ï","ó","ò","ô","ö","õ","ø","ú","ù","û","ü","ç","ñ","ý","Á","À","Â","Ä","Ã","Å","É","È","Ê","Ë","Í","Ì","Î","Ï","Ó","Ò","Ô","Ö","Õ","Ø","Ú","Ù","Û","Ü","Ç","Ñ","ß","ł","Ł","ş","Ş","ğ","ı","č","Č","š","Š","ž","Ž","ć","ř","ě","ő","ű"};
  static const char* to[] = {"a","a","a","a","a","a","e","e","e","e","i","i","i","i","o","o","o","o","o","o","u","u","u","u","c","n","y","A","A","A","A","A","A","E","E","E","E","I","I","I","I","O","O","O","O","O","O","U","U","U","U","C","N","ss","l","L","s","S","g","i","c","C","s","S","z","Z","c","r","e","o","u"};
  bool any = false;
  for (unsigned char c : s) if (c >= 0x80) { any = true; break; }
  if (!any) return s;
  string r = s;
  for (size_t k = 0; k < sizeof(from) / sizeof(from[0]); k++) r = replaceAll(r, from[k], to[k]);
  return r;
}
bool isDigits(const string& s) {
  if (s.empty()) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}
int toInt(const string& s, int def) {
  string t = trim(s);
  if (t.empty()) return def;
  char* e = nullptr;
  long v = strtol(t.c_str(), &e, 10);
  return e == t.c_str() ? def : int(v);
}
double toDouble(const string& s, double def) {
  string t = trim(s);
  if (t.empty()) return def;
  char* e = nullptr;
  double v = strtod(t.c_str(), &e);
  return e == t.c_str() ? def : v;
}
// Paths are UTF-8. On Windows the narrow C runtime uses the ANSI code page, so convert to UTF-16 and use _wfopen.
FILE* openFileUtf8(const string& path, bool write) {
#ifdef _WIN32
  std::wstring w;
  for (size_t i = 0; i < path.size();) {
    unsigned char c = (unsigned char)path[i];
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 7; extra = 3; }
    else if (c >= 0xE0) { cp = c & 15; extra = 2; }
    else if (c >= 0xC0) { cp = c & 31; extra = 1; }
    i++;
    for (int k = 0; k < extra && i < path.size(); k++, i++) cp = (cp << 6) | ((unsigned char)path[i] & 63);
    if (cp >= 0x10000) { cp -= 0x10000; w += wchar_t(0xD800 + (cp >> 10)); w += wchar_t(0xDC00 + (cp & 0x3FF)); }
    else w += wchar_t(cp);
  }
  return _wfopen(w.c_str(), write ? L"wb" : L"rb");
#else
  return fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

string readFile(const string& path, bool* ok) {
  FILE* f = openFileUtf8(path, false);
  if (!f) {
    if (ok) *ok = false;
    return "";
  }
  string s;
  if (fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
    if (sz > 0) s.reserve(size_t(sz));
    fseek(f, 0, SEEK_SET);
  }
  char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  if (ok) *ok = true;
  // UTF-8 BOM
  if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
  // UTF-16 LE BOM → convert to UTF-8 (WoS sometimes exports UTF-16)
  if (s.size() >= 2 && (unsigned char)s[0] == 0xFF && (unsigned char)s[1] == 0xFE) {
    string out;
    for (size_t i = 2; i + 1 < s.size(); i += 2) {
      uint32_t c = (unsigned char)s[i] | ((unsigned char)s[i + 1] << 8);
      if (c >= 0xD800 && c < 0xDC00 && i + 3 < s.size()) {
        uint32_t d = (unsigned char)s[i + 2] | ((unsigned char)s[i + 3] << 8);
        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        i += 2;
      }
      if (c < 0x80) out += char(c);
      else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 63)); }
      else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
      else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 63)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
    }
    s = out;
  }
  return s;
}
bool writeFileParts(const string& path, const std::vector<const string*>& parts) {
  FILE* f = openFileUtf8(path, true);
  if (!f) return false;
  bool ok = true;
  for (auto* p : parts) if (!p->empty() && fwrite(p->data(), 1, p->size(), f) != p->size()) ok = false;
  if (fclose(f) != 0) ok = false;
  return ok;
}
bool writeFile(const string& path, const string& data) { return writeFileParts(path, {&data}); }
string fileExt(const string& path) {
  size_t d = path.find_last_of('.'), s = path.find_last_of("/\\");
  if (d == string::npos || (s != string::npos && d < s)) return "";
  return lower(path.substr(d + 1));
}
string fileName(const string& path) {
  size_t s = path.find_last_of("/\\");
  return s == string::npos ? path : path.substr(s + 1);
}
string xmlEscape(const string& s) {
  string r;
  r.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': r += "&amp;"; break;
      case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break;
      case '"': r += "&quot;"; break;
      case '\'': r += "&apos;"; break;
      default: r += c;
    }
  }
  return r;
}

// ---------------------------------------------------------------- SHA-256
namespace {
const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace
string sha256Hex(const char* data, size_t len) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto block = [&](const unsigned char* m) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t(m[i * 4]) << 24) | (uint32_t(m[i * 4 + 1]) << 16) | (uint32_t(m[i * 4 + 2]) << 8) | uint32_t(m[i * 4 + 3]);
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
      uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  };
  const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
  size_t full = len / 64 * 64;
  for (size_t off = 0; off < full; off += 64) block(p + off);
  unsigned char tail[128] = {0};
  size_t r = len - full;
  for (size_t i = 0; i < r; i++) tail[i] = p[full + i];
  tail[r] = 0x80;
  size_t tl = r + 1 + 8 <= 64 ? 64 : 128;
  uint64_t bits = uint64_t(len) * 8;
  for (int i = 0; i < 8; i++) tail[tl - 1 - size_t(i)] = (unsigned char)((bits >> (i * 8)) & 255);
  block(tail);
  if (tl == 128) block(tail + 64);
  char out[65];
  for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
  return string(out, 64);
}
string sha256Hex(const string& data) { return sha256Hex(data.data(), data.size()); }
string nowIso() {
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  char b[32];
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return b;
}

// ------------------------------------------------------------ bib keys
string bibClean(const string& s) {
  string t = collapseWs(trim(s));
  while (!t.empty() && (t.back() == '.' || t.back() == ';' || t.back() == ',')) t.pop_back();
  while (!t.empty() && (t[0] == '"' || t[0] == '\'')) t.erase(0, 1);
  while (!t.empty() && (t.back() == '"' || t.back() == '\'')) t.pop_back();
  return trim(t);
}
string bibKey(const string& s) {
  string t = lower(asciiFold(bibClean(s)));
  string r;
  for (char c : t) {
    if (c == '-' || c == '_' || c == '/') c = ' ';
    if (c == '"' || c == '\'' || c == '.' || c == ',') continue;
    r += c;
  }
  return collapseWs(r);
}
string sourceKey(const string& s) {
  string t = lower(asciiFold(s)), r;
  for (char c : t) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) r += c;
  return r;
}
string normDoi(const string& s) {
  string t = lower(trim(s));
  for (const char* p : {"https://doi.org/", "http://doi.org/", "https://dx.doi.org/", "http://dx.doi.org/", "doi:", "doi "})
    if (startsWith(t, p)) t = trim(t.substr(strlen(p)));
  while (!t.empty() && (t.back() == '.' || t.back() == ',' || t.back() == ';')) t.pop_back();
  return startsWith(t, "10.") ? t : "";
}

Timer::Timer() { t0 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
double Timer::ms() const {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0;
}

}  // namespace vs

// ------------------------------------------------------------------ encoding / compression
namespace vs {

string base64(const string& d) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  string o;
  o.reserve((d.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < d.size(); i += 3) {
    uint32_t v = (uint8_t(d[i]) << 16) | (uint8_t(d[i + 1]) << 8) | uint8_t(d[i + 2]);
    o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
  }
  if (i + 1 == d.size()) { uint32_t v = uint8_t(d[i]) << 16; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "=="; }
  else if (i + 2 == d.size()) { uint32_t v = (uint8_t(d[i]) << 16) | (uint8_t(d[i + 1]) << 8); o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '='; }
  return o;
}

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
  static uint32_t tab[256];
  static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; } init = true; }
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = tab[(crc ^ p[i]) & 255] ^ (crc >> 8);
  return ~crc;
}

namespace {
struct BitW {
  string out;
  uint32_t acc = 0;
  int n = 0;
  void put(uint32_t v, int bits) { acc |= v << n; n += bits; while (n >= 8) { out += char(acc & 255); acc >>= 8; n -= 8; } }
  void putRev(uint32_t code, int len) { uint32_t r = 0; for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i); put(r, len); }
  void flush() { if (n > 0) { out += char(acc & 255); acc = 0; n = 0; } }
};
void fixedLit(BitW& w, int v) {
  if (v < 144) w.putRev(0x30 + v, 8);
  else if (v < 256) w.putRev(0x190 + v - 144, 9);
  else if (v < 280) w.putRev(v - 256, 7);
  else w.putRev(0xC0 + v - 280, 8);
}
const int LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
}  // namespace

string zlibCompress(const string& data) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
  size_t n = data.size();
  BitW w;
  w.out += char(0x78); w.out += char(0x01);
  w.put(1, 1);  // BFINAL
  w.put(1, 2);  // fixed Huffman
  const int HB = 15, WIN = 32768;
  vector<int> head(size_t(1) << HB, -1), prev(n, -1);
  auto hash = [&](size_t i) { return ((uint32_t(p[i]) << 10) ^ (uint32_t(p[i + 1]) << 5) ^ p[i + 2]) & ((1u << HB) - 1); };
  size_t i = 0;
  while (i < n) {
    int bestLen = 0, bestDist = 0;
    if (i + 2 < n) {
      uint32_t h = hash(i);
      int cand = head[h], chain = 0;
      while (cand >= 0 && int(i) - cand <= WIN && chain++ < 24) {
        int len = 0, mx = int(std::min<size_t>(258, n - i));
        while (len < mx && p[size_t(cand) + size_t(len)] == p[i + size_t(len)]) len++;
        if (len > bestLen) { bestLen = len; bestDist = int(i) - cand; if (len == mx) break; }
        cand = prev[size_t(cand)];
      }
      prev[i] = head[h];
      head[h] = int(i);
    }
    if (bestLen >= 3) {
      int li = 0;
      while (li < 28 && LBASE[li + 1] <= bestLen) li++;
      fixedLit(w, 257 + li);
      if (LEXT[li]) w.put(uint32_t(bestLen - LBASE[li]), LEXT[li]);
      int di = 0;
      while (di < 29 && DBASE[di + 1] <= bestDist) di++;
      w.putRev(uint32_t(di), 5);
      if (DEXT[di]) w.put(uint32_t(bestDist - DBASE[di]), DEXT[di]);
      for (int k = 1; k < bestLen; k++) {
        size_t j = i + size_t(k);
        if (j + 2 < n) { uint32_t h = hash(j); prev[j] = head[h]; head[h] = int(j); }
      }
      i += size_t(bestLen);
    } else { fixedLit(w, p[i]); i++; }
  }
  fixedLit(w, 256);
  w.flush();
  uint32_t a = 1, b = 0;
  for (size_t k = 0; k < n; k++) { a = (a + p[k]) % 65521; b = (b + a) % 65521; }
  uint32_t ad = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) w.out += char((ad >> s) & 255);
  return w.out;
}

string pngEncode(int w, int h, const uint8_t* px, bool alpha, double dpi) {
  int ch = alpha ? 4 : 3;
  string raw;
  raw.reserve(size_t(h) * (size_t(w) * ch + 1));
  for (int y = 0; y < h; y++) {
    raw += char(1);  // Sub filter: good for smooth rasters
    const uint8_t* row = px + size_t(y) * size_t(w) * 4;
    for (int x = 0; x < w; x++)
      for (int c = 0; c < ch; c++) {
        uint8_t v = row[x * 4 + c], left = x ? row[(x - 1) * 4 + c] : 0;
        raw += char(uint8_t(v - left));
      }
  }
  auto be32 = [](string& s, uint32_t v) { for (int k = 24; k >= 0; k -= 8) s += char((v >> k) & 255); };
  auto chunk = [&](string& out, const char* type, const string& data) {
    be32(out, uint32_t(data.size()));
    string td = string(type, 4) + data;
    out += td;
    be32(out, crc32(reinterpret_cast<const uint8_t*>(td.data()), td.size()));
  };
  string out = "\x89PNG\r\n\x1a\n";
  string ihdr;
  be32(ihdr, uint32_t(w)); be32(ihdr, uint32_t(h));
  ihdr += char(8); ihdr += char(alpha ? 6 : 2); ihdr += char(0); ihdr += char(0); ihdr += char(0);
  chunk(out, "IHDR", ihdr);
  if (dpi > 0) { string phys; uint32_t ppm = uint32_t(dpi / 0.0254 + 0.5); be32(phys, ppm); be32(phys, ppm); phys += char(1); chunk(out, "pHYs", phys); }
  chunk(out, "IDAT", zlibCompress(raw));
  chunk(out, "IEND", "");
  return out;
}

}  // namespace vs

namespace vs {
string base64Decode(const string& b64) {
  auto val = [](unsigned char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    return -1;
  };
  string out;
  int acc = 0, bits = 0;
  for (unsigned char ch : b64) {
    if (ch == '=') break;
    int v = val(ch);
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xFF); }
  }
  return out;
}
}  // namespace vs
