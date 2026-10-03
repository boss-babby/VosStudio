// Word (.docx) writer. Everything here follows ECMA-376 Part 1 (WordprocessingML, transitional) as Word writes it:
// child elements are emitted in schema order (Word rejects out-of-order property elements as "unreadable content"),
// tabs and line breaks are <w:tab/> and <w:br/> (a literal tab inside <w:t> is not a tab), lists come from
// numbering.xml, captions carry SEQ fields, links are w:hyperlink relationships, and settings.xml declares
// compatibility mode 15 so Word lays the document out with its current engine instead of "Compatibility Mode".
#include "docx.h"

#include <cstring>

#include "common.h"
#include "doc.h"
#include "equation.h"

namespace vs {

// ------------------------------------------------------------ ZIP (stored / deflate)
uint32_t crc32Bytes(const string& s) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

namespace {
void le16(string& o, uint32_t v) { o += char(v & 0xFF); o += char((v >> 8) & 0xFF); }
void le32(string& o, uint32_t v) { le16(o, v & 0xFFFF); le16(o, (v >> 16) & 0xFFFF); }
const uint32_t kDosDate = ((2026u - 1980u) << 9) | (1u << 5) | 1u;  // 2026-01-01, a fixed stamp keeps output reproducible
}  // namespace

void ZipWriter::add(const string& name, const string& data, bool deflate) {
  string comp;
  uint16_t method = 0;
  if (deflate && data.size() > 256) {
    string z = zlibCompress(data);  // zlib wrapper: 2-byte header + raw deflate + 4-byte Adler-32
    if (z.size() > 6 && z.size() - 6 < data.size()) { comp = z.substr(2, z.size() - 6); method = 8; }
  }
  const string& payload = method ? comp : data;
  Entry e{name, crc32Bytes(data), uint32_t(data.size()), uint32_t(payload.size()), uint32_t(body_.size()), method};
  le32(body_, 0x04034b50);  // local file header
  le16(body_, 20);          // version needed
  le16(body_, 0x0800);      // flags: UTF-8 names
  le16(body_, method);
  le16(body_, 0);           // time
  le16(body_, kDosDate);
  le32(body_, e.crc);
  le32(body_, e.csize);
  le32(body_, e.size);
  le16(body_, uint32_t(name.size()));
  le16(body_, 0);
  body_ += name;
  body_ += payload;
  entries_.push_back(e);
}

string ZipWriter::finish() const {
  string o = body_, cd;
  for (auto& e : entries_) {
    le32(cd, 0x02014b50);
    le16(cd, 20);
    le16(cd, 20);
    le16(cd, 0x0800);
    le16(cd, e.method);
    le16(cd, 0);
    le16(cd, kDosDate);
    le32(cd, e.crc);
    le32(cd, e.csize);
    le32(cd, e.size);
    le16(cd, uint32_t(e.name.size()));
    le16(cd, 0);
    le16(cd, 0);
    le16(cd, 0);
    le16(cd, 0);
    le32(cd, 0);
    le32(cd, e.offset);
    cd += e.name;
  }
  o += cd;
  le32(o, 0x06054b50);
  le16(o, 0);
  le16(o, 0);
  le16(o, uint32_t(entries_.size()));
  le16(o, uint32_t(entries_.size()));
  le32(o, uint32_t(cd.size()));
  le32(o, uint32_t(body_.size()));
  le16(o, 0);
  return o;
}

// ------------------------------------------------------------ text and XML helpers
namespace {
const char* kW = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const char* kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const int kTextTwips = 9026;           // A4 minus 2 x 2.54 cm margins
const long long kTextEmu = 5731200;    // the same width in EMU (15.92 cm)
const long long kMaxFigEmu = 7560000;  // 21 cm: taller figures are scaled down so figure + caption stay on one page

// Valid UTF-8 without control characters (tab and newline stay: runs turn them into <w:tab/> and <w:br/>).
// Word refuses the whole file when one byte of a part is not well-formed XML, so this is not optional.
string cleanText(const string& s) {
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
    else { o += "\xEF\xBF\xBD"; i++; }
  }
  return o;
}

string esc(const string& s) {
  string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\t': case '\n': case '\r': o += ' '; break;
      default: if (c >= 0x20) o += char(c);
    }
  }
  return o;
}

string attr(const string& s) { return esc(cleanText(s)); }  // attribute value

// Run properties in schema order (rStyle, rFonts, b, bCs, i, iCs, strike, color, sz, szCs, u, shd, vertAlign).
struct RPr {
  string style, font, color, shd, vert;
  bool b = false, i = false, strike = false, u = false;
  int sz = 0;  // half-points
  string str() const {
    string p;
    if (!style.empty()) p += "<w:rStyle w:val=\"" + style + "\"/>";
    if (!font.empty()) p += "<w:rFonts w:ascii=\"" + font + "\" w:hAnsi=\"" + font + "\" w:cs=\"" + font + "\"/>";
    if (b) p += "<w:b/><w:bCs/>";
    if (i) p += "<w:i/><w:iCs/>";
    if (strike) p += "<w:strike/>";
    if (!color.empty()) p += "<w:color w:val=\"" + color + "\"/>";
    if (sz) p += "<w:sz w:val=\"" + std::to_string(sz) + "\"/><w:szCs w:val=\"" + std::to_string(sz) + "\"/>";
    if (u) p += "<w:u w:val=\"single\"/>";
    if (!shd.empty()) p += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"" + shd + "\"/>";
    if (!vert.empty()) p += "<w:vertAlign w:val=\"" + vert + "\"/>";
    return p.empty() ? string() : "<w:rPr>" + p + "</w:rPr>";
  }
};

// Paragraph properties in schema order (pStyle, keepNext, keepLines, pageBreakBefore, numPr, pBdr, shd, tabs,
// spacing, ind, contextualSpacing, jc, outlineLvl).
struct PPr {
  string style, numPr, pBdr, shd, tabs, spacing, ind, jc;
  bool keepNext = false, keepLines = false, pageBreak = false, contextual = false;
  int outline = -1;
  string str() const {
    string p;
    if (!style.empty()) p += "<w:pStyle w:val=\"" + style + "\"/>";
    if (keepNext) p += "<w:keepNext/>";
    if (keepLines) p += "<w:keepLines/>";
    if (pageBreak) p += "<w:pageBreakBefore/>";
    p += numPr + pBdr + shd + tabs + spacing + ind;
    if (contextual) p += "<w:contextualSpacing/>";
    if (!jc.empty()) p += "<w:jc w:val=\"" + jc + "\"/>";
    if (outline >= 0) p += "<w:outlineLvl w:val=\"" + std::to_string(outline) + "\"/>";
    return p.empty() ? string() : "<w:pPr>" + p + "</w:pPr>";
  }
};

string para(const string& runs, const PPr& pp = PPr()) { return "<w:p>" + pp.str() + runs + "</w:p>"; }

// One run; tabs and newlines inside the text become <w:tab/> and <w:br/>.
string textRun(const string& text, const RPr& rp = RPr()) {
  if (text.empty()) return "";
  string o = "<w:r>" + rp.str(), buf;
  auto flushT = [&]() {
    if (!buf.empty()) { o += "<w:t xml:space=\"preserve\">" + esc(buf) + "</w:t>"; buf.clear(); }
  };
  for (char c : text) {
    if (c == '\t') { flushT(); o += "<w:tab/>"; }
    else if (c == '\n') { flushT(); o += "<w:br/>"; }
    else buf += c;
  }
  flushT();
  return o + "</w:r>";
}

// A complex field with a cached result (Word shows the cached text until it updates the field).
string field(const string& instr, const string& cached, const RPr& rp = RPr()) {
  string r = rp.str();
  return "<w:r>" + r + "<w:fldChar w:fldCharType=\"begin\"/></w:r><w:r>" + r + "<w:instrText xml:space=\"preserve\"> " + esc(instr) +
         " </w:instrText></w:r><w:r>" + r + "<w:fldChar w:fldCharType=\"separate\"/></w:r><w:r>" + r + "<w:t>" + esc(cached) +
         "</w:t></w:r><w:r>" + r + "<w:fldChar w:fldCharType=\"end\"/></w:r>";
}

// ------------------------------------------------------------ hyperlinks
// Word validates relationship targets as URIs: percent-encode what is not allowed, refuse the hopeless ones.
string uriEncode(const string& raw) {
  string u = trim(raw);
  if (startsWith(lower(u), "www.")) u = "https://" + u;
  if (!(startsWith(lower(u), "http://") || startsWith(lower(u), "https://") || startsWith(lower(u), "mailto:"))) return "";
  string o;
  static const char* hex = "0123456789ABCDEF";
  for (unsigned char c : u) {
    if (isalnum(c) || strchr("-._~:/?#[]@!$&'()*+,;=%", c)) o += char(c);
    else if (c <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}' || c >= 0x7F) {
      o += '%'; o += hex[c >> 4]; o += hex[c & 15];
    }
  }
  if (o.size() > 2000) return "";
  return o;
}

string hyperlink(const string& runs, const string& url, DocxCtx* cx) {
  string enc = cx ? uriEncode(url) : string();
  if (enc.empty() || runs.empty()) return runs;
  cx->links.push_back(enc);
  return "<w:hyperlink r:id=\"rIdHl" + std::to_string(cx->links.size()) + "\" w:history=\"1\">" + runs + "</w:hyperlink>";
}

// ------------------------------------------------------------ inline Markdown
struct Inline {
  DocxCtx* cx = nullptr;
  bool markdown = true;  // **bold**, *italic*, `code`, ~~strike~~, [text](url)
  bool autolink = true;  // bare URLs and DOIs become hyperlinks
};

bool wordByte(const string& s, size_t k) { return k < s.size() && (isalnum(uint8_t(s[k])) || uint8_t(s[k]) >= 0x80); }

