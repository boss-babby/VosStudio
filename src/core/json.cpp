#include "json.h"

#include <cstdio>

namespace vs {

const Json& Json::operator[](const string& k) const {
  static Json nul;
  if (t != Obj) return nul;
  for (auto& kv : o) if (kv.first == k) return kv.second;
  return nul;
}
bool Json::has(const string& k) const {
  if (t != Obj) return false;
  for (auto& kv : o) if (kv.first == k) return true;
  return false;
}
Json& Json::set(const string& k, const Json& v) {
  if (t != Obj) { t = Obj; o.clear(); }
  for (auto& kv : o) if (kv.first == k) { kv.second = v; return kv.second; }
  o.emplace_back(k, v);
  return o.back().second;
}

static void dumpStr(string& out, const string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += char(c);
    }
  }
  out += '"';
}
static void dumpRec(const Json& j, string& out, int indent, int depth) {
  auto nl = [&](int d) { if (indent >= 0) { out += '\n'; out.append(static_cast<size_t>(d * indent), ' '); } };
  switch (j.t) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: {
      if (!std::isfinite(j.n)) { out += "null"; break; }
      char b[40];
      if (std::fabs(j.n - std::round(j.n)) < 1e-12 && std::fabs(j.n) < 1e15) snprintf(b, sizeof b, "%lld", (long long)std::llround(j.n));
      else snprintf(b, sizeof b, "%.10g", j.n);
      out += b;
      break;
    }
    case Json::Str: dumpStr(out, j.s); break;
    case Json::Arr:
      out += '[';
      for (size_t i = 0; i < j.a.size(); i++) {
        if (i) out += ',';
        nl(depth + 1);
        dumpRec(j.a[i], out, indent, depth + 1);
      }
      if (!j.a.empty()) nl(depth);
      out += ']';
      break;
    case Json::Obj:
      out += '{';
      for (size_t i = 0; i < j.o.size(); i++) {
        if (i) out += ',';
        nl(depth + 1);
        dumpStr(out, j.o[i].first);
        out += indent >= 0 ? ": " : ":";
        dumpRec(j.o[i].second, out, indent, depth + 1);
      }
      if (!j.o.empty()) nl(depth);
      out += '}';
      break;
  }
}
string Json::dump(int indent) const {
  string out;
  dumpRec(*this, out, indent, 0);
  return out;
}

namespace {
struct P {
  const string& s;
  size_t i = 0;
  string err;
  explicit P(const string& t) : s(t) {}
  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++; }
  bool fail(const char* m) { if (err.empty()) err = string(m) + " at " + std::to_string(i); return false; }
  static void utf8(string& o, uint32_t c) {
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 63)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
    else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 63)); o += char(0x80 | ((c >> 6) & 63)); o += char(0x80 | (c & 63)); }
  }
  bool str(string& out) {
    if (s[i] != '"') return fail("expected string");
    i++;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c == '\\' && i < s.size()) {
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (i + 4 > s.size()) return fail("bad escape");
            uint32_t c1 = (uint32_t)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
            i += 4;
            if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
              uint32_t c2 = (uint32_t)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
              i += 6;
              c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
            }
            utf8(out, c1);
            break;
          }
          default: out += e;
        }
      } else out += c;
    }
    if (i >= s.size()) return fail("unterminated string");
    i++;
    return true;
  }
  bool val(Json& j, int depth) {
    if (depth > 200) return fail("too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    char c = s[i];
    if (c == '{') {
      j = Json::object();
      i++;
      ws();
      if (i < s.size() && s[i] == '}') { i++; return true; }
      for (;;) {
        ws();
        string k;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return fail("expected :");
        i++;
        Json v;
        if (!val(v, depth + 1)) return false;
        j.o.emplace_back(std::move(k), std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == '}') { i++; return true; }
        return fail("expected , or }");
      }
    }
    if (c == '[') {
      j = Json::array();
      i++;
      ws();
      if (i < s.size() && s[i] == ']') { i++; return true; }
      for (;;) {
        Json v;
        if (!val(v, depth + 1)) return false;
        j.a.push_back(std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == ']') { i++; return true; }
        return fail("expected , or ]");
      }
    }
    if (c == '"') { j.t = Json::Str; return str(j.s); }
    if (s.compare(i, 4, "true") == 0) { j = Json(true); i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { j = Json(false); i += 5; return true; }
    if (s.compare(i, 4, "null") == 0) { j = Json(); i += 4; return true; }
    const char* st = s.c_str() + i;
    char* e = nullptr;
    double v = strtod(st, &e);
    if (e == st) return fail("bad value");
    i += size_t(e - st);
    j = Json(v);
    return true;
  }
};
}  // namespace

Json Json::parse(const string& text, string* err) {
  P p(text);
  Json j;
  if (!p.val(j, 0)) {
    if (err) *err = p.err;
    return Json();
  }
  if (err) err->clear();
  return j;
}

}  // namespace vs
