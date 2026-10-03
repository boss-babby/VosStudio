// Microsoft Word automation for the writer's clipboard: Word only treats a pasted CITATION field as a citation of its
// own when the source is in the document's *current list* (References > Manage Sources). Clipboard HTML cannot carry
// that list (Word ignores dataStoreItem links on paste), so the application hands the sources to Word directly through
// its automation model — Document.Bibliography.Sources.Add(xml), the call Word's own dialog uses — and, for
// "Paste into Word", pastes at Word's cursor and updates the fields. Late-bound IDispatch: no type library, works with
// every Word since 2007, and a missing / busy Word is reported, never fatal.
#include "platform.h"

#include <objbase.h>
#include <oleauto.h>

#include <set>

#include "../core/common.h"

namespace vs {
namespace win {

namespace {

struct Var : VARIANT {
  Var() { VariantInit(this); }
  ~Var() { VariantClear(this); }
  Var(const Var&) = delete;
  Var& operator=(const Var&) = delete;
  static Var str(const std::wstring& s) { Var v; v.vt = VT_BSTR; v.bstrVal = SysAllocStringLen(s.c_str(), UINT(s.size())); return v; }
  static Var i4(long n) { Var v; v.vt = VT_I4; v.lVal = n; return v; }
  static Var b(bool x) { Var v; v.vt = VT_BOOL; v.boolVal = x ? VARIANT_TRUE : VARIANT_FALSE; return v; }
  static Var missing() { Var v; v.vt = VT_ERROR; v.scode = DISP_E_PARAMNOTFOUND; return v; }
  Var(Var&& o) noexcept { VariantInit(this); std::swap(static_cast<VARIANT&>(*this), static_cast<VARIANT&>(o)); }
  IDispatch* disp() const { return vt == VT_DISPATCH ? pdispVal : nullptr; }
  long long num() const {
    switch (vt) {
      case VT_I4: return lVal; case VT_I2: return iVal; case VT_I8: return llVal; case VT_UI4: return ulVal; case VT_UI8: return (long long)ullVal;
      case VT_R8: return (long long)dblVal; case VT_R4: return (long long)fltVal; case VT_BOOL: return boolVal ? 1 : 0; case VT_INT: return intVal; case VT_UINT: return uintVal;
      default: return 0;
    }
  }
  string text() const { return vt == VT_BSTR && bstrVal ? narrow(bstrVal) : string(); }
};

// One late-bound call. args are given in natural order; IDispatch::Invoke wants them reversed.
HRESULT call(IDispatch* obj, WORD flags, const wchar_t* name, Var* result, std::vector<VARIANT> args = {}, string* err = nullptr) {
  if (!obj) return E_POINTER;
  DISPID id = 0;
  LPOLESTR nm = const_cast<LPOLESTR>(name);
  HRESULT hr = obj->GetIDsOfNames(IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, &id);
  if (FAILED(hr)) {
    if (err) *err = hr == RPC_E_SERVERCALL_RETRYLATER || hr == RPC_E_CALL_REJECTED ? string("Word is busy (a dialog is open?)") : "Word does not know '" + narrow(name) + "'";
    return hr;
  }
  std::vector<VARIANT> rev(args.rbegin(), args.rend());
  DISPPARAMS dp{rev.empty() ? nullptr : rev.data(), nullptr, UINT(rev.size()), 0};
  DISPID named = DISPID_PROPERTYPUT;
  if (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) { dp.rgdispidNamedArgs = &named; dp.cNamedArgs = 1; }
  EXCEPINFO ex{};
  UINT bad = 0;
  hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp, result, &ex, &bad);
  if (FAILED(hr) && err) {
    if (ex.bstrDescription) *err = narrow(ex.bstrDescription);
    else if (hr == RPC_E_SERVERCALL_RETRYLATER || hr == RPC_E_CALL_REJECTED) *err = "Word is busy (a dialog is open?)";
    else *err = "Word call '" + narrow(name) + "' failed (0x" + [&] { char b[16]; snprintf(b, sizeof b, "%08lX", (unsigned long)hr); return string(b); }() + ")";
  }
  SysFreeString(ex.bstrDescription);
  SysFreeString(ex.bstrSource);
  SysFreeString(ex.bstrHelpFile);
  return hr;
}
Var get(IDispatch* obj, const wchar_t* name, string* err = nullptr) { Var r; call(obj, DISPATCH_PROPERTYGET, name, &r, {}, err); return r; }
Var getArg(IDispatch* obj, const wchar_t* name, long i, string* err = nullptr) { Var r; Var a = Var::i4(i); call(obj, DISPATCH_PROPERTYGET | DISPATCH_METHOD, name, &r, {a}, err); return r; }
HRESULT put(IDispatch* obj, const wchar_t* name, const Var& v, string* err = nullptr) { return call(obj, DISPATCH_PROPERTYPUT, name, nullptr, {v}, err); }
HRESULT method(IDispatch* obj, const wchar_t* name, std::vector<VARIANT> args = {}, Var* result = nullptr, string* err = nullptr) { return call(obj, DISPATCH_METHOD, name, result, std::move(args), err); }

struct WordApp {
  IDispatch* app = nullptr;
  IDispatch* doc = nullptr;
  bool started = false;
  ~WordApp() { if (doc) doc->Release(); if (app) app->Release(); }
};

// The running Word (Running Object Table), or a new hidden instance when startWord.
bool attach(WordApp& w, bool startWord, string& err) {
  CLSID clsid;
  if (FAILED(CLSIDFromProgID(L"Word.Application", &clsid))) { err = "Microsoft Word is not installed."; return false; }
  IUnknown* unk = nullptr;
  HRESULT hr = GetActiveObject(clsid, nullptr, &unk);
  if (SUCCEEDED(hr) && unk) {
    hr = unk->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&w.app));
    unk->Release();
  }
  if (!w.app) {
    if (!startWord) { err = "Word is not running."; return false; }
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch, reinterpret_cast<void**>(&w.app));
    if (FAILED(hr) || !w.app) { err = "Word could not be started."; return false; }
    w.started = true;
    put(w.app, L"Visible", Var::b(true));
  }
  // the document the user is working in; a new one when Word has none open
  Var docs = get(w.app, L"Documents", &err);
  long long n = docs.disp() ? get(docs.disp(), L"Count").num() : 0;
  if (n > 0) {
    Var ad = get(w.app, L"ActiveDocument", &err);
    if (ad.disp()) { w.doc = ad.disp(); w.doc->AddRef(); }
  }
  if (!w.doc && docs.disp()) {
    Var nd;
    if (SUCCEEDED(method(docs.disp(), L"Add", {}, &nd, &err)) && nd.disp()) { w.doc = nd.disp(); w.doc->AddRef(); }
  }
  if (!w.doc) { if (err.empty()) err = "Word has no document to receive the sources."; return false; }
  return true;
}

