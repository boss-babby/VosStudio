// pdfium_sigcheck: compiles the function table in src/core/pdfium.h against the PDFium release headers and fails
// to compile on any difference (return/parameter types, struct sizes and offsets). Run it whenever the PDFium
// release in tools/fetch-pdfium.sh is bumped:
//   g++ -std=c++17 -Ithird_party/pdfium/linux-x64/include -Isrc/core tools/pdfium_sigcheck.cpp -o build/host/sigcheck && build/host/sigcheck
// Types are compared structurally (any pointer matches any pointer; integers and enums of the same size match), since
// the table uses void* handles and int where the headers use typedefs and enums. The constants (FPDF_ANNOT_*, error
// codes, flags) are copied by hand and were compared value by value when the table was written.
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <cstdio>
namespace mine {
#include "pdfium.h"
}
#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_doc.h"
#include "fpdf_annot.h"
#include "fpdf_save.h"
#include "fpdf_edit.h"
namespace mine { namespace vs { namespace pdf {
template <class T> struct SizeOf : std::integral_constant<size_t, sizeof(T)> {};
template <> struct SizeOf<void> : std::integral_constant<size_t, 0> {};
template <class A, class B> struct Compat : std::integral_constant<bool, std::is_same<A, B>::value || (std::is_pointer<A>::value && std::is_pointer<B>::value) ||
    ((std::is_integral<A>::value || std::is_enum<A>::value) && (std::is_integral<B>::value || std::is_enum<B>::value) && SizeOf<A>::value == SizeOf<B>::value) ||
    (std::is_floating_point<A>::value && std::is_floating_point<B>::value && SizeOf<A>::value == SizeOf<B>::value)> {};
template <class... X> struct Pack {};
template <class X, class Y> struct All : std::false_type {};
template <> struct All<Pack<>, Pack<>> : std::true_type {};
template <class X0, class... Xs, class Y0, class... Ys> struct All<Pack<X0, Xs...>, Pack<Y0, Ys...>> : std::integral_constant<bool, Compat<X0, Y0>::value && All<Pack<Xs...>, Pack<Ys...>>::value> {};
template <class A, class B> struct SigCompat : std::false_type {};
template <class RA, class RB, class... PA, class... PB> struct SigCompat<RA(*)(PA...), RB(*)(PB...)> : std::integral_constant<bool, Compat<RA, RB>::value && sizeof...(PA) == sizeof...(PB) && All<Pack<PA...>, Pack<PB...>>::value> {};
#define VS_CHECK(name, ret, params) static_assert(SigCompat<decltype(&::name), ret(*) params>::value, "signature mismatch: " #name);
VS_PDFIUM_FUNCTIONS(VS_CHECK)
static_assert(sizeof(FS_MATRIX) == sizeof(::FS_MATRIX) && sizeof(FS_RECTF) == sizeof(::FS_RECTF) && sizeof(FS_POINTF) == sizeof(::FS_POINTF) && sizeof(FS_QUADPOINTSF) == sizeof(::FS_QUADPOINTSF), "struct size");
static_assert(sizeof(FPDF_LIBRARY_CONFIG) == sizeof(::FPDF_LIBRARY_CONFIG) && sizeof(FPDF_FILEACCESS) == sizeof(::FPDF_FILEACCESS) && sizeof(FPDF_FILEWRITE) == sizeof(::FPDF_FILEWRITE), "struct size 2");
static_assert(SigCompat<decltype(&::FPDFAnnot_AddInkStroke), int(*)(FPDF_ANNOTATION, const FS_POINTF*, size_t)>::value, "signature mismatch: FPDFAnnot_AddInkStroke");
static_assert(SigCompat<decltype(&::FPDFAnnot_GetInkListCount), unsigned long(*)(FPDF_ANNOTATION)>::value, "signature mismatch: FPDFAnnot_GetInkListCount");
static_assert(SigCompat<decltype(&::FPDFAnnot_GetInkListPath), unsigned long(*)(FPDF_ANNOTATION, unsigned long, FS_POINTF*, unsigned long)>::value, "signature mismatch: FPDFAnnot_GetInkListPath");
static_assert(SigCompat<decltype(&::FPDFAnnot_GetBorder), FPDF_BOOL(*)(FPDF_ANNOTATION, float*, float*, float*)>::value, "signature mismatch: FPDFAnnot_GetBorder");
static_assert(offsetof(FPDF_FILEACCESS, m_Param) == offsetof(::FPDF_FILEACCESS, m_Param) && offsetof(FPDF_LIBRARY_CONFIG, m_RendererType) == offsetof(::FPDF_LIBRARY_CONFIG, m_RendererType), "offsets");
}}}
int main() { std::puts("pdfium signatures OK"); }
