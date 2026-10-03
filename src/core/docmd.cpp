// VOSStudio Native: Markdown in and out of the document model, the assistant's report as a document, JSON for
// project files and the compact scene blob (see doc.h).
#include "doc.h"

#include <algorithm>
#include <cstring>

namespace vs {

namespace {

// ---------------------------------------------------------------- text repair
// Valid UTF-8 without control characters (tab and newline stay). Mojibake (UTF-8 decoded as Latin-1 and encoded
// again) is repaired; Latin-1 bytes become their UTF-8 characters.
string repairUtf8(const string& s) {
  string o;
  o.reserve(s.size());
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = uint8_t(s[i]);
    if (c < 0x80) {
      if (c >= 0x20 || c == '\t' || c == '\n') o += char(c);
      i++;
      continue;
    }
    int len = (c >= 0xF0 && c <= 0xF4) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2 && c <= 0xDF) ? 2 : 0;
    bool ok = len > 0 && i + size_t(len) <= n;
    for (int k = 1; ok && k < len; k++) ok = (uint8_t(s[i + size_t(k)]) & 0xC0) == 0x80;
    if (ok && len == 3) {
      uint32_t cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(uint8_t(s[i + 1]) & 0x3F) << 6) | uint32_t(uint8_t(s[i + 2]) & 0x3F);
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0xFFFE || cp == 0xFFFF) ok = false;
    }
    if (ok && len == 4) {
      uint32_t cp = (uint32_t(c & 0x07) << 18) | (uint32_t(uint8_t(s[i + 1]) & 0x3F) << 12) | (uint32_t(uint8_t(s[i + 2]) & 0x3F) << 6) | uint32_t(uint8_t(s[i + 3]) & 0x3F);
      if (cp < 0x10000 || cp > 0x10FFFF) ok = false;
    }
    if (ok) { o.append(s, i, size_t(len)); i += size_t(len); }
    else { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); i++; }  // a Latin-1 byte
  }
  // double-encoded UTF-8 ("Ã©" for "é", "â€“" for "–"): a run of sequences that each decode to one Latin-1 /
  // Windows-1252 byte is replaced when those bytes form valid UTF-8 on their own; everything else is kept
  static const struct { uint32_t cp; unsigned char b; } w1252[] = {{0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87}, {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C}, {0x017D, 0x8E}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98}, {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B}, {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F}};
  auto latinByte = [&](const string& t, size_t k, size_t& len, unsigned char& b) {
    unsigned char c = uint8_t(t[k]);
    if ((c & 0xE0) == 0xC0 && k + 1 < t.size()) { uint32_t cp = (uint32_t(c & 0x1F) << 6) | uint32_t(uint8_t(t[k + 1]) & 0x3F); if (cp < 0x80 || cp > 0xFF) return false; len = 2; b = uint8_t(cp); return true; }
    if ((c & 0xF0) == 0xE0 && k + 2 < t.size()) {
      uint32_t cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(uint8_t(t[k + 1]) & 0x3F) << 6) | uint32_t(uint8_t(t[k + 2]) & 0x3F);
      for (auto& e : w1252) if (e.cp == cp) { len = 3; b = e.b; return true; }
    }
    return false;
  };
  auto validUtf8 = [](const string& t) {
    size_t k = 0;
    bool multi = false;
    while (k < t.size()) {
      unsigned char c = uint8_t(t[k]);
      int len = c < 0x80 ? 1 : (c >= 0xF0 && c <= 0xF4) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2 && c <= 0xDF) ? 2 : 0;
      if (!len || k + size_t(len) > t.size()) return false;
      for (int j = 1; j < len; j++) if ((uint8_t(t[k + size_t(j)]) & 0xC0) != 0x80) return false;
      if (len > 1) multi = true;
      k += size_t(len);
    }
    return multi;
  };
  if (o.find("\xC3") != string::npos || o.find("\xC2") != string::npos || o.find("\xC5") != string::npos) {
    string r;
    for (size_t k = 0; k < o.size();) {
      size_t len = 0;
      unsigned char b = 0;
      if (uint8_t(o[k]) < 0x80 || !latinByte(o, k, len, b)) { r += o[k]; k++; continue; }
      string bytes, src;
      size_t j = k;
      while (j < o.size() && uint8_t(o[j]) >= 0x80 && latinByte(o, j, len, b)) { bytes += char(b); src.append(o, j, len); j += len; }
      // continuation bytes must follow a lead byte that is itself "mojibake" (a Latin-1 letter such as Ã or Â)
      r += validUtf8(bytes) && uint8_t(bytes[0]) >= 0xC2 ? bytes : src;
      k = j;
    }
    return r;
  }
  return o;
}

bool wordByte(const string& s, size_t k) { return k < s.size() && (isalnum(uint8_t(s[k])) || uint8_t(s[k]) >= 0x80); }

size_t findClose(const string& s, size_t from, const string& m, bool word) {
  for (size_t k = from; (k = s.find(m, k)) != string::npos; k++) {
    if (k == 0 || s[k - 1] == ' ' || s[k - 1] == '\\' || s[k - 1] == '\n') continue;
    if (m.size() == 1 && k + 1 < s.size() && s[k + 1] == m[0]) continue;
    if (word && wordByte(s, k + m.size())) continue;
    return k;
  }
  return string::npos;
}

string entity(const string& name) {
  static const struct { const char* n; const char* v; } tab[] = {
      {"nbsp", "\xC2\xA0"}, {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"ndash", "\xE2\x80\x93"},
      {"mdash", "\xE2\x80\x94"}, {"hellip", "\xE2\x80\xA6"}, {"deg", "\xC2\xB0"}, {"times", "\xC3\x97"}, {"le", "\xE2\x89\xA4"},
      {"ge", "\xE2\x89\xA5"}, {"ne", "\xE2\x89\xA0"}, {"plusmn", "\xC2\xB1"}, {"micro", "\xC2\xB5"}, {"copy", "\xC2\xA9"},
      {"lsquo", "\xE2\x80\x98"}, {"rsquo", "\xE2\x80\x99"}, {"ldquo", "\xE2\x80\x9C"}, {"rdquo", "\xE2\x80\x9D"}};
  for (auto& e : tab) if (name == e.n) return e.v;
  if (name.size() > 1 && name[0] == '#') {
    long cp = name[1] == 'x' || name[1] == 'X' ? strtol(name.c_str() + 2, nullptr, 16) : strtol(name.c_str() + 1, nullptr, 10);
    if (cp >= 0x20 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF)) {
      string o;
      if (cp < 0x80) o += char(cp);
      else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
      else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
      else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
      return o;
    }
  }
  return "";
}

size_t urlEnd(const string& s, size_t i) {
  size_t e = i;
  while (e < s.size() && !isspace(uint8_t(s[e])) && s[e] != '<' && s[e] != '>' && s[e] != '"' && s[e] != '|') e++;
  while (e > i) {
    char c = s[e - 1];
    if (strchr(".,;:!?'\"]}*", c)) { e--; continue; }
    if (c == ')') {
      int open = 0, close = 0;
      for (size_t k = i; k < e; k++) { if (s[k] == '(') open++; else if (s[k] == ')') close++; }
      if (close > open) { e--; continue; }
    }
    break;
  }
  return e;
}

size_t doiEnd(const string& s, size_t i) {
  if (s.compare(i, 3, "10.") != 0) return i;
  size_t k = i + 3, d = 0;
  while (k < s.size() && isdigit(uint8_t(s[k]))) { k++; d++; }
  if (d < 4 || d > 9 || k >= s.size() || s[k] != '/') return i;
  size_t e = urlEnd(s, k + 1);
  return e > k + 1 ? e : i;
}