// closing marker at or after `from`: preceded by a non-space, not escaped; `word` markers must not sit inside a word
size_t findClose(const string& s, size_t from, const string& m, bool word) {
  for (size_t k = from; (k = s.find(m, k)) != string::npos; k++) {
    if (k == 0 || s[k - 1] == ' ' || s[k - 1] == '\\' || s[k - 1] == '\n') continue;
    if (m.size() == 1 && k + 1 < s.size() && s[k + 1] == m[0]) continue;  // part of a double marker
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

string inlineRuns(const string& src, const RPr& base, const Inline& in);

// text of a URL that starts at i (already known to start with http(s):// or www.)
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

// a DOI (10.NNNN/suffix) starting at i
size_t doiEnd(const string& s, size_t i) {
  if (s.compare(i, 3, "10.") != 0) return i;
  size_t k = i + 3, d = 0;
  while (k < s.size() && isdigit(uint8_t(s[k]))) { k++; d++; }
  if (d < 4 || d > 9 || k >= s.size() || s[k] != '/') return i;
  size_t e = urlEnd(s, k + 1);
  return e > k + 1 ? e : i;
}

string inlineRuns(const string& src, const RPr& base, const Inline& in) {
  string s = cleanText(src);
  string o, cur;
  auto flush = [&]() { if (!cur.empty()) { o += textRun(cur, base); cur.clear(); } };
  auto sub = [&](const string& text, RPr rp) { flush(); o += inlineRuns(text, rp, in); };
  size_t n = s.size();
  for (size_t i = 0; i < n;) {
    char c = s[i];
    if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|>~<", s[i + 1])) { cur += s[i + 1]; i += 2; continue; }
    if (c == '&') {  // HTML entity
      size_t e = s.find(';', i);
      if (e != string::npos && e - i <= 8) {
        string v = entity(s.substr(i + 1, e - i - 1));
        if (!v.empty()) { cur += v; i = e + 1; continue; }
      }
    }
    if (c == '<') {  // the few HTML tags models use in Markdown
      size_t e = s.find('>', i);
      if (e != string::npos && e - i <= 12) {
        string tag = lower(trim(s.substr(i + 1, e - i - 1)));
        while (!tag.empty() && tag.back() == '/') tag.pop_back();
        tag = trim(tag);
        if (tag == "br") { cur += '\n'; i = e + 1; continue; }
        static const struct { const char* t; int f; } tags[] = {{"b", 1}, {"strong", 1}, {"i", 2}, {"em", 2}, {"u", 3}, {"sub", 4}, {"sup", 5}, {"s", 6}, {"del", 6}, {"code", 7}};
        for (auto& t : tags) {
          if (tag != t.t) continue;
          size_t close = string::npos;
          for (size_t k = e + 1; (k = s.find("</", k)) != string::npos; k++) {
            size_t ce = s.find('>', k);
            if (ce == string::npos) break;
            if (lower(trim(s.substr(k + 2, ce - k - 2))) == tag) { close = k; break; }
          }
          if (close == string::npos) { i = e + 1; goto next; }  // unmatched tag: dropped
          RPr rp = base;
          if (t.f == 1) rp.b = true;
          if (t.f == 2) rp.i = true;
          if (t.f == 3) rp.u = true;
          if (t.f == 4) rp.vert = "subscript";
          if (t.f == 5) rp.vert = "superscript";
          if (t.f == 6) rp.strike = true;
          if (t.f == 7) { rp.style = "CodeChar"; rp.font.clear(); }
          sub(s.substr(e + 1, close - e - 1), rp);
          i = s.find('>', close) + 1;
          goto next;
        }
      }
    }
    if (in.markdown) {
      if (c == '`') {
        size_t e = s.find('`', i + 1);
        if (e != string::npos && e > i + 1) {
          flush();
          RPr rp = base;
          rp.style = "CodeChar";
          o += textRun(s.substr(i + 1, e - i - 1), rp);
          i = e + 1;
          continue;
        }
      }
      if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c && i + 2 < n && s[i + 2] != ' ' && s[i + 2] != c) {
        size_t e = findClose(s, i + 2, string(2, c), c == '_');
        if (e != string::npos && e > i + 2) { RPr rp = base; rp.b = true; sub(s.substr(i + 2, e - i - 2), rp); i = e + 2; continue; }
      }
      if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] != ' ' && s[i + 1] != c && (c == '*' || !wordByte(s, i == 0 ? n : i - 1))) {
        size_t e = findClose(s, i + 1, string(1, c), c == '_');
        if (e != string::npos && e > i + 1) { RPr rp = base; rp.i = true; sub(s.substr(i + 1, e - i - 1), rp); i = e + 1; continue; }
      }
      if (c == '~' && i + 2 < n && s[i + 1] == '~' && s[i + 2] != ' ') {
        size_t e = findClose(s, i + 2, "~~", false);
        if (e != string::npos && e > i + 2) { RPr rp = base; rp.strike = true; sub(s.substr(i + 2, e - i - 2), rp); i = e + 2; continue; }
      }
      if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {  // [text](url) and ![alt](src)
        size_t b = c == '!' ? i + 1 : i;
        size_t e = s.find(']', b + 1);
        if (e != string::npos && e + 1 < n && s[e + 1] == '(' && s.find('\n', b) > e) {
          size_t ce = string::npos;
          for (size_t k = e + 2, depth = 0; k < n && s[k] != '\n'; k++) {  // the closing parenthesis, allowing (...) inside the URL
            if (s[k] == '(') depth++;
            else if (s[k] == ')') { if (!depth) { ce = k; break; } depth--; }
          }
          if (ce != string::npos) {
            string text = s.substr(b + 1, e - b - 1), url = trim(s.substr(e + 2, ce - e - 2));
            size_t sp = url.find(' ');
            if (sp != string::npos && sp + 1 < url.size() && (url[sp + 1] == '"' || url[sp + 1] == '\'')) url = url.substr(0, sp);  // [text](url "title")
            if (c == '!') { cur += text; i = ce + 1; continue; }
            if (startsWith(lower(url), "doi.org/") || startsWith(url, "10.")) url = "https://doi.org/" + url.substr(startsWith(url, "10.") ? 0 : 8);
            flush();
            RPr lr = base;
            if (!uriEncode(url).empty()) lr.style = "Hyperlink";  // an unusable target keeps the text, plain
            o += hyperlink(inlineRuns(text, lr, in), url, in.cx);
            i = ce + 1;
            continue;
          }
        }
      }
    }
    if (in.autolink && (c == 'h' || c == 'w' || c == '1') && (i == 0 || !wordByte(s, i - 1))) {
      bool url = s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0 || s.compare(i, 4, "www.") == 0;
      size_t e = url ? urlEnd(s, i) : doiEnd(s, i);
      if (e > i + 8) {
        string t = s.substr(i, e - i);
        flush();
        RPr lr = base;
        lr.style = "Hyperlink";
        o += hyperlink(textRun(t, lr), url ? t : "https://doi.org/" + t, in.cx);
        i = e;
        continue;
      }
    }
    cur += c;
    i++;
  next:;
  }
  flush();
  return o;
}

// ------------------------------------------------------------ block Markdown
struct Blk {
  enum T { Para, Heading, Item, Quote, Code, Rule, Table, Caption } t = Para;
  int level = 0;         // heading: raw '#' count (99 = bold line); item: depth 0..2
  bool ordered = false;  // item
  int number = 1;        // item: the number written in the text
  int listIdx = -1;      // item: numbered-list instance (index into DocxCtx::listStarts)
  string text;
  vector<string> lines;              // code
  vector<vector<string>> rows;       // table
  vector<char> align;                // table: 'l' 'c' 'r' or 'a' (auto)
  bool header = true;                // table
};

bool isRule(const string& t) {
  if (t.size() < 3) return false;
  char c = t[0];
  if (c != '-' && c != '*' && c != '_') return false;
  int k = 0;
  for (char x : t) { if (x == c) k++; else if (x != ' ') return false; }
  return k >= 3;
}

int headingLevel(const string& t, string& rest) {
  size_t k = 0;
  while (k < t.size() && k < 6 && t[k] == '#') k++;
  if (!k || k >= t.size() || t[k] != ' ') return 0;
  rest = trim(t.substr(k));
  while (!rest.empty() && rest.back() == '#') rest.pop_back();  // closing hashes
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
  if (r.size() >= 2 && (r[0] == '-' || r[0] == '*' || r[0] == '+') && (r[1] == ' ' || r[1] == '\t')) {
    ordered = false; number = 0; content = trim(r.substr(2)); return true;
  }
  if (r.size() >= 4 && r.compare(0, 3, "\xE2\x80\xA2") == 0 && r[3] == ' ') { ordered = false; number = 0; content = trim(r.substr(4)); return true; }
  size_t d = 0;
  while (d < r.size() && d < 3 && isdigit(uint8_t(r[d]))) d++;
  if (d > 0 && d + 1 < r.size() && (r[d] == '.' || r[d] == ')') && (r[d + 1] == ' ' || r[d + 1] == '\t')) {
    ordered = true; number = toInt(r.substr(0, d), 1); content = trim(r.substr(d + 2)); return true;
  }
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
  if (isSepRow(t)) return hasPipe(next) && !isSepRow(next);  // a separator first: table without a header row
  if (isSepRow(next)) return true;
  return t[0] == '|' && !next.empty() && next[0] == '|';
}

string cleanTitle(string s) {
  s = lower(trim(s));
  size_t k = 0;
  while (k < s.size() && (isdigit(uint8_t(s[k])) || s[k] == '.' || s[k] == ' ')) k++;  // "2.1 Methods"
  if (k < s.size()) s = s.substr(k);
  while (!s.empty() && (s.back() == ':' || s.back() == '.' || s.back() == ' ')) s.pop_back();
  if (s.size() > 4 && startsWith(s, "**") && endsWith(s, "**")) s = s.substr(2, s.size() - 4);
  return s;
}

