// VOSStudio Native — shared utilities (portable C++17, no Windows headers)
#pragma once
#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vs {

using std::string;
using std::vector;

// ------------------------------------------------------------------ random
// mulberry32 — identical sequence to the web app's bsRng / mulberry32
struct Rng {
  uint32_t a;
  explicit Rng(uint32_t seed = 1) : a(seed) {}
  double operator()() {
    a += 0x6D2B79F5u;
    uint32_t t = a;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return double(t ^ (t >> 14)) / 4294967296.0;
  }
  int below(int n) { return n <= 0 ? 0 : std::min(n - 1, int((*this)() * n)); }
};

// ------------------------------------------------------------------ colour
struct Color {
  float r = 0, g = 0, b = 0, a = 1;
  Color() = default;
  Color(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
  static Color hex(uint32_t rgb, float a = 1) {
    return Color(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
  }
  static Color parse(const string& s);  // "#rrggbb"
  string hexStr() const;
  Color withA(float na) const { return Color(r, g, b, na); }
  Color mix(const Color& o, float t) const {
    return Color(r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t);
  }
  float luma() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
};

// ------------------------------------------------------------------ strings
string trim(const string& s);
string lower(const string& s);
string upper(const string& s);
vector<string> split(const string& s, char sep, bool keepEmpty = false);
vector<string> splitAny(const string& s, const string& seps);
string join(const vector<string>& v, const string& sep);
bool startsWith(const string& s, const string& p);
bool endsWith(const string& s, const string& p);
bool iequals(const string& a, const string& b);
bool contains(const string& s, const string& sub);
bool icontains(const string& s, const string& sub);
string replaceAll(string s, const string& from, const string& to);
string collapseWs(const string& s);
string titleCase(const string& s);
string fmtInt(long long v);             // 1,234
// The application's version and a reference to it, for the methods paragraph and "Cite" (set by the shell at start).
void setAppVersion(const string& v);
const string& appVersion();
string currentYear();                   // "2026"
string softwareReference(bool bibtex);  // APA-style reference to this software, or a BibTeX entry
string fmtNum(double v, int dec = 1);    // 12.3
string fmtFixed(double v, int dec);
string truncate(const string& s, size_t n);  // UTF-8 aware, adds …
size_t utf8Len(const string& s);
string asciiFold(const string& s);       // strip common Latin diacritics
bool isDigits(const string& s);
int toInt(const string& s, int def = 0);
double toDouble(const string& s, double def = 0);
string readFile(const string& path, bool* ok = nullptr);
bool writeFile(const string& path, const string& data);
bool writeFileParts(const string& path, const std::vector<const string*>& parts);
FILE* openFileUtf8(const string& path, bool write);
bool fileExistsU(const string& path);
long long fileSizeU(const string& path);   // -1 when it cannot be opened
bool removeFileU(const string& path);
bool renameFileU(const string& from, const string& to);  // replaces an existing target
string fileExt(const string& path);      // lower-case, without dot
string fileName(const string& path);
string xmlEscape(const string& s);
string sha256Hex(const string& data);
string sha256Hex(const char* data, size_t len);
string base64(const string& data);
string base64Decode(const string& b64);  // ignores whitespace and invalid characters
string zlibCompress(const string& data);   // deflate (LZ77 + fixed Huffman) in a zlib wrapper
uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0);
string pngEncode(int w, int h, const uint8_t* rgba, bool alpha, double dpi = 0);
string nowIso();

// normalised keys used everywhere for matching labels
string bibClean(const string& s);   // tidy a label (trim, collapse spaces, strip trailing dots)
string bibKey(const string& s);     // matching key for keywords / authors / orgs
string sourceKey(const string& s);  // matching key for sources (alnum only)
string normDoi(const string& s);

template <class T> T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double sq(double x) { return x * x; }

struct Timer {
  double t0;
  Timer();
  double ms() const;
};

}  // namespace vs