string absoluteUrl(string url) {
  url = trim(url);
  if (startsWith(lower(url), "doi.org/")) return "https://" + url;
  if (startsWith(url, "10.") && doiEnd(url, 0) == url.size()) return "https://doi.org/" + url;
  if (startsWith(lower(url), "www.")) return "https://" + url;
  string l = lower(url);
  if (startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "mailto:")) {
    for (unsigned char c : url) if (c < 0x21 && c != ' ') return "";
    return url;
  }
  return "";
}

void parseInline(const string& s, uint16_t base, const string& link, bool md, bool autolink, vector<Span>& out) {
  string cur;
  auto flush = [&]() { if (!cur.empty()) { out.push_back({cur, base, link}); cur.clear(); } };
  auto sub = [&](const string& text, uint16_t f, const string& l) { flush(); parseInline(text, f, l, md, autolink, out); };
  size_t n = s.size();
  for (size_t i = 0; i < n;) {
    char c = s[i];
    if (md && c == '\\' && i + 1 < n && (s[i + 1] == '(' || s[i + 1] == '[')) {
      const string close = s[i + 1] == '(' ? "\\)" : "\\]";
      size_t e = s.find(close, i + 2);
      if (e != string::npos && e - i <= 32772) {
        flush();
        out.push_back({s.substr(i + 2, e - i - 2), uint16_t(base | F_MATH), link});
        i = e + close.size();
        continue;
      }
    }
    if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|>~<", s[i + 1])) { cur += s[i + 1]; i += 2; continue; }
    if (c == '&') {
      size_t e = s.find(';', i);
      if (e != string::npos && e - i <= 8) {
        string v = entity(s.substr(i + 1, e - i - 1));
        if (!v.empty()) { cur += v; i = e + 1; continue; }
      }
    }
    if (c == '<') {
      size_t e = s.find('>', i);
      if (e != string::npos && e - i <= 12) {
        string tag = lower(trim(s.substr(i + 1, e - i - 1)));
        while (!tag.empty() && tag.back() == '/') tag.pop_back();
        tag = trim(tag);
        if (tag == "br") { cur += '\n'; i = e + 1; continue; }
        static const struct { const char* t; uint16_t f; } tags[] = {{"b", F_BOLD}, {"strong", F_BOLD}, {"i", F_ITALIC}, {"em", F_ITALIC}, {"u", F_UNDER}, {"sub", F_SUB}, {"sup", F_SUP}, {"s", F_STRIKE}, {"del", F_STRIKE}, {"code", F_CODE}, {"mark", F_MARK}};
        bool handled = false;
        for (auto& t : tags) {
          if (tag != t.t) continue;
          size_t close = string::npos;
          for (size_t k = e + 1; (k = s.find("</", k)) != string::npos; k++) {
            size_t ce = s.find('>', k);
            if (ce == string::npos) break;
            if (lower(trim(s.substr(k + 2, ce - k - 2))) == tag) { close = k; break; }
          }
          if (close == string::npos) { i = e + 1; handled = true; break; }
          sub(s.substr(e + 1, close - e - 1), uint16_t(base | t.f), link);
          i = s.find('>', close) + 1;
          handled = true;
          break;
        }
        if (handled) continue;
        if (autolink && e > i + 8) {  // <https://...>
          string u = absoluteUrl(s.substr(i + 1, e - i - 1));
          if (!u.empty()) { flush(); out.push_back({s.substr(i + 1, e - i - 1), base, u}); i = e + 1; continue; }
        }
      }
    }
    if (md) {
      if (c == '`') {
        size_t e = s.find('`', i + 1);
        if (e != string::npos && e > i + 1) { flush(); out.push_back({s.substr(i + 1, e - i - 1), uint16_t(base | F_CODE), link}); i = e + 1; continue; }
      }
      if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c && i + 2 < n && s[i + 2] != ' ' && s[i + 2] != c) {
        size_t e = findClose(s, i + 2, string(2, c), c == '_');
        if (e != string::npos && e > i + 2) { sub(s.substr(i + 2, e - i - 2), uint16_t(base | F_BOLD), link); i = e + 2; continue; }
      }
      if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] != ' ' && s[i + 1] != c && (c == '*' || !wordByte(s, i == 0 ? n : i - 1))) {
        size_t e = findClose(s, i + 1, string(1, c), c == '_');
        if (e != string::npos && e > i + 1) { sub(s.substr(i + 1, e - i - 1), uint16_t(base | F_ITALIC), link); i = e + 1; continue; }
      }
      if (c == '~' && i + 2 < n && s[i + 1] == '~' && s[i + 2] != ' ') {
        size_t e = findClose(s, i + 2, "~~", false);
        if (e != string::npos && e > i + 2) { sub(s.substr(i + 2, e - i - 2), uint16_t(base | F_STRIKE), link); i = e + 2; continue; }
      }
      if (c == '=' && i + 2 < n && s[i + 1] == '=' && s[i + 2] != ' ') {
        size_t e = findClose(s, i + 2, "==", false);
        if (e != string::npos && e > i + 2) { sub(s.substr(i + 2, e - i - 2), uint16_t(base | F_MARK), link); i = e + 2; continue; }
      }
      if (c == '[' && i + 1 < n && s[i + 1] == '@') {  // [@doi:10.1/x;@t:abc] citation field (keys of Document::refs)
        size_t e = s.find(']', i + 2);
        if (e != string::npos && s.find('\n', i) > e) {
          vector<string> keys;
          for (auto& k0 : split(s.substr(i + 1, e - i - 1), ';')) { string k = trim(k0); if (!k.empty() && k[0] == '@') k = trim(k.substr(1)); if (!k.empty()) keys.push_back(k); }
          if (!keys.empty()) {
            flush();
            Span cs;
            cs.fmt = base;
            cs.cite = join(keys, ";");
            cs.text = "[?]";
            out.push_back(cs);
            i = e + 1;
            continue;
          }
        }
      }
      if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
        size_t b = c == '!' ? i + 1 : i;
        size_t e = s.find(']', b + 1);
        if (e != string::npos && e + 1 < n && s[e + 1] == '(' && s.find('\n', b) > e) {
          size_t ce = string::npos;
          for (size_t k = e + 2, depth = 0; k < n && s[k] != '\n'; k++) {
            if (s[k] == '(') depth++;
            else if (s[k] == ')') { if (!depth) { ce = k; break; } depth--; }
          }
          if (ce != string::npos) {
            string text = s.substr(b + 1, e - b - 1), url = trim(s.substr(e + 2, ce - e - 2));
            size_t sp = url.find(' ');
            if (sp != string::npos && sp + 1 < url.size() && (url[sp + 1] == '"' || url[sp + 1] == '\'')) url = url.substr(0, sp);
            if (c == '!') { cur += text; i = ce + 1; continue; }
            string u = absoluteUrl(url);
            sub(text, base, u.empty() ? link : u);
            i = ce + 1;
            continue;
          }
        }
      }
    }
    if (autolink && link.empty() && (c == 'h' || c == 'w' || c == '1') && (i == 0 || !wordByte(s, i - 1))) {
      bool url = s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0 || s.compare(i, 4, "www.") == 0;
      size_t e = url ? urlEnd(s, i) : doiEnd(s, i);
      if (e > i + 8) {
        string t = s.substr(i, e - i);
        flush();
        out.push_back({t, base, url ? (startsWith(lower(t), "www.") ? "https://" + t : t) : "https://doi.org/" + t});
        i = e;
        continue;
      }
    }
    cur += c;
    i++;
  }
  flush();
}