vector<Blk> parseBlocks(const string& text, DocxCtx& cx, const string& sectionTitle) {
  vector<Blk> out;
  vector<string> lines = split(replaceAll(text, "\r", ""), '\n', true);
  string pbuf;
  bool prevBreak = false, afterBlank = false, prevQuote = false;
  vector<int> indents;  // open list levels by indent
  int listIdx = -1;     // current numbered list instance
  bool lastNumbered0 = false;
  auto flushPara = [&]() {
    string t = trim(pbuf);
    if (!t.empty()) { Blk b; b.text = t; out.push_back(b); }
    pbuf.clear();
    prevBreak = false;
  };
  auto endList = [&]() { indents.clear(); listIdx = -1; lastNumbered0 = false; };
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
        string l = replaceAll(lines[i], "\r", "");
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
        b.lines.push_back(l);
      }
      out.push_back(b);
      prevQuote = false;
      continue;
    }
    if (isRule(t)) { flushPara(); endList(); Blk b; b.t = Blk::Rule; out.push_back(b); prevQuote = false; continue; }
    string rest;
    if (int hl = headingLevel(t, rest)) {
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
        bool restart = listIdx < 0 || (b.level == 0 && number == 1 && lastNumbered0);
        if (restart) { cx.listStarts.push_back(std::max(1, number)); listIdx = int(cx.listStarts.size()) - 1; }
        b.listIdx = listIdx;
        if (b.level == 0) lastNumbered0 = true;
      }
      out.push_back(b);
      continue;
    }
    if (!indents.empty() && indent >= 2 && !out.empty() && out.back().t == Blk::Item) {  // continuation of the item
      out.back().text += (blank ? "\n" : " ") + t;
      continue;
    }
    endList();
    bool hardBreak = raw.size() >= 2 && raw.compare(raw.size() - 2, 2, "  ") == 0;
    string piece = t;
    if (endsWith(piece, "\\")) { piece.pop_back(); hardBreak = true; }
    if (!pbuf.empty()) pbuf += prevBreak ? "\n" : " ";
    pbuf += piece;
    prevBreak = hardBreak;
  }
  flushPara();
  // a line that is only bold reads as a run-in heading
  for (auto& b : out) {
    if (b.t != Blk::Para || b.text.size() <= 4 || b.text.size() > 120 || !startsWith(b.text, "**") || !endsWith(b.text, "**")) continue;
    if (b.text.find("**", 2) != b.text.size() - 2) continue;
    b.t = Blk::Heading;
    b.level = 99;
    b.text = b.text.substr(2, b.text.size() - 4);
  }
  // "Table 3. ..." next to a table is its caption
  for (size_t k = 0; k < out.size(); k++) {
    if (out[k].t != Blk::Table) continue;
    for (size_t j : {k - 1, k + 1}) {
      if (j >= out.size() || out[j].t != Blk::Para) continue;
      string t = lower(out[j].text);
      if (startsWith(t, "**")) t = t.substr(2);
      if (startsWith(t, "table ") && t.size() > 6 && isdigit(uint8_t(t[6])) && out[j].text.size() < 300) out[j].t = Blk::Caption;
    }
  }
  // the section title repeated as the first heading
  if (!sectionTitle.empty() && !out.empty() && out[0].t == Blk::Heading && cleanTitle(out[0].text) == cleanTitle(sectionTitle)) out.erase(out.begin());
  return out;
}

// ------------------------------------------------------------ block emitters
bool numericCell(const string& s) {
  bool digit = false;
  for (unsigned char c : s) {
    if (isdigit(c)) digit = true;
    else if (!strchr(" .,%+-()/:", c) && c != 0xE2 && c != 0x80 && c != 0x93 && c != 0x94 && c != 0xC2 && c != 0xB1) return false;
  }
  return digit;
}

string tableXml(const Blk& b, const Inline& in) {
  size_t cols = 0;
  for (auto& r : b.rows) cols = std::max(cols, r.size());
  if (!cols) return "";
  // column widths: proportional to the longest cell (capped), with a floor
  vector<double> need(cols, 4);
  for (auto& r : b.rows)
    for (size_t c = 0; c < r.size(); c++) need[c] = std::max(need[c], double(std::min<size_t>(r[c].size(), 40)));
  double tot = 0;
  for (double v : need) tot += v;
  vector<int> w(cols);
  int used = 0, floorW = std::min(900, kTextTwips / int(cols));
  for (size_t c = 0; c < cols; c++) { w[c] = std::max(floorW, int(kTextTwips * need[c] / std::max(1.0, tot))); used += w[c]; }
  for (size_t c = 0; c < cols; c++) w[c] = int(double(w[c]) * kTextTwips / used);
  // alignment: from the separator row, else numbers right
  vector<char> al(cols, 'a');
  for (size_t c = 0; c < cols && c < b.align.size(); c++) al[c] = b.align[c];
  for (size_t c = 0; c < cols; c++) {
    if (al[c] != 'a') continue;
    int num = 0, txt = 0;
    for (size_t r = b.header ? 1 : 0; r < b.rows.size(); r++) {
      if (c >= b.rows[r].size() || b.rows[r][c].empty()) continue;
      (numericCell(b.rows[r][c]) ? num : txt)++;
    }
    al[c] = num > 0 && txt == 0 ? 'r' : 'l';
  }
  RPr cellR;
  if (cols >= 9) cellR.sz = 15;
  else if (cols >= 6) cellR.sz = 17;
  string o = "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"5000\" w:type=\"pct\"/>"
             "<w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" w:firstColumn=\"0\" w:lastColumn=\"0\" w:noHBand=\"0\" w:noVBand=\"1\"/></w:tblPr><w:tblGrid>";
  for (int v : w) o += "<w:gridCol w:w=\"" + std::to_string(v) + "\"/>";
  o += "</w:tblGrid>";
  for (size_t r = 0; r < b.rows.size(); r++) {
    bool head = b.header && r == 0;
    o += "<w:tr><w:trPr><w:cantSplit/>" + string(head ? "<w:tblHeader/>" : "") + "</w:trPr>";
    for (size_t c = 0; c < cols; c++) {
      string cell = c < b.rows[r].size() ? b.rows[r][c] : string();
      o += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(w[c]) + "\" w:type=\"dxa\"/>";
      if (head) o += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"EEF2F7\"/>";
      o += "<w:vAlign w:val=\"center\"/></w:tcPr>";
      PPr pp;
      pp.style = "TableText";
      if (al[c] == 'r') pp.jc = "right";
      else if (al[c] == 'c') pp.jc = "center";
      RPr rp = cellR;
      rp.b = head;
      o += para(inlineRuns(cell, rp, in), pp) + "</w:tc>";
    }
    o += "</w:tr>";
  }
  PPr sp;
  sp.style = "TableSpacer";
  return o + "</w:tbl>" + para("", sp);
}

string blocksXml(const vector<Blk>& blocks, const Inline& in, int headingBase) {
  int minLevel = 7;
  for (auto& b : blocks) if (b.t == Blk::Heading && b.level < 99) minLevel = std::min(minLevel, b.level);
  string o;
  for (size_t k = 0; k < blocks.size(); k++) {
    const Blk& b = blocks[k];
    switch (b.t) {
      case Blk::Para: o += para(inlineRuns(b.text, RPr(), in)); break;
      case Blk::Heading: {
        int h = b.level == 99 ? std::min(4, headingBase + 1) : std::min(4, std::max(headingBase, headingBase + b.level - (minLevel == 7 ? 1 : minLevel)));
        PPr pp;
        pp.style = "Heading" + std::to_string(h);
        o += para(inlineRuns(b.text, RPr(), in), pp);
        break;
      }
      case Blk::Item: {
        PPr pp;
        pp.style = "ListParagraph";
        int numId = b.ordered && b.listIdx >= 0 ? 2 + b.listIdx : 1;
        pp.numPr = "<w:numPr><w:ilvl w:val=\"" + std::to_string(b.level) + "\"/><w:numId w:val=\"" + std::to_string(numId) + "\"/></w:numPr>";
        o += para(inlineRuns(b.text, RPr(), in), pp);
        break;
      }
      case Blk::Quote: { PPr pp; pp.style = "Quote"; o += para(inlineRuns(b.text, RPr(), in), pp); break; }
      case Blk::Code: {
        PPr pp;
        pp.style = "SourceCode";
        for (auto& l : b.lines) o += para(textRun(cleanText(l)), pp);
        PPr sp;
        sp.style = "TableSpacer";
        o += para("", sp);
        break;
      }
      case Blk::Rule: {
        PPr pp;
        pp.pBdr = "<w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"BFC5CE\"/></w:pBdr>";
        pp.spacing = "<w:spacing w:before=\"60\" w:after=\"160\"/>";
        o += para("", pp);
        break;
      }
      case Blk::Table: o += tableXml(b, in); break;
      case Blk::Caption: {
        PPr pp;
        pp.style = "Caption";
        pp.keepNext = k + 1 < blocks.size() && blocks[k + 1].t == Blk::Table;
        o += para(inlineRuns(b.text, RPr(), in), pp);
        break;
      }
    }
  }
  return o;
}

string figureXml(int n, const string& title, const string& caption, int rid, long long cx, long long cy, DocxCtx& ctx) {
  string o;
  string capText = title;
  if (!caption.empty()) capText += (capText.empty() ? "" : ". ") + caption;
  if (rid > 0) {
    string id = std::to_string(rid), nm = "Figure " + std::to_string(n), dp = std::to_string(ctx.nextDrawing++);
    PPr pp;
    pp.keepNext = true;
    pp.spacing = "<w:spacing w:before=\"200\" w:after=\"60\"/>";
    pp.jc = "center";
    o += "<w:p>" + pp.str() + "<w:r><w:drawing>"
         "<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) +
         "\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:docPr id=\"" + dp + "\" name=\"" + attr(nm) + "\" descr=\"" + attr(capText) +
         "\"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
         "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
         "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"0\" name=\"figure" + std::to_string(n) + ".png\"/><pic:cNvPicPr><a:picLocks noChangeAspect=\"1\" noChangeArrowheads=\"1\"/></pic:cNvPicPr></pic:nvPicPr>"
         "<pic:blipFill><a:blip r:embed=\"rId" + id + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
         "<pic:spPr bwMode=\"auto\"><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) +
         "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>";
  }
  // caption: "Figure <SEQ Figure>. Title. Caption" - Word's own caption shape, so cross-references and a table of figures work
  PPr cp;
  cp.style = "Caption";
  cp.keepLines = true;
  cp.jc = "center";
  RPr lab;
  lab.b = true;
  Inline in;
  in.cx = &ctx;
  o += para(textRun("Figure ", lab) + field("SEQ Figure \\* ARABIC", std::to_string(n), lab) + textRun(". ", lab) + inlineRuns(capText, RPr(), in), cp);
  return o;
}
}  // namespace

// ------------------------------------------------------------ parts
string docxBodyXml(const string& markdown, DocxCtx& cx, const string& sectionTitle) {
  Inline in;
  in.cx = &cx;
  return blocksXml(parseBlocks(markdown, cx, sectionTitle), in, sectionTitle.empty() ? 1 : 2);
}