// the sources into the document's current list; ones whose tag is already there are left alone
bool addSources(WordApp& w, const vector<WordSource>& sources, WordResult& r) {
  string err;
  Var bib = get(w.doc, L"Bibliography", &err);
  if (!bib.disp()) { r.error = err.empty() ? "The document has no bibliography object." : err; return false; }
  Var srcs = get(bib.disp(), L"Sources", &err);
  if (!srcs.disp()) { r.error = err.empty() ? "The document's source list is not reachable." : err; return false; }
  std::set<string> have;
  long long n = get(srcs.disp(), L"Count").num();
  for (long i = 1; i <= n; i++) {
    Var s = getArg(srcs.disp(), L"Item", i);
    if (s.disp()) { string t = get(s.disp(), L"Tag").text(); if (!t.empty()) have.insert(t); }
  }
  static const string kNs = "http://schemas.openxmlformats.org/officeDocument/2006/bibliography";
  for (auto& s : sources) {
    if (have.count(s.tag)) { r.present++; continue; }
    string xml = s.xml;
    if (xml.find("xmlns:b=") == string::npos) xml = replaceAll(xml, "<b:Source>", "<b:Source xmlns:b=\"" + kNs + "\">");
    Var a = Var::str(widen(xml));
    string e;
    if (SUCCEEDED(method(srcs.disp(), L"Add", {a}, nullptr, &e))) { r.added++; have.insert(s.tag); }
    else if (r.error.empty()) r.error = "Word refused a source (" + s.tag + "): " + e;
  }
  return true;
}

void toFront(WordApp& w) {
  Var hwnd = get(w.app, L"Hwnd");
  if (hwnd.num()) {
    HWND h = reinterpret_cast<HWND>(static_cast<INT_PTR>(hwnd.num()));
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
  }
  method(w.app, L"Activate");
}

}  // namespace

bool wordRunning() {
  CLSID clsid;
  if (FAILED(CLSIDFromProgID(L"Word.Application", &clsid))) return false;
  IUnknown* unk = nullptr;
  if (FAILED(GetActiveObject(clsid, nullptr, &unk)) || !unk) return false;
  unk->Release();
  return true;
}

WordResult wordAddSources(const vector<WordSource>& sources, bool startWord) {
  WordResult r;
  WordApp w;
  if (!attach(w, startWord, r.error)) return r;
  r.reachable = true;
  r.document = get(w.doc, L"Name").text();
  addSources(w, sources, r);
  return r;
}

WordResult wordPasteAtCursor(const vector<WordSource>& sources, bool startWord) {
  WordResult r;
  WordApp w;
  if (!attach(w, startWord, r.error)) return r;
  r.reachable = true;
  r.document = get(w.doc, L"Name").text();
  bool srcOk = addSources(w, sources, r);
  string err;
  Var sel = get(w.app, L"Selection", &err);
  if (!sel.disp()) { r.error = err.empty() ? "Word has no selection to paste at." : err; return r; }
  long start = long(get(sel.disp(), L"Start").num());
  if (FAILED(method(sel.disp(), L"Paste", {}, nullptr, &err))) { r.error = err.empty() ? "Word could not paste." : err; return r; }
  long end = long(get(sel.disp(), L"End").num());
  // the pasted citation fields get their text from Word's sources (and its current style) — only when every source is there
  if (srcOk && r.error.empty() && end > start) {
    Var a = Var::i4(start), b = Var::i4(end);
    Var rng;
    if (SUCCEEDED(method(w.doc, L"Range", {a, b}, &rng)) && rng.disp()) {
      Var fields = get(rng.disp(), L"Fields");
      if (fields.disp()) method(fields.disp(), L"Update");
    }
  }
  toFront(w);
  return r;
}

}  // namespace win
}  // namespace vs