// ---------------------------------------------------------------- block Markdown
struct Blk {
  enum T { Para, Heading, Item, Quote, Code, Rule, Table, Caption } t = Para;
  int level = 0;
  bool ordered = false, restart = false;
  int number = 1;
  string text;
  vector<string> lines;
  vector<vector<string>> rows;
  vector<char> align;
  bool header = true;
};

bool isRule(const string& t) {
  if (t.size() < 3) return false;
  char c = t[0];
  if (c != '-' && c != '*' && c != '_') return false;
  int k = 0;
  for (char x : t) { if (x == c) k++; else if (x != ' ') return false; }
  return k >= 3;
}

int headingMarks(const string& t, string& rest) {
  size_t k = 0;
  while (k < t.size() && k < 6 && t[k] == '#') k++;
  if (!k || k >= t.size() || t[k] != ' ') return 0;
  rest = trim(t.substr(k));
  while (!rest.empty() && rest.back() == '#') rest.pop_back();
  rest = trim(rest);
  return int(k);
}

int leadingWidth(const string& raw, size_t& k) {
  int w = 0;
  for (k = 0; k < raw.size() && (raw[k] == ' ' || raw[k] == '\t'); k++) w += raw[k] == '\t' ? 4 : 1;
  return w;
}

bool listItem(const string& raw, int& indent, bool& ordered, int& number, string& content) {
  size_t k;
  indent = leadingWidth(raw, k);
  string r = raw.substr(k);
  if (r.size() >= 2 && (r[0] == '-' || r[0] == '*' || r[0] == '+') && (r[1] == ' ' || r[1] == '\t')) { ordered = false; number = 0; content = trim(r.substr(2)); return true; }
  if (r.size() >= 4 && r.compare(0, 3, "\xE2\x80\xA2") == 0 && r[3] == ' ') { ordered = false; number = 0; content = trim(r.substr(4)); return true; }
  size_t d = 0;
  while (d < r.size() && d < 3 && isdigit(uint8_t(r[d]))) d++;
  if (d > 0 && d + 1 < r.size() && (r[d] == '.' || r[d] == ')') && (r[d + 1] == ' ' || r[d + 1] == '\t')) { ordered = true; number = toInt(r.substr(0, d), 1); content = trim(r.substr(d + 2)); return true; }
  return false;
}

bool hasPipe(const string& t) {
  for (size_t i = 0; i < t.size(); i++) if (t[i] == '|' && (i == 0 || t[i - 1] != '\\')) return true;
  return false;
}

vector<string> splitCells(const string& line) {
  string t = trim(line);
  if (!t.empty() && t.front() == '|') t.erase(0, 1);
  if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
  vector<string> cells;
  string cur;
  for (size_t i = 0; i < t.size(); i++) {
    if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') { cur += '|'; i++; continue; }
    if (t[i] == '|') { cells.push_back(trim(cur)); cur.clear(); continue; }
    cur += t[i];
  }
  cells.push_back(trim(cur));
  return cells;
}

bool isSepRow(const string& t, vector<char>* align = nullptr) {
  if (!hasPipe(t) || t.find('-') == string::npos) return false;
  vector<string> cells = splitCells(t);
  if (cells.empty()) return false;
  vector<char> al;
  for (auto& c : cells) {
    if (c.empty()) return false;
    size_t a = 0, b = c.size();
    bool l = c[0] == ':', r = c.size() > 1 && c.back() == ':';
    if (l) a++;
    if (r) b--;
    if (b <= a) return false;
    for (size_t k = a; k < b; k++) if (c[k] != '-') return false;
    al.push_back(l && r ? 'c' : r ? 'r' : l ? 'l' : 'a');
  }
  if (align) *align = al;
  return true;
}

bool tableStart(const vector<string>& lines, size_t i) {
  string t = trim(lines[i]);
  if (!hasPipe(t)) return false;
  string next = i + 1 < lines.size() ? trim(lines[i + 1]) : string();
  if (isSepRow(t)) return hasPipe(next) && !isSepRow(next);
  if (isSepRow(next)) return true;
  return t[0] == '|' && !next.empty() && next[0] == '|';
}

string cleanTitle(string s) {
  s = lower(trim(s));
  size_t k = 0;
  while (k < s.size() && (isdigit(uint8_t(s[k])) || s[k] == '.' || s[k] == ' ')) k++;
  if (k < s.size()) s = s.substr(k);
  while (!s.empty() && (s.back() == ':' || s.back() == '.' || s.back() == ' ')) s.pop_back();
  if (s.size() > 4 && startsWith(s, "**") && endsWith(s, "**")) s = s.substr(2, s.size() - 4);
  return s;
}

vector<Blk> parseBlocks(const string& text, const string& dropTitle) {
  vector<Blk> out;
  vector<string> lines = split(replaceAll(text, "\r", ""), '\n', true);
  string pbuf;
  bool prevBreak = false, afterBlank = false, prevQuote = false;
  vector<int> indents;
  bool inNumbered = false, lastNumbered0 = false;
  auto flushPara = [&]() {
    string t = trim(pbuf);
    if (!t.empty()) { Blk b; b.text = t; out.push_back(b); }
    pbuf.clear();
    prevBreak = false;
  };
  auto endList = [&]() { indents.clear(); inNumbered = false; lastNumbered0 = false; };
  for (size_t i = 0; i < lines.size(); i++) {
    const string& raw = lines[i];
    string t = trim(raw);
    if (t.empty()) { flushPara(); afterBlank = true; prevQuote = false; continue; }
    bool blank = afterBlank;
    afterBlank = false;
    if (startsWith(t, "```") || startsWith(t, "~~~")) {
      flushPara();
      endList();
      string fence = t.substr(0, 3);
      Blk b;
      b.t = Blk::Code;
      for (i++; i < lines.size() && !startsWith(trim(lines[i]), fence); i++) {
        string l = lines[i];
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
        b.lines.push_back(l);
      }
      out.push_back(b);
      prevQuote = false;
      continue;
    }
    if (isRule(t)) { flushPara(); endList(); Blk b; b.t = Blk::Rule; out.push_back(b); prevQuote = false; continue; }
    string rest;
    if (int hl = headingMarks(t, rest)) {
      flushPara();
      endList();
      Blk b;
      b.t = Blk::Heading;
      b.level = hl;
      b.text = rest;
      out.push_back(b);
      prevQuote = false;
      continue;
    }
    if (tableStart(lines, i)) {
      flushPara();
      endList();
      Blk b;
      b.t = Blk::Table;
      bool sawSep = false;
      size_t j = i;
      for (; j < lines.size(); j++) {
        string u = trim(lines[j]);
        if (u.empty() || !hasPipe(u)) break;
        vector<char> al;
        if (isSepRow(u, &al)) {
          if (!sawSep && b.rows.size() <= 1) { b.align = al; sawSep = true; b.header = b.rows.size() == 1; }
          continue;
        }
        b.rows.push_back(splitCells(u));
      }
      if (!sawSep) b.header = b.rows.size() >= 2;
      if (!b.rows.empty()) out.push_back(b);
      i = j - 1;
      prevQuote = false;
      continue;
    }
    if (t[0] == '>') {
      flushPara();
      endList();
      string q = trim(t.substr(1));
      if (prevQuote && !out.empty() && out.back().t == Blk::Quote && !blank) out.back().text += (q.empty() ? "" : " ") + q;
      else if (!q.empty()) { Blk b; b.t = Blk::Quote; b.text = q; out.push_back(b); }
      prevQuote = true;
      continue;
    }
    prevQuote = false;
    int indent = 0, number = 0;
    bool ordered = false;
    string content;
    if (listItem(raw, indent, ordered, number, content)) {
      flushPara();
      while (!indents.empty() && indent < indents.back() - 1) indents.pop_back();
      if (indents.empty() || indent > indents.back() + 1) indents.push_back(indent);
      Blk b;
      b.t = Blk::Item;
      b.level = std::min<int>(2, int(indents.size()) - 1);
      b.ordered = ordered;
      b.number = number;
      b.text = content;
      if (ordered) {
        b.restart = !inNumbered || (b.level == 0 && number == 1 && lastNumbered0);
        inNumbered = true;
        if (b.level == 0) lastNumbered0 = true;
      }
      out.push_back(b);
      continue;
    }
    if (!indents.empty() && indent >= 2 && !out.empty() && out.back().t == Blk::Item) { out.back().text += (blank ? "\n" : " ") + t; continue; }
    endList();
    bool hardBreak = raw.size() >= 2 && raw.compare(raw.size() - 2, 2, "  ") == 0;
    string piece = t;
    if (endsWith(piece, "\\")) { piece.pop_back(); hardBreak = true; }
    if (!pbuf.empty()) pbuf += prevBreak ? "\n" : " ";
    pbuf += piece;
    prevBreak = hardBreak;
  }
  flushPara();
  for (auto& b : out) {
    if (b.t != Blk::Para || b.text.size() <= 4 || b.text.size() > 120 || !startsWith(b.text, "**") || !endsWith(b.text, "**")) continue;
    if (b.text.find("**", 2) != b.text.size() - 2) continue;
    b.t = Blk::Heading;
    b.level = 99;
    b.text = b.text.substr(2, b.text.size() - 4);
  }
  for (size_t k = 0; k < out.size(); k++) {
    if (out[k].t != Blk::Table) continue;
    for (size_t j : {k - 1, k + 1}) {
      if (j >= out.size() || out[j].t != Blk::Para) continue;
      string t = lower(out[j].text);
      if (startsWith(t, "**")) t = t.substr(2);
      if (startsWith(t, "table ") && t.size() > 6 && isdigit(uint8_t(t[6])) && out[j].text.size() < 300) out[j].t = Blk::Caption;
    }
  }
  if (!dropTitle.empty() && !out.empty() && out[0].t == Blk::Heading && cleanTitle(out[0].text) == cleanTitle(dropTitle)) out.erase(out.begin());
  return out;
}