string docxDocumentXml(const ReportDoc& d, const vector<std::pair<int, int>>& figurePx, DocxCtx& cx) {
  Inline in;
  in.cx = &cx;
  string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document xmlns:w=\"" + string(kW) + "\" xmlns:r=\"" + kR +
             "\" xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\" xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
             " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>";
  PPr tp;
  tp.style = "Title";
  o += para(inlineRuns(d.title.empty() ? "Report" : d.title, RPr(), in), tp);
  if (!d.subtitle.empty()) { PPr pp; pp.style = "Subtitle"; o += para(inlineRuns(d.subtitle, RPr(), in), pp); }
  bool meta = false;
  for (auto& m : d.meta) {
    if (trim(m).empty()) continue;
    PPr pp;
    pp.style = "ReportMeta";
    o += para(inlineRuns(m, RPr(), in), pp);
    meta = true;
  }
  if (!d.date.empty()) { PPr pp; pp.style = "ReportMeta"; o += para(inlineRuns(d.date, RPr(), in), pp); meta = true; }
  {
    PPr pp;  // rule under the title block
    pp.pBdr = "<w:pBdr><w:bottom w:val=\"single\" w:sz=\"8\" w:space=\"1\" w:color=\"1F2937\"/></w:pBdr>";
    pp.spacing = "<w:spacing w:before=\"" + string(meta ? "60" : "0") + "\" w:after=\"240\"/>";
    o += para("", pp);
  }
  int fig = 0;
  for (auto& b : d.blocks) {
    if (b.kind == ReportBlock::Section) {
      if (!trim(b.title).empty()) { PPr pp; pp.style = "Heading1"; o += para(inlineRuns(b.title, RPr(), in), pp); }
      o += docxBodyXml(b.text, cx, trim(b.title));
    } else {
      fig++;
      int rid = 0;
      long long cx_ = kTextEmu, cy = kTextEmu * 3 / 4;
      if (size_t(fig - 1) < figurePx.size() && figurePx[size_t(fig - 1)].first > 0 && figurePx[size_t(fig - 1)].second > 0) {
        rid = 100 + fig;
        double W = b.fig.W > 0 ? b.fig.W : 640;
        double ratio = double(figurePx[size_t(fig - 1)].second) / double(figurePx[size_t(fig - 1)].first);  // the rendered pixels decide the shape
        cx_ = (long long)(W * 12700);
        if (cx_ > kTextEmu) cx_ = kTextEmu;
        cy = (long long)(double(cx_) * ratio);
        if (cy > kMaxFigEmu) { cy = kMaxFigEmu; cx_ = (long long)(double(cy) / ratio); }
        if (cx_ < 12700) cx_ = 12700;
        if (cy < 12700) cy = 12700;
      }
      o += figureXml(fig, b.title, b.text, rid, cx_, cy, cx);
    }
  }
  if (!d.references.empty()) {
    PPr hp;
    hp.style = "Heading1";
    o += para(textRun("References"), hp);
    Inline ref;  // references are plain text: asterisks in titles are not Markdown, but DOIs and URLs are links
    ref.cx = &cx;
    ref.markdown = false;
    int n = 0;
    for (auto& r : d.references) {
      PPr pp;
      pp.style = "Bibliography";
      o += para(textRun("[" + std::to_string(++n) + "]\t") + inlineRuns(r, RPr(), ref), pp);
    }
  }
  o += "<w:sectPr><w:footerReference w:type=\"default\" r:id=\"rIdFooter\"/><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
       "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/>"
       "<w:cols w:space=\"708\"/><w:docGrid w:linePitch=\"360\"/></w:sectPr>";
  o += "</w:body></w:document>";
  return o;
}

string docxStylesXml(const string& font, int sizeHalfPt, int lineTwips, const string& headingFont) {
  string f = attr(font.empty() ? string("Calibri") : font), sz = std::to_string(sizeHalfPt > 0 ? sizeHalfPt : 22);
  string hf = headingFont.empty() || headingFont == font ? string() : "<w:rFonts w:ascii=\"" + attr(headingFont) + "\" w:hAnsi=\"" + attr(headingFont) + "\" w:cs=\"" + attr(headingFont) + "\"/>";
  string line = std::to_string(clampv(lineTwips, 240, 600));
  string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles xmlns:w=\"" + string(kW) + "\">"
             "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"" + f + "\" w:hAnsi=\"" + f + "\" w:eastAsia=\"" + f + "\" w:cs=\"" + f + "\"/><w:sz w:val=\"" + sz + "\"/><w:szCs w:val=\"" + sz + "\"/>"
             "<w:lang w:val=\"en-GB\" w:eastAsia=\"en-US\" w:bidi=\"ar-SA\"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after=\"140\" w:line=\"" + line + "\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>";
  // element order inside w:style: name, basedOn, next, uiPriority, semiHidden, unhideWhenUsed, qFormat, pPr, rPr, tblPr
  auto st = [&](const char* type, const string& id, const string& name, const string& based, const string& next, const string& pPr, const string& rPr,
                bool q = true, bool def = false, bool hidden = false, const string& tblPr = "") {
    o += "<w:style w:type=\"" + string(type) + "\"" + (def ? " w:default=\"1\"" : "") + " w:styleId=\"" + id + "\"><w:name w:val=\"" + name + "\"/>";
    if (!based.empty()) o += "<w:basedOn w:val=\"" + based + "\"/>";
    if (!next.empty()) o += "<w:next w:val=\"" + next + "\"/>";
    if (hidden) o += "<w:uiPriority w:val=\"99\"/><w:semiHidden/><w:unhideWhenUsed/>";
    if (q) o += "<w:qFormat/>";
    if (!pPr.empty()) o += "<w:pPr>" + pPr + "</w:pPr>";
    if (!rPr.empty()) o += "<w:rPr>" + rPr + "</w:rPr>";
    if (!tblPr.empty()) o += "<w:tblPr>" + tblPr + "</w:tblPr>";
    o += "</w:style>";
  };
  const string ink = "<w:color w:val=\"1F2937\"/>";
  st("paragraph", "Normal", "Normal", "", "", "", "", true, true);
  st("character", "DefaultParagraphFont", "Default Paragraph Font", "", "", "", "", false, true, true);
  st("table", "TableNormal", "Normal Table", "", "", "", "", false, true, true,
     "<w:tblInd w:w=\"0\" w:type=\"dxa\"/><w:tblCellMar><w:top w:w=\"0\" w:type=\"dxa\"/><w:left w:w=\"108\" w:type=\"dxa\"/><w:bottom w:w=\"0\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar>");
  st("numbering", "NoList", "No List", "", "", "", "", false, true, true);
  st("paragraph", "Title", "Title", "Normal", "Normal", "<w:spacing w:before=\"0\" w:after=\"120\" w:line=\"240\" w:lineRule=\"auto\"/><w:contextualSpacing/>",
     hf + "<w:b/><w:bCs/>" + ink + "<w:sz w:val=\"48\"/><w:szCs w:val=\"48\"/>");
  st("paragraph", "Subtitle", "Subtitle", "Normal", "Normal", "<w:spacing w:after=\"160\"/>", "<w:color w:val=\"4B5563\"/><w:sz w:val=\"28\"/><w:szCs w:val=\"28\"/>");
  st("paragraph", "ReportMeta", "Report Meta", "Normal", "Normal", "<w:spacing w:after=\"40\"/>", "<w:i/><w:iCs/><w:color w:val=\"6B7280\"/><w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/>");
  st("paragraph", "Heading1", "heading 1", "Normal", "Normal", "<w:keepNext/><w:keepLines/><w:spacing w:before=\"360\" w:after=\"120\"/><w:outlineLvl w:val=\"0\"/>",
     hf + "<w:b/><w:bCs/>" + ink + "<w:sz w:val=\"32\"/><w:szCs w:val=\"32\"/>");
  st("paragraph", "Heading2", "heading 2", "Normal", "Normal", "<w:keepNext/><w:keepLines/><w:spacing w:before=\"280\" w:after=\"100\"/><w:outlineLvl w:val=\"1\"/>",
     hf + "<w:b/><w:bCs/>" + ink + "<w:sz w:val=\"26\"/><w:szCs w:val=\"26\"/>");
  st("paragraph", "Heading3", "heading 3", "Normal", "Normal", "<w:keepNext/><w:keepLines/><w:spacing w:before=\"200\" w:after=\"80\"/><w:outlineLvl w:val=\"2\"/>",
     hf + "<w:b/><w:bCs/><w:color w:val=\"374151\"/><w:sz w:val=\"23\"/><w:szCs w:val=\"23\"/>");
  st("paragraph", "Heading4", "heading 4", "Normal", "Normal", "<w:keepNext/><w:keepLines/><w:spacing w:before=\"160\" w:after=\"60\"/><w:outlineLvl w:val=\"3\"/>",
     hf + "<w:b/><w:bCs/><w:i/><w:iCs/><w:color w:val=\"374151\"/><w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>");
  st("paragraph", "Header", "header", "Normal", "", "<w:tabs><w:tab w:val=\"center\" w:pos=\"4513\"/><w:tab w:val=\"right\" w:pos=\"9026\"/></w:tabs><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>",
     "<w:color w:val=\"6B7280\"/><w:sz w:val=\"18\"/><w:szCs w:val=\"18\"/>", false);
  st("paragraph", "Caption", "caption", "Normal", "Normal", "<w:spacing w:before=\"60\" w:after=\"240\"/>", "<w:color w:val=\"4B5563\"/><w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/>");
  st("paragraph", "ListParagraph", "List Paragraph", "Normal", "", "<w:spacing w:after=\"60\"/><w:ind w:left=\"720\"/><w:contextualSpacing/>", "");
  st("paragraph", "Quote", "Quote", "Normal", "Normal",
     "<w:pBdr><w:left w:val=\"single\" w:sz=\"12\" w:space=\"10\" w:color=\"BFC5CE\"/></w:pBdr><w:spacing w:before=\"120\" w:after=\"160\"/><w:ind w:left=\"567\" w:right=\"567\"/>",
     "<w:i/><w:iCs/><w:color w:val=\"404040\"/>");
  st("paragraph", "SourceCode", "Source Code", "Normal", "",
     "<w:keepLines/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F1F3F6\"/><w:spacing w:before=\"0\" w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:ind w:left=\"113\" w:right=\"113\"/>",
     "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/><w:sz w:val=\"18\"/><w:szCs w:val=\"18\"/>", false);
  st("paragraph", "TableText", "Table Text", "Normal", "", "<w:spacing w:before=\"30\" w:after=\"30\" w:line=\"240\" w:lineRule=\"auto\"/>", "<w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/>", false);
  st("paragraph", "TableSpacer", "Table Spacer", "Normal", "Normal", "<w:spacing w:before=\"0\" w:after=\"100\" w:line=\"120\" w:lineRule=\"exact\"/>", "<w:sz w:val=\"10\"/><w:szCs w:val=\"10\"/>", false);
  st("paragraph", "Bibliography", "Bibliography", "Normal", "Bibliography", "<w:tabs><w:tab w:val=\"left\" w:pos=\"567\"/></w:tabs><w:spacing w:after=\"80\"/><w:ind w:left=\"567\" w:hanging=\"567\"/>",
     "<w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/>");
  st("paragraph", "Footer", "footer", "Normal", "", "<w:tabs><w:tab w:val=\"center\" w:pos=\"4513\"/><w:tab w:val=\"right\" w:pos=\"9026\"/></w:tabs><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>",
     "<w:color w:val=\"6B7280\"/><w:sz w:val=\"18\"/><w:szCs w:val=\"18\"/>", false);
  st("character", "Hyperlink", "Hyperlink", "DefaultParagraphFont", "", "", "<w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/>", false, false, true);
  st("character", "CodeChar", "Source Code Char", "DefaultParagraphFont", "", "",
     "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:cs=\"Consolas\"/><w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F1F3F6\"/>", false);
  st("paragraph", "TOC1", "toc 1", "Normal", "Normal", "<w:tabs><w:tab w:val=\"right\" w:leader=\"dot\" w:pos=\"9016\"/></w:tabs><w:spacing w:after=\"60\"/>", "<w:b/><w:bCs/>", false, false, true);
  st("paragraph", "TOC2", "toc 2", "Normal", "Normal", "<w:tabs><w:tab w:val=\"right\" w:leader=\"dot\" w:pos=\"9016\"/></w:tabs><w:spacing w:after=\"60\"/><w:ind w:left=\"284\"/>", "", false, false, true);
  st("paragraph", "TOC3", "toc 3", "Normal", "Normal", "<w:tabs><w:tab w:val=\"right\" w:leader=\"dot\" w:pos=\"9016\"/></w:tabs><w:spacing w:after=\"60\"/><w:ind w:left=\"567\"/>", "<w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/>", false, false, true);
  o += "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:basedOn w:val=\"TableNormal\"/><w:uiPriority w:val=\"39\"/><w:tblPr><w:tblBorders>"
       "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFC5CE\"/><w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFC5CE\"/>"
       "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFC5CE\"/><w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFC5CE\"/>"
       "<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFC5CE\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFC5CE\"/>"
       "</w:tblBorders><w:tblCellMar><w:top w:w=\"40\" w:type=\"dxa\"/><w:left w:w=\"108\" w:type=\"dxa\"/><w:bottom w:w=\"40\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>";
  o += "</w:styles>";
  return o;
}

