#include "pdfium.h"

namespace vs {
namespace pdf {

namespace {
Pdfium g_api;
bool g_bound = false;
}  // namespace

bool bind(void* (*resolve)(void* ctx, const char* name), void* ctx, std::string* err) {
  Pdfium t;
  std::string missing;
#define VS_PDFIUM_BIND(name, ret, params)                                                     \
  t.name = reinterpret_cast<ret(*) params>(resolve(ctx, #name));                              \
  if (!t.name) missing += std::string(missing.empty() ? "" : ", ") + #name;
  VS_PDFIUM_FUNCTIONS(VS_PDFIUM_BIND)
#undef VS_PDFIUM_BIND
  // Ink creation was added as an experimental PDFium API; keep the reader usable with older compatible DLLs.
  t.FPDFAnnot_AddInkStroke = reinterpret_cast<decltype(t.FPDFAnnot_AddInkStroke)>(resolve(ctx, "FPDFAnnot_AddInkStroke"));
  t.FPDFAnnot_GetInkListCount = reinterpret_cast<decltype(t.FPDFAnnot_GetInkListCount)>(resolve(ctx, "FPDFAnnot_GetInkListCount"));
  t.FPDFAnnot_GetInkListPath = reinterpret_cast<decltype(t.FPDFAnnot_GetInkListPath)>(resolve(ctx, "FPDFAnnot_GetInkListPath"));
  t.FPDFAnnot_GetBorder = reinterpret_cast<decltype(t.FPDFAnnot_GetBorder)>(resolve(ctx, "FPDFAnnot_GetBorder"));
  if (!missing.empty()) {
    if (err) *err = "PDF engine: missing functions (an older pdfium build?): " + missing;
    return false;
  }
  g_api = t;
  g_bound = true;
  return true;
}

void unbind() { g_api = Pdfium(); g_bound = false; }
bool bound() { return g_bound; }
const Pdfium& api() { return g_api; }

}  // namespace pdf
}  // namespace vs