Para paraOf(const string& md, PStyle st, bool markdown = true) {
  Para p;
  p.style = st;
  p.spans = spansFromMarkdown(md, markdown, true);
  return p;
}

// ---------------------------------------------------------------- Markdown out
string escapeMd(const string& t) {
  string o;
  for (char c : t) { if (c == '*' || c == '_' || c == '`' || c == '[' || c == '#' || c == '|' || c == '~' || c == '<' || c == '\\') o += '\\'; o += c; }
  return o;
}

}  // namespace

// ---------------------------------------------------------------- public: Markdown
vector<Span> spansFromMarkdown(const string& line, bool markdown, bool autolink) {
  vector<Span> out;
  parseInline(repairUtf8(line), 0, "", markdown, autolink, out);
  Para p;
  p.spans = out;
  p.normalize();
  return p.spans;
}

vector<Block> blocksFromMarkdown(const string& markdown, int headingBase, const string& dropTitle) {
  vector<Blk> bl = parseBlocks(repairUtf8(markdown), dropTitle);
  int minLevel = 7;
  for (auto& b : bl) if (b.t == Blk::Heading && b.level < 99) minLevel = std::min(minLevel, b.level);
  headingBase = clampv(headingBase, 1, 4);
  vector<Block> out;
  for (size_t k = 0; k < bl.size(); k++) {
    const Blk& b = bl[k];
    Block nb;
    switch (b.t) {
      case Blk::Para: nb.p = paraOf(b.text, PStyle::Body); break;
      case Blk::Heading: {
        int h = b.level == 99 ? std::min(4, headingBase + 1) : std::min(4, headingBase + b.level - (minLevel == 7 ? 1 : minLevel));
        nb.p = paraOf(b.text, headingStyle(h));
        break;
      }
      case Blk::Item:
        nb.p = paraOf(b.text, b.ordered ? PStyle::Number : PStyle::Bullet);
        nb.p.level = b.level;
        nb.p.numStart = b.ordered && b.restart ? std::max(1, b.number) : 0;
        break;
      case Blk::Quote: nb.p = paraOf(b.text, PStyle::Quote); break;
      case Blk::Code: {
        nb.p.style = PStyle::Code;
        string t = join(b.lines, "\n");
        if (!t.empty()) nb.p.spans.push_back({repairUtf8(t), 0, ""});
        break;
      }
      case Blk::Rule: nb.kind = Block::Rule; break;
      case Blk::Caption: nb.p = paraOf(b.text, PStyle::Caption); break;
      case Blk::Table: {
        nb.kind = Block::TableBlock;
        size_t cols = 0;
        for (auto& r : b.rows) cols = std::max(cols, r.size());
        for (auto& r : b.rows) {
          vector<Para> row;
          for (size_t c = 0; c < cols; c++) row.push_back(paraOf(c < r.size() ? r[c] : string(), PStyle::Body));
          nb.tbl.cells.push_back(row);
        }
        nb.tbl.align.assign(cols, 'a');
        for (size_t c = 0; c < cols && c < b.align.size(); c++) nb.tbl.align[c] = b.align[c];
        nb.tbl.header = b.header;
        nb.tbl.fixup();
        break;
      }
    }
    out.push_back(std::move(nb));
  }
  return out;
}

void docAppendMarkdown(Document& d, const string& markdown, int headingBase, const string& dropTitle) {
  vector<Block> bl = blocksFromMarkdown(markdown, headingBase, dropTitle);
  // an empty trailing paragraph is replaced rather than kept above the new text
  if (!d.blocks.empty() && d.blocks.back().kind == Block::Paragraph && d.blocks.back().p.empty() && d.blocks.back().p.style == PStyle::Body) d.blocks.pop_back();
  d.blocks.insert(d.blocks.end(), bl.begin(), bl.end());
}

string spansToMarkdown(const vector<Span>& spans) {
  string o;
  for (auto& s : spans) {
    if (s.isCite()) {  // citation fields round-trip as [@key;@key]
      vector<string> ks;
      for (auto& k : split(s.cite, ';')) if (!trim(k).empty()) ks.push_back("@" + trim(k));
      o += "[" + join(ks, ";") + "]";
      continue;
    }
    if (s.fmt & F_MATH) { o += "\\(" + s.text + "\\)"; continue; }
    string t = s.fmt & F_CODE ? "`" + s.text + "`" : escapeMd(s.text);
    if (s.fmt & F_BOLD) t = "**" + t + "**";
    if (s.fmt & F_ITALIC) t = "*" + t + "*";
    if (s.fmt & F_STRIKE) t = "~~" + t + "~~";
    if (s.fmt & F_UNDER) t = "<u>" + t + "</u>";
    if (s.fmt & F_SUB) t = "<sub>" + t + "</sub>";
    if (s.fmt & F_SUP) t = "<sup>" + t + "</sup>";
    if (s.fmt & F_MARK) t = "==" + t + "==";
    if (!s.link.empty()) t = "[" + t + "](" + s.link + ")";
    o += t;
  }
  return o;
}