string docxNumberingXml(const DocxCtx& cx) {
  string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:numbering xmlns:w=\"" + string(kW) + "\">";
  auto lvl = [&](int i, const string& fmt, const string& text, const string& font) {
    string s = "<w:lvl w:ilvl=\"" + std::to_string(i) + "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"" + fmt + "\"/><w:lvlText w:val=\"" + text +
               "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"" + std::to_string(720 * (i + 1)) + "\" w:hanging=\"360\"/></w:pPr>";
    if (!font.empty()) s += "<w:rPr><w:rFonts w:ascii=\"" + font + "\" w:hAnsi=\"" + font + "\" w:hint=\"default\"/></w:rPr>";
    return s + "</w:lvl>";
  };
  // 0: bullets  1: numbers (decimal, letters, roman), nine levels each as Word expects
  o += "<w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
  static const char* bullets[] = {"\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x96\xAA"};
  for (int i = 0; i < 9; i++) o += lvl(i, "bullet", bullets[i % 3], "Calibri");
  o += "</w:abstractNum><w:abstractNum w:abstractNumId=\"1\"><w:multiLevelType w:val=\"hybridMultilevel\"/>";
  static const char* fmts[] = {"decimal", "lowerLetter", "lowerRoman"};
  for (int i = 0; i < 9; i++) o += lvl(i, fmts[i % 3], "%" + std::to_string(i + 1) + ".", "");
  o += "</w:abstractNum>";
  o += "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>";
  // every numbered list is its own instance with a start override, otherwise Word continues the count across lists
  for (size_t k = 0; k < cx.listStarts.size(); k++) {
    o += "<w:num w:numId=\"" + std::to_string(2 + k) + "\"><w:abstractNumId w:val=\"1\"/>";
    for (int i = 0; i < 9; i++)
      o += "<w:lvlOverride w:ilvl=\"" + std::to_string(i) + "\"><w:startOverride w:val=\"" + std::to_string(i == 0 ? std::max(1, cx.listStarts[k]) : 1) + "\"/></w:lvlOverride>";
    o += "</w:num>";
  }
  return o + "</w:numbering>";
}

string docxSettingsXml() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:settings xmlns:w=\"" + string(kW) + "\"><w:zoom w:percent=\"100\"/>"
         "<w:defaultTabStop w:val=\"720\"/><w:characterSpacingControl w:val=\"doNotCompress\"/><w:compat>"
         "<w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/>"
         "<w:compatSetting w:name=\"overrideTableStyleFontSizeAndJustification\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"1\"/>"
         "<w:compatSetting w:name=\"enableOpenTypeFeatures\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"1\"/>"
         "<w:compatSetting w:name=\"doNotFlipMirrorIndents\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"1\"/>"
         "<w:compatSetting w:name=\"differentiateMultirowTableHeaders\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"1\"/>"
         "</w:compat><w:decimalSymbol w:val=\".\"/><w:listSeparator w:val=\",\"/></w:settings>";
}

string docxFontTableXml(const string& extraFont) {
  string extra;
  if (!extraFont.empty() && extraFont != "Calibri" && extraFont != "Consolas") extra = "<w:font w:name=\"" + attr(extraFont) + "\"><w:charset w:val=\"00\"/><w:family w:val=\"auto\"/><w:pitch w:val=\"variable\"/></w:font>";
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:fonts xmlns:w=\"" + string(kW) + "\">" + extra +
         "<w:font w:name=\"Calibri\"><w:panose1 w:val=\"020F0502020204030204\"/><w:charset w:val=\"00\"/><w:family w:val=\"swiss\"/><w:pitch w:val=\"variable\"/>"
         "<w:sig w:usb0=\"E4002EFF\" w:usb1=\"C200247B\" w:usb2=\"00000009\" w:usb3=\"00000000\" w:csb0=\"000001FF\" w:csb1=\"00000000\"/></w:font>"
         "<w:font w:name=\"Consolas\"><w:panose1 w:val=\"020B0609020204030204\"/><w:charset w:val=\"00\"/><w:family w:val=\"modern\"/><w:pitch w:val=\"fixed\"/>"
         "<w:sig w:usb0=\"E00006FF\" w:usb1=\"0000FCFF\" w:usb2=\"00000001\" w:usb3=\"00000000\" w:csb0=\"0000019F\" w:csb1=\"00000000\"/></w:font></w:fonts>";
}

string docxFooterXml() {
  PPr pp;
  pp.style = "Footer";
  pp.jc = "center";
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:ftr xmlns:w=\"" + string(kW) + "\" xmlns:r=\"" + kR + "\">" +
         para(textRun("Page ") + field("PAGE", "1") + textRun(" of ") + field("NUMPAGES", "1"), pp) + "</w:ftr>";
}

