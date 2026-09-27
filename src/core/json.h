// Minimal JSON value / parser / writer (bundles, OpenAlex, settings)
#pragma once
#include "common.h"

namespace vs {

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false;
  double n = 0;
  string s;
  vector<Json> a;
  vector<std::pair<string, Json>> o;  // keeps insertion order

  Json() = default;
  Json(bool v) : t(Bool), b(v) {}
  Json(int v) : t(Num), n(v) {}
  Json(long long v) : t(Num), n(double(v)) {}
  Json(double v) : t(Num), n(v) {}
  Json(const char* v) : t(Str), s(v) {}
  Json(const string& v) : t(Str), s(v) {}
  static Json array() { Json j; j.t = Arr; return j; }
  static Json object() { Json j; j.t = Obj; return j; }

  bool isNull() const { return t == Null; }
  const Json& operator[](const string& k) const;
  const Json& operator[](size_t i) const { static Json nul; return i < a.size() ? a[i] : nul; }
  Json& set(const string& k, const Json& v);
  Json& push(const Json& v) { if (t != Arr) { t = Arr; a.clear(); } a.push_back(v); return a.back(); }
  bool has(const string& k) const;
  size_t size() const { return t == Arr ? a.size() : t == Obj ? o.size() : 0; }

  double num(double def = 0) const { return t == Num ? n : t == Str ? toDouble(s, def) : t == Bool ? (b ? 1 : 0) : def; }
  int integer(int def = 0) const { return t == Null ? def : int(std::lround(num(def))); }
  string str(const string& def = "") const { return t == Str ? s : t == Num ? fmtNum(n, 6) : def; }
  bool boolean(bool def = false) const { return t == Bool ? b : t == Num ? n != 0 : def; }

  string dump(int indent = -1) const;
  static Json parse(const string& text, string* err = nullptr);
};

}  // namespace vs