string paraToMarkdown(const Para& p) {
  string body = spansToMarkdown(p.spans);
  string ind(size_t(p.level) * 2, ' ');
  switch (p.style) {
    case PStyle::Title: return "# " + body;
    case PStyle::Subtitle: return "*" + body + "*";
    case PStyle::Meta: return body;
    case PStyle::H1: return "## " + body;
    case PStyle::H2: return "### " + body;
    case PStyle::H3: return "#### " + body;
    case PStyle::H4: return "##### " + body;
    case PStyle::Bullet: return ind + "- " + body;
    case PStyle::Number: return ind + std::to_string(std::max(1, p.numStart)) + ". " + body;
    case PStyle::Quote: return "> " + replaceAll(body, "\n", "\n> ");
    case PStyle::Code: return "```\n" + p.text() + "\n```";
    case PStyle::Caption: return "*" + body + "*";
    case PStyle::Reference: return body;
    case PStyle::Body: return ind.empty() ? body : ind + body;
  }
  return body;
}

vector<int> docListNumbers(const Document& d) {
  vector<int> nums(d.blocks.size(), 0);
  int counters[3] = {0, 0, 0};
  bool inList = false;
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& b = d.blocks[k];
    if (b.kind != Block::Paragraph || !isList(b.p.style)) { inList = false; counters[0] = counters[1] = counters[2] = 0; continue; }
    if (b.p.style != PStyle::Number) continue;
    int L = clampv(b.p.level, 0, 2);
    if (!inList || b.p.numStart > 0) { counters[L] = b.p.numStart > 0 ? b.p.numStart : 1; }
    else counters[L]++;
    for (int j = L + 1; j < 3; j++) counters[j] = 0;
    inList = true;
    nums[k] = counters[L];
  }
  return nums;
}

vector<string> docHeadingNumbers(const Document& d) {
  vector<string> out(d.blocks.size());
  int c[5] = {0, 0, 0, 0, 0};
  bool refs = false;
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& b = d.blocks[k];
    if (b.kind != Block::Paragraph || !isHeading(b.p.style)) continue;
    int L = headingLevel(b.p.style);
    string t = lower(trim(b.p.text()));
    if (L == 1 && (t == "references" || t == "bibliography" || t == "acknowledgements" || t == "appendix")) { refs = true; continue; }
    if (refs) continue;
    c[L]++;
    for (int j = L + 1; j <= 4; j++) c[j] = 0;
    string s;
    for (int j = 1; j <= L; j++) s += (j > 1 ? "." : "") + std::to_string(std::max(1, c[j]));
    out[k] = s;
  }
  return out;
}

string blocksToMarkdown(const Document& d) {
  string o;
  vector<int> nums = docListNumbers(d);
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& b = d.blocks[k];
    if (b.kind == Block::Paragraph) {
      Para p = b.p;
      if (p.style == PStyle::Number && nums[k] > 0) p.numStart = nums[k];
      if (p.style == PStyle::Reference) o += "[" + std::to_string(d.refNumber(int(k))) + "] " + paraToMarkdown(p) + "\n\n";
      else o += paraToMarkdown(p) + "\n" + (isList(p.style) && k + 1 < d.blocks.size() && d.blocks[k + 1].kind == Block::Paragraph && isList(d.blocks[k + 1].p.style) ? "" : "\n");
    } else if (b.kind == Block::FigureBlock) {
      o += "**Figure " + std::to_string(d.figureNumber(int(k))) + ".** " + spansToMarkdown(b.fig.caption.spans) + "\n\n";
    } else if (b.kind == Block::TableBlock) {
      const Table& t = b.tbl;
      for (int r = 0; r < t.rows(); r++) {
        o += "|";
        for (int c = 0; c < t.cols(); c++) o += " " + replaceAll(replaceAll(spansToMarkdown(t.cells[size_t(r)][size_t(c)].spans), "\n", "<br>"), "|", "\\|") + " |";
        o += "\n";
        if (r == 0) {
          o += "|";
          for (int c = 0; c < t.cols(); c++) { char a = c < int(t.align.size()) ? t.align[size_t(c)] : 'a'; o += a == 'c' ? ":---:|" : a == 'r' ? "---:|" : a == 'l' ? ":---|" : "---|"; }
          o += "\n";
        }
      }
      o += "\n";
    } else if (b.kind == Block::Rule) o += "---\n\n";
    else if (b.kind == Block::PageBreak) o += "<!-- page break -->\n\n";
    else if (b.kind == Block::Toc) o += "[TOC]\n\n";
  }
  return o;
}

// ---------------------------------------------------------------- the assistant's report
Document docFromReport(const ReportDoc& r) {
  Document d;
  d.created = nowIso();
  d.blocks.push_back(Block::para(r.title.empty() ? string("Literature report") : r.title, PStyle::Title));
  if (!r.subtitle.empty()) d.blocks.push_back(Block::para(r.subtitle, PStyle::Subtitle));
  for (auto& m : r.meta) d.blocks.push_back(Block::para(m, PStyle::Meta));
  if (!r.date.empty()) d.blocks.push_back(Block::para(r.date, PStyle::Meta));
  for (auto& b : r.blocks) {
    if (b.kind == ReportBlock::Section) {
      if (!trim(b.title).empty()) { Block h; h.p = paraOf(b.title, PStyle::H1); d.blocks.push_back(h); }
      docAppendMarkdown(d, b.text, 2, b.title);
    } else {
      if (b.fig.W <= 0 || b.fig.H <= 0) continue;
      int a = d.addAsset(b.fig, b.figId, b.title, "");
      Block f;
      f.kind = Block::FigureBlock;
      f.fig.asset = a;
      string cap = b.title;
      if (!b.text.empty()) cap += (cap.empty() ? "" : ". ") + b.text;
      f.fig.caption = paraOf(cap, PStyle::Caption);
      f.fig.alt = cap;
      d.blocks.push_back(f);
    }
  }
  if (!r.references.empty()) {
    d.blocks.push_back(Block::para("References", PStyle::H1));
    for (auto& ref : r.references) { Block p; p.p = paraOf(ref, PStyle::Reference, false); d.blocks.push_back(p); }
  }
  return d;
}

string docOutlineText(const Document& d) {
  string s;
  vector<string> hn = docHeadingNumbers(d);
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& b = d.blocks[k];
    if (b.kind == Block::Paragraph && (isHeading(b.p.style) || b.p.style == PStyle::Title)) {
      int L = b.p.style == PStyle::Title ? 0 : headingLevel(b.p.style);
      s += string(size_t(L) * 2, ' ') + (L ? "- " : "") + b.p.text() + "\n";
    } else if (b.kind == Block::FigureBlock) s += "  [Figure " + std::to_string(d.figureNumber(int(k))) + ": " + truncate(b.fig.caption.text(), 100) + "]\n";
    else if (b.kind == Block::TableBlock) s += "  [Table " + std::to_string(d.tableNumber(int(k))) + ": " + std::to_string(b.tbl.rows()) + " x " + std::to_string(b.tbl.cols()) + "]\n";
  }
  return s;
}

// ---------------------------------------------------------------- references and sections
namespace {
bool isRefsHeading(const Block& b) {
  if (b.kind != Block::Paragraph || !isHeading(b.p.style)) return false;
  string t = lower(trim(b.p.text()));
  return t == "references" || t == "bibliography" || t == "reference list";
}
}  // namespace

int docBodyEnd(const Document& d) {
  for (size_t k = 0; k < d.blocks.size(); k++) if (isRefsHeading(d.blocks[k])) return int(k);
  return int(d.blocks.size());
}

int docAddReference(Document& d, const string& text0, const string& url) {
  string text = trim(text0);
  if (text.empty()) return 0;
  RefEntry e;
  e.raw = text;
  e.url = trim(url);
  if (!e.url.empty() && e.raw.find(e.url) != string::npos) e.raw = trim(replaceAll(e.raw, e.url, ""));
  if (e.raw.empty()) e.raw = text;
  int at = docAddReference(d, e);
  if (at < 0) return 0;
  docRefreshCitations(d);  // the Reference paragraph exists from now on
  const string& key = d.refs[size_t(at)].key;
  for (size_t k = 0; k < d.blocks.size(); k++)
    if (d.blocks[k].kind == Block::Paragraph && d.blocks[k].p.style == PStyle::Reference && d.blocks[k].p.refKey == key) return d.refNumber(int(k));
  return at + 1;
}