namespace {
std::pair<int, int> pngSize(const string& bytes) {
  if (bytes.size() > 24 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) {
    auto be = [&](size_t at) { return (int(uint8_t(bytes[at])) << 24) | (int(uint8_t(bytes[at + 1])) << 16) | (int(uint8_t(bytes[at + 2])) << 8) | int(uint8_t(bytes[at + 3])); };
    return {be(16), be(20)};
  }
  return {0, 0};
}
struct DocxMeta { string title, subject, creator, keywords, font, headingFont; int sizeHalfPt = 22, lineTwips = 264; bool footer = true; };

string packDocx(const string& document, const vector<string>& images, const DocxCtx& cx, const DocxMeta& meta) {
  ZipWriter z;
  string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
              "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
              "<Default Extension=\"png\" ContentType=\"image/png\"/><Default Extension=\"svg\" ContentType=\"image/svg+xml\"/>"
              "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
              "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
              "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
              "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>"
              "<Override PartName=\"/word/fontTable.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.fontTable+xml\"/>"
              "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>"
              + string(cx.headerXml.empty() ? "" : "<Override PartName=\"/word/header1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml\"/>")
              + string(cx.sourcesXml.empty() ? "" : "<Override PartName=\"/customXml/itemProps1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.customXmlProperties+xml\"/>") +
              "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
              "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/></Types>";
  z.add("[Content_Types].xml", ct, true);
  z.add("_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/></Relationships>",
        true);
  const string relNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
  string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                "<Relationship Id=\"rIdStyles\" Type=\"" + relNs + "styles\" Target=\"styles.xml\"/>"
                "<Relationship Id=\"rIdNumbering\" Type=\"" + relNs + "numbering\" Target=\"numbering.xml\"/>"
                "<Relationship Id=\"rIdSettings\" Type=\"" + relNs + "settings\" Target=\"settings.xml\"/>"
                "<Relationship Id=\"rIdFonts\" Type=\"" + relNs + "fontTable\" Target=\"fontTable.xml\"/>"
                "<Relationship Id=\"rIdFooter\" Type=\"" + relNs + "footer\" Target=\"footer1.xml\"/>";
  if (!cx.headerXml.empty()) rels += "<Relationship Id=\"rIdHeader\" Type=\"" + relNs + "header\" Target=\"header1.xml\"/>";
  if (!cx.sourcesXml.empty()) rels += "<Relationship Id=\"rIdSources\" Type=\"" + relNs + "customXml\" Target=\"../customXml/item1.xml\"/>";
  for (size_t i = 0; i < images.size(); i++) {
    string n = std::to_string(i + 1);
    bool svg = i < cx.svgParts.size() && !cx.svgParts[i].empty();
    if (images[i].empty() && !svg) continue;
    if (!images[i].empty()) {
      rels += "<Relationship Id=\"rId" + std::to_string(101 + i) + "\" Type=\"" + relNs + "image\" Target=\"media/figure" + n + ".png\"/>";
      z.add("word/media/figure" + n + ".png", images[i]);
    } else {  // SVG only: the main blip points at the SVG part too (Word 2016+ reads it; older Word shows an empty frame)
      rels += "<Relationship Id=\"rId" + std::to_string(101 + i) + "\" Type=\"" + relNs + "image\" Target=\"media/figure" + n + ".svg\"/>";
    }
    if (svg) {
      rels += "<Relationship Id=\"rIdS" + std::to_string(101 + i) + "\" Type=\"" + relNs + "image\" Target=\"media/figure" + n + ".svg\"/>";
      z.add("word/media/figure" + n + ".svg", cx.svgParts[i], true);
    }
  }
  for (size_t i = 0; i < cx.links.size(); i++)
    rels += "<Relationship Id=\"rIdHl" + std::to_string(i + 1) + "\" Type=\"" + relNs + "hyperlink\" Target=\"" + esc(cx.links[i]) + "\" TargetMode=\"External\"/>";
  rels += "</Relationships>";
  z.add("word/_rels/document.xml.rels", rels, true);
  z.add("word/document.xml", document, true);
  z.add("word/styles.xml", docxStylesXml(meta.font, meta.sizeHalfPt, meta.lineTwips, meta.headingFont), true);
  if (!cx.headerXml.empty()) z.add("word/header1.xml", cx.headerXml, true);
  if (!cx.sourcesXml.empty()) {
    z.add("customXml/item1.xml", cx.sourcesXml, true);
    z.add("customXml/itemProps1.xml", docBibSourcesPropsXml(), true);
    z.add("customXml/_rels/item1.xml.rels",
          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
          "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXmlProps\" Target=\"itemProps1.xml\"/></Relationships>", true);
  }
  z.add("word/numbering.xml", docxNumberingXml(cx), true);
  z.add("word/settings.xml", docxSettingsXml(), true);
  z.add("word/fontTable.xml", docxFontTableXml(meta.font), true);
  z.add("word/footer1.xml", docxFooterXml(), true);
  string now = nowIso();
  string creator = meta.creator.empty() ? string("VOSStudio") : meta.creator;
  z.add("docProps/core.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:title>" + attr(meta.title) + "</dc:title><dc:subject>" + attr(meta.subject) +
        "</dc:subject><dc:creator>" + attr(creator) + "</dc:creator>" + (meta.keywords.empty() ? string() : "<cp:keywords>" + attr(meta.keywords) + "</cp:keywords>") +
        "<cp:lastModifiedBy>" + attr(creator) + "</cp:lastModifiedBy>"
        "<dcterms:created xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">" + now + "</dcterms:modified></cp:coreProperties>",
        true);
  z.add("docProps/app.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
        "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"><Application>VOSStudio</Application></Properties>",
        true);
  return z.finish();
}
}  // namespace

string toDOCX(const ReportDoc& d, const PngRenderer& png, double figureDpi) {
  // figures first: their pixel sizes decide whether an image is embedded and its shape
  vector<string> images;
  vector<std::pair<int, int>> px;
  for (auto& b : d.blocks) {
    if (b.kind != ReportBlock::Figure) continue;
    string bytes = png ? png(b.fig, figureDpi) : string();
    auto wh = pngSize(bytes);
    images.push_back(wh.first > 0 && wh.second > 0 ? bytes : string());
    px.push_back(wh);
  }
  DocxCtx cx;
  string document = docxDocumentXml(d, px, cx);
  DocxMeta meta;
  meta.title = d.title;
  meta.subject = d.subtitle;
  return packDocx(document, images, cx, meta);
}

// ------------------------------------------------------------ the editor's document
namespace {

string hex6(uint32_t c) {
  char b[8];
  snprintf(b, sizeof b, "%02X%02X%02X", (c >> 16) & 255, (c >> 8) & 255, c & 255);
  return b;
}

RPr spanRPr(const Span& s, const RPr& base) {
  RPr r = base;
  if (!s.font.empty()) r.font = attr(s.font);
  if (s.size > 0) r.sz = int(std::lround(s.size * 2));
  if (s.color) r.color = hex6(s.color);
  if (s.fmt & F_BOLD) r.b = true;
  if (s.fmt & F_ITALIC) r.i = true;
  if (s.fmt & F_STRIKE) r.strike = true;
  if (s.fmt & F_UNDER) r.u = true;
  if (s.fmt & F_SUB) r.vert = "subscript";
  if (s.fmt & F_SUP) r.vert = "superscript";
  if (s.fmt & F_CODE) r.style = "CodeChar";
  if (s.fmt & F_MARK) r.shd = "FFF3A0";
  if (!s.link.empty()) r.style = "Hyperlink";
  return r;
}

// runs for a paragraph's spans; consecutive spans with the same link share one w:hyperlink
std::map<string, string>* g_citeTags = nullptr;  // key -> Word source tag while docToDOCX runs (single-threaded)

// a citation field as Word makes it: a content control around { CITATION Tag \l 1033 [\m Tag2] } with the visible text cached
string citationSdt(const Span& s, const RPr& base, DocxCtx& cx) {
  vector<string> tags;
  if (g_citeTags) for (auto& k : split(s.cite, ';')) { auto it = g_citeTags->find(trim(k)); if (it != g_citeTags->end()) tags.push_back(it->second); }
  if (tags.empty()) return textRun(cleanText(s.text), spanRPr(s, base));
  string instr = "CITATION " + tags[0] + " \\l 1033";
  for (size_t i = 1; i < tags.size(); i++) instr += " \\m " + tags[i];
  RPr rp = spanRPr(s, base);
  string r = rp.str();
  return "<w:sdt><w:sdtPr><w:id w:val=\"" + std::to_string(cx.nextSdtId++) + "\"/><w:citation/></w:sdtPr><w:sdtContent>"
         "<w:r>" + r + "<w:fldChar w:fldCharType=\"begin\"/></w:r><w:r>" + r + "<w:instrText xml:space=\"preserve\"> " + esc(instr) + " </w:instrText></w:r>"
         "<w:r>" + r + "<w:fldChar w:fldCharType=\"separate\"/></w:r>" + textRun(cleanText(s.text), rp) + "<w:r>" + r + "<w:fldChar w:fldCharType=\"end\"/></w:r></w:sdtContent></w:sdt>";
}

string spanRuns(const vector<Span>& spans, const RPr& base, DocxCtx& cx) {
  string o;
  for (size_t i = 0; i < spans.size();) {
    if (spans[i].fmt & F_MATH) {
      string error;
      string math = mathToOmml(spans[i].text, &error);
      o += math.empty() ? textRun(cleanText(spans[i].text), spanRPr(spans[i], base)) : math;
      i++;
      continue;
    }
    if (spans[i].isCite()) { o += citationSdt(spans[i], base, cx); i++; continue; }
    if (spans[i].link.empty()) { o += textRun(cleanText(spans[i].text), spanRPr(spans[i], base)); i++; continue; }
    size_t j = i;
    string runs;
    while (j < spans.size() && spans[j].link == spans[i].link) { runs += textRun(cleanText(spans[j].text), spanRPr(spans[j], base)); j++; }
    o += hyperlink(runs, spans[i].link, &cx);
    i = j;
  }
  return o;
}

string jcOf(PAlign a) { return a == PAlign::Center ? "center" : a == PAlign::Right ? "right" : a == PAlign::Justify ? "both" : ""; }

bool captionLabelled(const string& text) {
  string t = lower(trim(text));
  for (const char* pfx : {"figure ", "fig. ", "table "}) {
    size_t n = strlen(pfx);
    if (t.size() > n && startsWith(t, pfx) && isdigit(uint8_t(t[n]))) return true;
  }
  return false;
}

string docTableXml(const Table& t, DocxCtx& cx, int textTwips) {
  int R = t.rows(), C = t.cols();
  if (!R || !C) return "";
  vector<int> w(size_t(C), textTwips / C);
  if (int(t.widths.size()) == C) {
    float sum = 0;
    for (float v : t.widths) sum += std::max(0.02f, v);
    for (int c = 0; c < C; c++) w[size_t(c)] = int(float(textTwips) * std::max(0.02f, t.widths[size_t(c)]) / sum);
  } else {
    vector<double> need(size_t(C), 4);
    for (auto& r : t.cells)
      for (int c = 0; c < C; c++) need[size_t(c)] = std::max(need[size_t(c)], double(std::min<size_t>(r[size_t(c)].size(), 40)));
    double tot = 0;
    for (double v : need) tot += v;
    int used = 0, floorW = std::min(900, textTwips / C);
    for (int c = 0; c < C; c++) { w[size_t(c)] = std::max(floorW, int(textTwips * need[size_t(c)] / std::max(1.0, tot))); used += w[size_t(c)]; }
    for (int c = 0; c < C; c++) w[size_t(c)] = int(double(w[size_t(c)]) * textTwips / used);
  }
  RPr cellR;
  if (C >= 9) cellR.sz = 15;
  else if (C >= 6) cellR.sz = 17;
  string o = "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"5000\" w:type=\"pct\"/>"
             "<w:tblLook w:val=\"04A0\" w:firstRow=\"1\" w:lastRow=\"0\" w:firstColumn=\"0\" w:lastColumn=\"0\" w:noHBand=\"0\" w:noVBand=\"1\"/></w:tblPr><w:tblGrid>";
  for (int v : w) o += "<w:gridCol w:w=\"" + std::to_string(v) + "\"/>";
  o += "</w:tblGrid>";
  for (int r = 0; r < R; r++) {
    bool head = t.header && r == 0;
    o += "<w:tr><w:trPr><w:cantSplit/>" + string(head ? "<w:tblHeader/>" : "") + "</w:trPr>";
    for (int c = 0; c < C; c++) {
      const Para& cell = t.cells[size_t(r)][size_t(c)];
      o += "<w:tc><w:tcPr><w:tcW w:w=\"" + std::to_string(w[size_t(c)]) + "\" w:type=\"dxa\"/>";
      if (head) o += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"EEF2F7\"/>";
      o += "<w:vAlign w:val=\"center\"/></w:tcPr>";
      PPr pp;
      pp.style = "TableText";
      char a = c < int(t.align.size()) ? t.align[size_t(c)] : 'a';
      pp.jc = cell.align != PAlign::Left ? jcOf(cell.align) : a == 'r' ? "right" : a == 'c' ? "center" : "";
      RPr rp = cellR;
      rp.b = head;
      o += para(spanRuns(cell.spans, rp, cx), pp) + "</w:tc>";
    }
    o += "</w:tr>";
  }
  PPr sp;
  sp.style = "TableSpacer";
  return o + "</w:tbl>" + para("", sp);
}

}  // namespace