string docResolveCitationKeys(const string& md, const std::function<string(int)>& keyFor) {
  string o;
  for (size_t i = 0; i < md.size();) {
    if (md[i] == '[' && i + 2 < md.size() && md[i + 1] == 'R' && isdigit(uint8_t(md[i + 2]))) {
      size_t e = md.find(']', i);
      if (e != string::npos && e - i < 80) {
        string inner = md.substr(i + 1, e - i - 1);
        bool ok = true;
        vector<string> keys;
        for (auto& part : splitAny(inner, ",;")) {
          string t = trim(part);
          if (t.size() < 2 || t[0] != 'R' || !isDigits(t.substr(1))) { ok = false; break; }
          string k = keyFor(toInt(t.substr(1), 0));
          if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
        }
        if (ok) {
          if (!keys.empty()) {
            string rep = "[";
            for (size_t k = 0; k < keys.size(); k++) rep += (k ? ";@" : "@") + keys[k];
            o += rep + "]";
          } else {
            while (!o.empty() && o.back() == ' ') o.pop_back();
          }
          i = e + 1;
          continue;
        }
      }
    }
    o += md[i];
    i++;
  }
  return o;
}

string docResolveCitations(const string& md, const std::function<int(int)>& numberFor) {
  string o;
  for (size_t i = 0; i < md.size();) {
    if (md[i] == '[' && i + 2 < md.size() && md[i + 1] == 'R' && isdigit(uint8_t(md[i + 2]))) {
      size_t e = md.find(']', i);
      if (e != string::npos && e - i < 80) {
        string inner = md.substr(i + 1, e - i - 1);
        bool ok = true;
        vector<int> nums;
        for (auto& part : splitAny(inner, ",;")) {
          string t = trim(part);
          if (t.size() < 2 || t[0] != 'R' || !isDigits(t.substr(1))) { ok = false; break; }
          int n = numberFor(toInt(t.substr(1), 0));
          if (n > 0 && std::find(nums.begin(), nums.end(), n) == nums.end()) nums.push_back(n);
        }
        if (ok) {
          if (!nums.empty()) {
            std::sort(nums.begin(), nums.end());
            string rep = "[";
            for (size_t k = 0; k < nums.size(); k++) rep += (k ? ", " : "") + std::to_string(nums[k]);
            o += rep + "]";
          } else {
            while (!o.empty() && o.back() == ' ') o.pop_back();  // "text [R99]" -> "text"
          }
          i = e + 1;
          continue;
        }
      }
    }
    o += md[i];
    i++;
  }
  return o;
}

vector<DocSection> docSections(const Document& d) {
  vector<DocSection> out;
  int end = docBodyEnd(d);
  DocSection cur;
  cur.heading = -1;
  cur.first = 0;
  for (int k = 0; k < end; k++) {
    const Block& b = d.blocks[size_t(k)];
    if (b.kind == Block::Paragraph && b.p.style == PStyle::H1) {
      cur.last = k - 1;
      if (cur.heading >= 0 || cur.last >= cur.first) out.push_back(cur);
      cur = DocSection();
      cur.heading = k;
      cur.first = k;
      cur.title = b.p.text();
    }
  }
  cur.last = end - 1;
  if (cur.heading >= 0 || cur.last >= cur.first) out.push_back(cur);
  return out;
}

string docSectionMarkdown(const Document& d, const DocSection& s) {
  Document part;
  part.setup = d.setup;
  part.assets = d.assets;
  for (int k = std::max(0, s.first); k <= s.last && k < int(d.blocks.size()); k++) {
    if (k == s.heading) continue;
    part.blocks.push_back(d.blocks[size_t(k)]);
  }
  return blocksToMarkdown(part);
}

void docReplaceBlocks(Document& d, int first, int last, const vector<Block>& with) {
  first = clampv(first, 0, int(d.blocks.size()));
  last = clampv(last, first - 1, int(d.blocks.size()) - 1);
  d.blocks.erase(d.blocks.begin() + first, d.blocks.begin() + last + 1);
  d.blocks.insert(d.blocks.begin() + first, with.begin(), with.end());
  if (d.blocks.empty()) d.blocks.push_back(Block::para(""));
}

int docInsertSection(Document& d, const string& title, const vector<Block>& body, int before) {
  int at = before >= 0 ? clampv(before, 0, int(d.blocks.size())) : docBodyEnd(d);
  // an empty trailing paragraph before the insertion point is replaced
  if (at > 0 && at <= int(d.blocks.size()) && d.blocks[size_t(at) - 1].kind == Block::Paragraph && d.blocks[size_t(at) - 1].p.empty() && d.blocks[size_t(at) - 1].p.style == PStyle::Body) { d.blocks.erase(d.blocks.begin() + at - 1); at--; }
  vector<Block> ins;
  if (!trim(title).empty()) { Block h; h.p = paraOf(title, PStyle::H1); ins.push_back(h); }
  ins.insert(ins.end(), body.begin(), body.end());
  d.blocks.insert(d.blocks.begin() + at, ins.begin(), ins.end());
  return trim(title).empty() ? -1 : at;
}

// ---------------------------------------------------------------- JSON
namespace {

Json paraJson(const Para& p) {
  Json j = Json::object();
  j.set("s", pstyleId(p.style));
  if (p.align != PAlign::Left) j.set("a", int(p.align));
  if (p.level) j.set("l", p.level);
  if (p.numStart) j.set("n", p.numStart);
  if (!p.refKey.empty()) { j.set("ref", p.refKey); j.set("gen", std::to_string(p.refGen)); }
  Json runs = Json::array();
  for (auto& s : p.spans) {
    Json r = Json::array();
    r.push(s.text);
    bool props = !s.font.empty() || s.size != 0 || s.color != 0 || !s.cite.empty();
    if (s.fmt || !s.link.empty() || props) r.push(int(s.fmt));
    if (!s.link.empty() || props) r.push(s.link);
    if (props) {
      Json x = Json::object();
      if (!s.font.empty()) x.set("font", s.font);
      if (s.size != 0) x.set("size", double(s.size));
      if (s.color != 0) x.set("color", std::to_string(s.color));
      if (!s.cite.empty()) x.set("cite", s.cite);
      r.push(x);
    }
    runs.push(r);
  }
  j.set("r", runs);
  return j;
}

Para paraFromJson(const Json& j) {
  Para p;
  pstyleFromId(j["s"].str("body"), p.style);
  p.align = PAlign(clampv(j["a"].integer(0), 0, 3));
  p.level = clampv(j["l"].integer(0), 0, 2);
  p.numStart = std::max(0, j["n"].integer(0));
  p.refKey = j["ref"].str();
  p.refGen = std::strtoull(j["gen"].str("0").c_str(), nullptr, 10);
  const Json& runs = j["r"];
  for (size_t i = 0; i < runs.size(); i++) {
    const Json& r = runs[i];
    Span s;
    s.text = r[0].str();
    s.fmt = uint16_t(r[1].integer(0));
    s.link = r[2].str();
    if (r.size() > 3 && r[3].t == Json::Obj) {
      s.font = r[3]["font"].str();
      s.size = float(r[3]["size"].num(0));
      s.color = uint32_t(std::strtoull(r[3]["color"].str("0").c_str(), nullptr, 10));
      s.cite = r[3]["cite"].str();
    }
    p.spans.push_back(s);
  }
  p.normalize();
  return p;
}

}  // namespace