const char* docBibSourcesPropsXml() {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<ds:datastoreItem ds:itemID=\"{8E4B1C2A-5D3F-4A6B-9C7D-1E2F3A4B5C6D}\" xmlns:ds=\"http://schemas.openxmlformats.org/officeDocument/2006/customXml\">"
         "<ds:schemaRefs><ds:schemaRef ds:uri=\"http://schemas.openxmlformats.org/officeDocument/2006/bibliography\"/></ds:schemaRefs></ds:datastoreItem>";
}

// Word's bibliography sources (customXml/item1.xml): one b:Source per entry, tags like Smi20 (unique); fills key -> tag.
// Shared by the .docx writer and the clipboard HTML (a dataStoreItem beside the copied text).
vector<std::pair<string, string>> docBibSourceItems(const Document& d, std::map<string, string>& citeTags, const std::set<string>* only) {
  vector<std::pair<string, string>> items;
  citeTags.clear();
  if (!d.refs.empty()) {
    std::set<string> usedTags;
    vector<int> order;  // list order = the document's Reference paragraphs, then the rest
    for (auto& b : d.blocks) if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference && !b.p.refKey.empty()) { int i = docRefIndexOf(d, b.p.refKey); if (i >= 0 && std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i); }
    for (size_t i = 0; i < d.refs.size(); i++) if (std::find(order.begin(), order.end(), int(i)) == order.end()) order.push_back(int(i));
    int refOrder = 0;
    for (int i : order) {
      const RefEntry& e = d.refs[size_t(i)];
      string last, year = e.year;
      if (e.structured() && !e.authors.empty()) last = splitAuthor(e.authors[0]).last;
      else if (!e.structured()) { size_t c = e.raw.find_first_of(",.("); last = trim(e.raw.substr(0, c == string::npos ? 8 : c)); for (size_t q = 0; q + 3 < e.raw.size(); q++) if (isdigit(uint8_t(e.raw[q])) && isdigit(uint8_t(e.raw[q + 1])) && isdigit(uint8_t(e.raw[q + 2])) && isdigit(uint8_t(e.raw[q + 3]))) { year = e.raw.substr(q, 4); break; } }
      if (last.empty()) last = e.title.empty() ? "Ref" : e.title;
      string tag;
      for (char c : asciiFold(last)) if (isalpha(uint8_t(c)) && tag.size() < 3) tag += c;
      if (tag.empty()) tag = "Ref";
      tag[0] = char(toupper(uint8_t(tag[0])));
      tag += year.size() >= 4 ? year.substr(2, 2) : string("00");
      string base = tag;
      for (int n = 1; usedTags.count(tag); n++) tag = base + std::to_string(n);
      usedTags.insert(tag);
      citeTags[e.key] = tag;
      if (only && !only->count(e.key)) continue;
      auto el = [&](const char* name, const string& v) { return v.empty() ? string() : "<b:" + string(name) + ">" + esc(cleanText(v)) + "</b:" + name + ">"; };
      string guid = sha256Hex(e.key).substr(0, 32);
      string g = "{" + upper(guid.substr(0, 8) + "-" + guid.substr(8, 4) + "-" + guid.substr(12, 4) + "-" + guid.substr(16, 4) + "-" + guid.substr(20, 12)) + "}";
      string src = "<b:Source>" + el("Tag", tag) + el("SourceType", e.structured() ? wordSourceType(e.kind) : "Misc") + el("Guid", g);
      if (e.structured()) {
        src += el("Title", e.title) + el("Year", e.year);
        if (!e.authors.empty()) {
          src += "<b:Author><b:Author><b:NameList>";
          for (auto& a : e.authors) { RefName n = splitAuthor(a); src += "<b:Person>" + el("Last", n.last) + el("First", n.given.empty() ? n.initials : n.given) + "</b:Person>"; }
          src += "</b:NameList></b:Author></b:Author>";
        }
        if (e.kind == "book" || e.kind == "chapter") src += el("BookTitle", e.kind == "chapter" ? e.container : string()) + el("City", e.place) + el("Publisher", e.publisher);
        else if (e.kind == "conference") src += el("ConferenceName", e.container) + el("City", e.place) + el("Publisher", e.publisher);
        else src += el("JournalName", e.container) + el("Publisher", e.publisher);
        src += el("Volume", e.volume) + el("Issue", e.issue) + el("Pages", e.pages) + el("DOI", e.doi) + el("URL", e.url);
      } else {
        src += el("Title", e.raw) + el("Year", year) + el("URL", e.url);
      }
      src += el("RefOrder", std::to_string(++refOrder)) + "</b:Source>";
      items.push_back({tag, src});
    }
  }
  return items;
}

string docBibSourcesXml(const Document& d, std::map<string, string>& citeTags, const std::set<string>* only) {
  vector<std::pair<string, string>> items = docBibSourceItems(d, citeTags, only);
  if (d.refs.empty()) return string();
  const CiteStyleInfo& st = citeStyleInfo(d.citeStyle);
  string src = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<b:Sources SelectedStyle=\"" + esc(st.wordXsl) + "\" StyleName=\"" + esc(st.wordName) +
               "\" xmlns:b=\"http://schemas.openxmlformats.org/officeDocument/2006/bibliography\" xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/bibliography\">";
  for (auto& it : items) src += it.second;
  src += "</b:Sources>";
  return src;
}