Json docToJson(const Document& d, bool withAssets) {
  Json j = Json::object();
  Json bl = Json::array();
  for (auto& b : d.blocks) {
    Json x = Json::object();
    switch (b.kind) {
      case Block::Paragraph: x.set("k", "p"); x.set("p", paraJson(b.p)); break;
      case Block::TableBlock: {
        x.set("k", "t");
        Json rows = Json::array();
        for (auto& r : b.tbl.cells) { Json row = Json::array(); for (auto& c : r) row.push(paraJson(c)); rows.push(row); }
        x.set("cells", rows);
        x.set("align", string(b.tbl.align.begin(), b.tbl.align.end()));
        if (!b.tbl.widths.empty()) { Json w = Json::array(); for (float v : b.tbl.widths) w.push(double(v)); x.set("widths", w); }
        x.set("header", b.tbl.header);
        break;
      }
      case Block::FigureBlock:
        x.set("k", "f");
        x.set("asset", b.fig.asset);
        x.set("w", b.fig.widthPct);
        x.set("cap", paraJson(b.fig.caption));
        if (!b.fig.alt.empty()) x.set("alt", b.fig.alt);
        if (!b.fig.format.empty()) x.set("format", b.fig.format);
        if (!b.fig.label.empty()) x.set("label", b.fig.label);
        if (b.fig.captionAbove) x.set("capAbove", true);
        if (b.fig.border) x.set("border", true);
        if (b.fig.align != PAlign::Center) x.set("align", int(b.fig.align));
        break;
      case Block::Rule: x.set("k", "hr"); break;
      case Block::PageBreak: x.set("k", "pb"); break;
      case Block::Toc: x.set("k", "toc"); break;
    }
    bl.push(x);
  }
  j.set("blocks", bl);
  Json as = Json::array();
  for (auto& a : d.assets) {
    Json x = Json::object();
    x.set("id", a.id);
    x.set("title", a.title);
    if (!a.summary.empty()) x.set("summary", a.summary);
    if (withAssets) x.set("scene", sceneToBlob(a.scene));
    as.push(x);
  }
  j.set("assets", as);
  Json st = Json::object();
  st.set("paper", d.setup.paper);
  st.set("margins", d.setup.margins);
  st.set("font", d.setup.font);
  st.set("size", double(d.setup.baseSize));
  st.set("numbered", d.setup.numberedHeadings);
  st.set("toc", d.setup.toc);
  st.set("pageNumbers", d.setup.pageNumbers);
  st.set("lineSpacing", double(d.setup.lineSpacing));
  if (!d.setup.headingFont.empty()) st.set("headingFont", d.setup.headingFont);
  if (!d.setup.header.empty()) st.set("header", d.setup.header);
  st.set("figureFormat", d.setup.figureFormat);
  j.set("setup", st);
  j.set("citeStyle", d.citeStyle);
  Json refs = Json::array();
  for (auto& e : d.refs) {
    Json x = Json::object();
    x.set("key", e.key);
    if (!e.recordId.empty()) x.set("recordId", e.recordId);
    x.set("kind", e.kind);
    if (!e.authors.empty()) { Json a = Json::array(); for (auto& n : e.authors) a.push(n); x.set("authors", a); }
    auto put = [&](const char* k, const string& v) { if (!v.empty()) x.set(k, v); };
    put("year", e.year); put("title", e.title); put("container", e.container); put("volume", e.volume); put("issue", e.issue);
    put("pages", e.pages); put("publisher", e.publisher); put("place", e.place); put("doi", e.doi); put("url", e.url); put("raw", e.raw);
    if (e.rec >= 0) x.set("rec", e.rec);
    refs.push(x);
  }
  j.set("refs", refs);
  if (!d.author.empty()) j.set("author", d.author);
  if (!d.keywords.empty()) j.set("keywords", d.keywords);
  if (!d.created.empty()) j.set("created", d.created);
  return j;
}

bool docFromJson(Document& d, const Json& j) {
  d = Document();
  if (j.t != Json::Obj) return false;
  const Json& bl = j["blocks"];
  for (size_t i = 0; i < bl.size(); i++) {
    const Json& x = bl[i];
    string k = x["k"].str("p");
    Block b;
    if (k == "p") b.p = paraFromJson(x["p"]);
    else if (k == "t") {
      b.kind = Block::TableBlock;
      const Json& rows = x["cells"];
      for (size_t r = 0; r < rows.size(); r++) { vector<Para> row; for (size_t c = 0; c < rows[r].size(); c++) row.push_back(paraFromJson(rows[r][c])); b.tbl.cells.push_back(row); }
      string al = x["align"].str();
      b.tbl.align.assign(al.begin(), al.end());
      const Json& w = x["widths"];
      for (size_t c = 0; c < w.size(); c++) b.tbl.widths.push_back(float(w[c].num()));
      b.tbl.header = x["header"].boolean(true);
      b.tbl.fixup();
      if (b.tbl.rows() == 0 || b.tbl.cols() == 0) continue;
    } else if (k == "f") {
      b.kind = Block::FigureBlock;
      b.fig.asset = x["asset"].integer(-1);
      b.fig.widthPct = clampv(x["w"].integer(100), 20, 100);
      b.fig.caption = paraFromJson(x["cap"]);
      b.fig.caption.style = PStyle::Caption;
      b.fig.alt = x["alt"].str();
      b.fig.format = x["format"].str();
      b.fig.label = x["label"].str();
      b.fig.captionAbove = x["capAbove"].boolean(false);
      b.fig.border = x["border"].boolean(false);
      b.fig.align = PAlign(clampv(x["align"].integer(int(PAlign::Center)), 0, 3));
    } else if (k == "hr") b.kind = Block::Rule;
    else if (k == "pb") b.kind = Block::PageBreak;
    else if (k == "toc") b.kind = Block::Toc;
    else continue;
    d.blocks.push_back(std::move(b));
  }
  const Json& as = j["assets"];
  for (size_t i = 0; i < as.size(); i++) {
    FigAsset a;
    a.id = as[i]["id"].str();
    a.title = as[i]["title"].str();
    a.summary = as[i]["summary"].str();
    sceneFromBlob(as[i]["scene"].str(), a.scene);
    d.assets.push_back(std::move(a));
  }
  for (auto& b : d.blocks) if (b.kind == Block::FigureBlock && b.fig.asset >= int(d.assets.size())) b.fig.asset = -1;
  const Json& st = j["setup"];
  if (st.t == Json::Obj) {
    d.setup.paper = st["paper"].str("A4") == "Letter" ? "Letter" : "A4";
    d.setup.margins = st["margins"].str("normal");
    d.setup.font = st["font"].str("Calibri");
    d.setup.baseSize = float(clampv(st["size"].num(11), 8.0, 16.0));
    d.setup.numberedHeadings = st["numbered"].boolean(false);
    d.setup.toc = st["toc"].boolean(false);
    d.setup.pageNumbers = st["pageNumbers"].boolean(true);
    d.setup.lineSpacing = float(clampv(st["lineSpacing"].num(1.15), 1.0, 2.5));
    d.setup.headingFont = st["headingFont"].str();
    d.setup.header = st["header"].str();
    d.setup.figureFormat = st["figureFormat"].str("vector") == "png" ? "png" : "vector";
  }
  d.citeStyle = citeStyleInfo(j["citeStyle"].str("apa")).id;
  const Json& refs = j["refs"];
  for (size_t i = 0; i < refs.size(); i++) {
    const Json& x = refs[i];
    RefEntry e;
    e.key = x["key"].str();
    e.recordId = x["recordId"].str();
    e.kind = x["kind"].str("article");
    const Json& a = x["authors"];
    for (size_t k = 0; k < a.size(); k++) e.authors.push_back(a[k].str());
    e.year = x["year"].str(); e.title = x["title"].str(); e.container = x["container"].str(); e.volume = x["volume"].str(); e.issue = x["issue"].str();
    e.pages = x["pages"].str(); e.publisher = x["publisher"].str(); e.place = x["place"].str(); e.doi = x["doi"].str(); e.url = x["url"].str(); e.raw = x["raw"].str();
    e.rec = x["rec"].integer(-1);
    if (!e.key.empty()) d.refs.push_back(e);
  }
  // documents saved before 1.12 had plain "[n]" citations and free-text Reference paragraphs: adopt the paragraphs as entries
  if (d.refs.empty()) {
    for (auto& b : d.blocks) {
      if (b.kind != Block::Paragraph || b.p.style != PStyle::Reference || b.p.empty()) continue;
      RefEntry e;
      e.raw = b.p.text();
      for (auto& sp : b.p.spans) if (!sp.link.empty()) { e.url = sp.link; break; }
      if (!e.url.empty() && e.raw.find(e.url) != string::npos) e.raw = trim(replaceAll(e.raw, e.url, ""));
      e.key = "raw:" + sha256Hex(lower(collapseWs(trim(e.raw)))).substr(0, 16);
      b.p.refKey = e.key;
      b.p.refGen = 0;
      d.refs.push_back(e);
    }
    if (!d.refs.empty()) d.citeStyle = "ieee";  // keeps the [n] look those documents were written with
  }
  d.author = j["author"].str();
  d.keywords = j["keywords"].str();
  d.created = j["created"].str();
  if (d.blocks.empty()) d.blocks.push_back(Block::para(""));
  return true;
}

// ---------------------------------------------------------------- scene blob
// "VSC1" + little-endian fields; strings are length-prefixed; base64 for JSON.
namespace {
struct Writer {
  string o;
  void u8(uint8_t v) { o += char(v); }
  void u32(uint32_t v) { for (int k = 0; k < 4; k++) o += char((v >> (8 * k)) & 0xFF); }
  void f32(float v) { uint32_t u; memcpy(&u, &v, 4); u32(u); }
  void f64(double v) { uint64_t u; memcpy(&u, &v, 8); for (int k = 0; k < 8; k++) o += char((u >> (8 * k)) & 0xFF); }
  void str(const string& s) { u32(uint32_t(s.size())); o += s; }
  void col(const Color& c) { f32(c.r); f32(c.g); f32(c.b); f32(c.a); }
};
struct Reader {
  const string& s;
  size_t i = 0;
  bool ok = true;
  explicit Reader(const string& b) : s(b) {}
  uint8_t u8() { if (i + 1 > s.size()) { ok = false; return 0; } return uint8_t(s[i++]); }
  uint32_t u32() { if (i + 4 > s.size()) { ok = false; return 0; } uint32_t v = 0; for (int k = 0; k < 4; k++) v |= uint32_t(uint8_t(s[i + size_t(k)])) << (8 * k); i += 4; return v; }
  float f32() { uint32_t u = u32(); float v; memcpy(&v, &u, 4); return v; }
  double f64() { if (i + 8 > s.size()) { ok = false; return 0; } uint64_t u = 0; for (int k = 0; k < 8; k++) u |= uint64_t(uint8_t(s[i + size_t(k)])) << (8 * k); i += 8; double v; memcpy(&v, &u, 8); return v; }
  string str() { uint32_t n = u32(); if (!ok || i + n > s.size()) { ok = false; return ""; } string v = s.substr(i, n); i += n; return v; }
  Color col() { Color c; c.r = f32(); c.g = f32(); c.b = f32(); c.a = f32(); return c; }
};
}  // namespace

string sceneToBlob(const Scene& sc) {
  Writer w;
  w.o = "VSC1";
  w.f64(sc.W);
  w.f64(sc.H);
  w.u32(uint32_t(sc.items.size()));
  for (const Prim& p : sc.items) {
    w.u8(uint8_t(p.type));
    w.f32(p.x); w.f32(p.y); w.f32(p.w); w.f32(p.h); w.f32(p.r);
    uint8_t flags = uint8_t((p.fill ? 1 : 0) | (p.stroke ? 2 : 0) | (p.roundCap ? 4 : 0) | (p.evenOdd ? 8 : 0) | (p.bold ? 16 : 0) | (p.halo ? 32 : 0) | (p.sphere ? 64 : 0) | (p.italic ? 128 : 0));
    w.u8(flags);
    w.u8(uint8_t((p.mono ? 1 : 0) | (p.underline ? 2 : 0)));
    w.col(p.fillC); w.col(p.strokeC); w.col(p.haloC);
    w.f32(p.sw); w.f32(p.size); w.f32(p.haloW);
    w.u8(uint8_t(p.anchor));
    w.str(p.text); w.str(p.group); w.str(p.href);
    w.u32(uint32_t(p.dash.size()));
    for (float v : p.dash) w.f32(v);
    w.u32(uint32_t(p.d.size()));
    for (auto& c : p.d) { w.u8(uint8_t(c.op)); w.f32(c.x1); w.f32(c.y1); w.f32(c.x2); w.f32(c.y2); }
    w.u32(uint32_t(p.imgW)); w.u32(uint32_t(p.imgH));
    w.str(string(p.rgba.begin(), p.rgba.end()));
  }
  return base64(w.o);
}

bool sceneFromBlob(const string& blob, Scene& sc) {
  sc = Scene();
  string s = base64Decode(blob);
  if (s.size() < 4 || s.compare(0, 4, "VSC1") != 0) return false;
  Reader r(s);
  r.i = 4;
  sc.W = r.f64();
  sc.H = r.f64();
  uint32_t n = r.u32();
  if (!r.ok || n > 5000000) return false;
  sc.items.reserve(n);
  for (uint32_t k = 0; k < n && r.ok; k++) {
    Prim p;
    p.type = Prim::Type(clampv<int>(r.u8(), 0, 4));
    p.x = r.f32(); p.y = r.f32(); p.w = r.f32(); p.h = r.f32(); p.r = r.f32();
    uint8_t flags = r.u8(), f2 = r.u8();
    p.fill = flags & 1; p.stroke = flags & 2; p.roundCap = flags & 4; p.evenOdd = flags & 8; p.bold = flags & 16; p.halo = flags & 32; p.sphere = flags & 64; p.italic = flags & 128;
    p.mono = f2 & 1; p.underline = f2 & 2;
    p.fillC = r.col(); p.strokeC = r.col(); p.haloC = r.col();
    p.sw = r.f32(); p.size = r.f32(); p.haloW = r.f32();
    p.anchor = r.u8();
    p.text = r.str(); p.group = r.str(); p.href = r.str();
    uint32_t nd = r.u32();
    if (!r.ok || nd > 1000000) return false;
    for (uint32_t j = 0; j < nd; j++) p.dash.push_back(r.f32());
    uint32_t nc = r.u32();
    if (!r.ok || nc > 10000000) return false;
    p.d.reserve(nc);
    for (uint32_t j = 0; j < nc && r.ok; j++) { PathCmd c; c.op = char(r.u8()); c.x1 = r.f32(); c.y1 = r.f32(); c.x2 = r.f32(); c.y2 = r.f32(); p.d.push_back(c); }
    p.imgW = int(r.u32()); p.imgH = int(r.u32());
    string px = r.str();
    p.rgba.assign(px.begin(), px.end());
    if (!r.ok) return false;
    sc.items.push_back(std::move(p));
  }
  return r.ok;
}

}  // namespace vs