string docToDOCX(const Document& d, const ScenePng& png, double dpi) {
  const PageSetup& ps = d.setup;
  int pageW = ps.paper == "Letter" ? 12240 : 11906, pageH = ps.paper == "Letter" ? 15840 : 16838;
  int margin = int(std::lround(ps.margin() * 20));
  int textTwips = pageW - 2 * margin;
  long long textEmu = (long long)textTwips * 635;
  long long maxFigEmu = (long long)(pageH - 2 * margin) * 635 * 8 / 10;
  vector<int> nums = docListNumbers(d);
  vector<string> hn = docHeadingNumbers(d);
  DocxCtx cx;
  vector<string> images;
  string o = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document xmlns:w=\"" + string(kW) + "\" xmlns:r=\"" + kR +
             "\" xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\" xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
             " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>";
  auto tocXml = [&]() {
    PPr hp;
    hp.style = "Heading1";
    string t = para(textRun("Contents"), hp);
    bool first = true;
    string last;
    for (size_t k = 0; k < d.blocks.size(); k++) {
      const Block& b = d.blocks[k];
      if (b.kind != Block::Paragraph || !isHeading(b.p.style) || headingLevel(b.p.style) > 3) continue;
      PPr pp;
      pp.style = "TOC" + std::to_string(headingLevel(b.p.style));
      string runs;
      if (first) runs += "<w:r><w:fldChar w:fldCharType=\"begin\" w:dirty=\"true\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> TOC \\o \"1-3\" \\h \\z \\u </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>";
      string pre = ps.numberedHeadings && !hn[k].empty() ? hn[k] + "\t" : string();
      runs += textRun(cleanText(pre + b.p.text()));
      t += para(runs, pp);
      first = false;
      last = pp.style;
    }
    if (first) t += para("<w:r><w:fldChar w:fldCharType=\"begin\" w:dirty=\"true\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> TOC \\o \"1-3\" \\h \\z \\u </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>" + textRun("Update this field to build the table of contents."));
    t += para("<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>");
    return t;
  };
  bool frontDone = false, tocDone = false;
  bool hasTocBlock = false;
  for (auto& b : d.blocks) if (b.kind == Block::Toc) hasTocBlock = true;
  // ---- Word's own citations: one source per entry, tags like Smi20 (unique)
  std::map<string, string> citeTags;
  bool numericStyle = citeStyleInfo(d.citeStyle).numeric;
  cx.sourcesXml = docBibSourcesXml(d, citeTags);
  g_citeTags = citeTags.empty() ? nullptr : &citeTags;
  if (!ps.header.empty()) {
    PPr hp;
    hp.style = "Header";
    hp.jc = "right";
    cx.headerXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:hdr xmlns:w=\"" + string(kW) + "\" xmlns:r=\"" + kR + "\">" + para(textRun(cleanText(ps.header)), hp) + "</w:hdr>";
  }
  bool inList = false;
  int fig = 0;
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& b = d.blocks[k];
    // ---- the generated bibliography: consecutive Reference paragraphs with an entry key become one Word bibliography field
    if (b.kind == Block::Paragraph && b.p.style == PStyle::Reference && !b.p.refKey.empty() && !citeTags.empty()) {
      size_t j = k;
      while (j < d.blocks.size() && d.blocks[j].kind == Block::Paragraph && d.blocks[j].p.style == PStyle::Reference && !d.blocks[j].p.refKey.empty()) j++;
      string bib = "<w:sdt><w:sdtPr><w:id w:val=\"" + std::to_string(cx.nextSdtId++) + "\"/><w:bibliography/></w:sdtPr><w:sdtContent>";
      for (size_t q = k; q < j; q++) {
        PPr pp;
        pp.style = "Bibliography";
        string runs;
        if (q == k) runs += "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> BIBLIOGRAPHY </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>";
        runs += spanRuns(d.blocks[q].p.spans, RPr(), cx);
        if (q + 1 == j) runs += "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>";
        bib += para(runs, pp);
      }
      bib += "</w:sdtContent></w:sdt>";
      o += bib;
      k = j - 1;
      continue;
    }
    bool front = b.kind == Block::Paragraph && (b.p.style == PStyle::Title || b.p.style == PStyle::Subtitle || b.p.style == PStyle::Meta);
    if (!front && !frontDone) {
      frontDone = true;
      if (ps.toc && !hasTocBlock) { o += tocXml(); tocDone = true; }
    }
    bool listPara = b.kind == Block::Paragraph && isList(b.p.style);
    if (!listPara) inList = false;
    switch (b.kind) {
      case Block::Paragraph: {
        const Para& p = b.p;
        PPr pp;
        pp.jc = jcOf(p.align);
        RPr base;
        switch (p.style) {
          case PStyle::Title: pp.style = "Title"; break;
          case PStyle::Subtitle: pp.style = "Subtitle"; break;
          case PStyle::Meta: pp.style = "ReportMeta"; break;
          case PStyle::H1: case PStyle::H2: case PStyle::H3: case PStyle::H4: pp.style = "Heading" + std::to_string(headingLevel(p.style)); break;
          case PStyle::Quote: pp.style = "Quote"; break;
          case PStyle::Caption: pp.style = "Caption"; pp.keepNext = k + 1 < d.blocks.size() && d.blocks[k + 1].kind == Block::TableBlock; break;
          case PStyle::Reference: pp.style = "Bibliography"; break;
          case PStyle::Bullet: case PStyle::Number: pp.style = "ListParagraph"; break;
          default: break;
        }
        if (p.style == PStyle::Code) {
          PPr cp;
          cp.style = "SourceCode";
          vector<string> lines = split(p.text(), '\n', true);
          if (lines.empty()) lines.push_back("");
          for (auto& l : lines) o += para(textRun(cleanText(l)), cp);
          PPr sp;
          sp.style = "TableSpacer";
          o += para("", sp);
          break;
        }
        if (listPara) {
          int numId = 1;
          if (p.style == PStyle::Number) {
            if (!inList || p.numStart > 0 || cx.listStarts.empty()) cx.listStarts.push_back(std::max(1, p.numStart > 0 ? p.numStart : nums[k]));
            numId = 1 + int(cx.listStarts.size());
          }
          inList = true;
          pp.numPr = "<w:numPr><w:ilvl w:val=\"" + std::to_string(clampv(p.level, 0, 8)) + "\"/><w:numId w:val=\"" + std::to_string(numId) + "\"/></w:numPr>";
        } else if (p.level > 0 && p.style == PStyle::Body) pp.ind = "<w:ind w:left=\"" + std::to_string(720 * p.level) + "\"/>";
        string runs;
        if (isHeading(p.style) && ps.numberedHeadings && !hn[k].empty()) runs += textRun(hn[k] + "  ");
        if (p.style == PStyle::Caption) {
          int t = -1;
          if (k + 1 < d.blocks.size() && d.blocks[k + 1].kind == Block::TableBlock) t = int(k) + 1;
          else if (k > 0 && d.blocks[k - 1].kind == Block::TableBlock) t = int(k) - 1;
          if (t >= 0 && !captionLabelled(p.text())) {
            RPr lab;
            lab.b = true;
            runs += textRun("Table ", lab) + field("SEQ Table \\* ARABIC", std::to_string(d.tableNumber(t)), lab) + textRun(". ", lab);
          }
        }
        if (p.style == PStyle::Reference && numericStyle && p.refKey.empty()) runs += textRun("[" + std::to_string(d.refNumber(int(k))) + "]\t");
        runs += spanRuns(p.spans, base, cx);
        o += para(runs, pp);
        break;
      }
      case Block::FigureBlock: {
        fig++;
        int n = d.figureNumber(int(k));
        string bytes, svg;
        bool haveScene = b.fig.asset >= 0 && b.fig.asset < int(d.assets.size());
        if (haveScene && png) bytes = png(d.assets[size_t(b.fig.asset)].scene, dpi);
        if (haveScene && b.fig.formatOr(ps.figureFormat) == "vector") {
          svg = toSVG(d.assets[size_t(b.fig.asset)].scene, ps.serif());
          size_t start = svg.find("<svg");
          if (start != string::npos) svg = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + svg.substr(start);
        }
        auto wh = pngSize(bytes);
        int rid = 0;
        long long cxe = textEmu * std::max(20, std::min(100, b.fig.widthPct)) / 100, cy = cxe * 3 / 4;
        if (wh.first > 0 && wh.second > 0) {
          images.push_back(bytes);
          cx.svgParts.resize(images.size());
          cx.svgParts.back() = svg;
          rid = 100 + int(images.size());
          cy = (long long)(double(cxe) * double(wh.second) / double(wh.first));
          if (cy > maxFigEmu) { cy = maxFigEmu; cxe = (long long)(double(cy) * double(wh.first) / double(wh.second)); }
        } else if (haveScene && !svg.empty()) {  // no renderer (tests, headless): the SVG alone, sized from the scene
          const Scene& sc = d.assets[size_t(b.fig.asset)].scene;
          images.push_back("");
          cx.svgParts.resize(images.size());
          cx.svgParts.back() = svg;
          rid = 100 + int(images.size());
          if (sc.W > 0 && sc.H > 0) cy = (long long)(double(cxe) * sc.H / sc.W);
          if (cy > maxFigEmu) { cy = maxFigEmu; cxe = (long long)(double(cy) * sc.W / std::max(1.0, sc.H)); }
        }
        string cap = b.fig.caption.text();
        string capRuns;
        if (!captionLabelled(cap) && b.fig.label != "-") {
          RPr lab;
          lab.b = true;
          capRuns = textRun(b.fig.labelText() + " ", lab) + field("SEQ Figure \\* ARABIC", std::to_string(n), lab) + textRun(". ", lab);
        }
        PPr cp;
        cp.style = "Caption";
        cp.keepLines = true;
        cp.jc = b.fig.caption.align == PAlign::Left ? "center" : jcOf(b.fig.caption.align);
        string captionXml = para(capRuns + spanRuns(b.fig.caption.spans, RPr(), cx), cp);
        if (b.fig.captionAbove) { PPr cpa = cp; cpa.keepNext = true; captionXml = para(capRuns + spanRuns(b.fig.caption.spans, RPr(), cx), cpa); o += captionXml; }
        if (rid > 0) {
          string id = std::to_string(rid), nm = "Figure " + std::to_string(n), dp = std::to_string(cx.nextDrawing++);
          PPr pp;
          pp.keepNext = !b.fig.captionAbove;
          pp.spacing = "<w:spacing w:before=\"200\" w:after=\"60\"/>";
          pp.jc = b.fig.align == PAlign::Left ? "left" : b.fig.align == PAlign::Right ? "right" : "center";
          bool hasSvg = !svg.empty();
          string blip = hasSvg ? "<a:blip r:embed=\"rId" + id + "\"><a:extLst><a:ext uri=\"{96DAC541-7B7A-43D3-8B79-37D633B846F1}\"><asvg:svgBlip xmlns:asvg=\"http://schemas.microsoft.com/office/drawing/2016/SVG/main\" r:embed=\"rIdS" + id + "\"/></a:ext></a:extLst></a:blip>"
                               : "<a:blip r:embed=\"rId" + id + "\"/>";
          string ln = b.fig.border ? "<a:ln w=\"6350\"><a:solidFill><a:srgbClr val=\"B8BCC4\"/></a:solidFill></a:ln>" : "";
          o += "<w:p>" + pp.str() + "<w:r><w:drawing>"
               "<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"" + std::to_string(cxe) + "\" cy=\"" + std::to_string(cy) +
               "\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:docPr id=\"" + dp + "\" name=\"" + attr(nm) + "\" descr=\"" + attr(b.fig.alt.empty() ? cap : b.fig.alt) +
               "\"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
               "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
               "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"0\" name=\"figure" + std::to_string(images.size()) + ".png\"/><pic:cNvPicPr><a:picLocks noChangeAspect=\"1\" noChangeArrowheads=\"1\"/></pic:cNvPicPr></pic:nvPicPr>"
               "<pic:blipFill>" + blip + "<a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
               "<pic:spPr bwMode=\"auto\"><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + std::to_string(cxe) + "\" cy=\"" + std::to_string(cy) +
               "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/>" + ln + "</pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>";
        }
        if (!b.fig.captionAbove) o += captionXml;
        break;
      }
      case Block::TableBlock: o += docTableXml(b.tbl, cx, textTwips); break;
      case Block::Rule: {
        PPr pp;
        pp.pBdr = "<w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" w:color=\"BFC5CE\"/></w:pBdr>";
        pp.spacing = "<w:spacing w:before=\"60\" w:after=\"160\"/>";
        o += para("", pp);
        break;
      }
      case Block::PageBreak: o += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>"; break;
      case Block::Toc: o += tocXml(); tocDone = true; break;
    }
  }
  (void)tocDone;
  o += "<w:sectPr>" + string(cx.headerXml.empty() ? "" : "<w:headerReference w:type=\"default\" r:id=\"rIdHeader\"/>") + string(ps.pageNumbers ? "<w:footerReference w:type=\"default\" r:id=\"rIdFooter\"/>" : "") + "<w:pgSz w:w=\"" + std::to_string(pageW) + "\" w:h=\"" + std::to_string(pageH) + "\"/>"
       "<w:pgMar w:top=\"" + std::to_string(margin) + "\" w:right=\"" + std::to_string(margin) + "\" w:bottom=\"" + std::to_string(margin) + "\" w:left=\"" + std::to_string(margin) +
       "\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/><w:cols w:space=\"708\"/><w:docGrid w:linePitch=\"360\"/></w:sectPr>";
  o += "</w:body></w:document>";
  DocxMeta meta;
  meta.title = d.title();
  meta.creator = d.author;
  meta.keywords = d.keywords;
  meta.font = ps.font;
  meta.headingFont = ps.headingFont;
  meta.lineTwips = int(std::lround(clampv(ps.lineSpacing, 1.0f, 2.5f) * 240));
  meta.sizeHalfPt = int(std::lround(ps.baseSize * 2));
  g_citeTags = nullptr;
  return packDocx(o, images, cx, meta);
}

}  // namespace vs
