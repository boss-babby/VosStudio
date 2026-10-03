// The PDFium C API the application uses, bound at run time (no import library, no SDK at build time): the shell
// loads pdfium.dll / libpdfium.so and hands over a symbol resolver; every function below becomes a pointer in the
// `Pdfium` table. The declarations mirror PDFium's public headers (fpdfview.h, fpdf_text.h, fpdf_doc.h,
// fpdf_annot.h, fpdf_save.h) for the calls we make; tests/pdf_test.cpp exercises each of them against the real
// library, and tools/pdfium_sigcheck.cpp compares them with the headers when those are present.
//
// PDFium is not thread-safe: all calls go through one worker thread (pdfdoc.h).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace vs {
namespace pdf {

// ---- opaque handles and plain structs (layout as in the headers)
typedef void* FPDF_DOCUMENT;
typedef void* FPDF_PAGE;
typedef void* FPDF_BITMAP;
typedef void* FPDF_TEXTPAGE;
typedef void* FPDF_SCHHANDLE;
typedef void* FPDF_PAGELINK;
typedef void* FPDF_BOOKMARK;
typedef void* FPDF_DEST;
typedef void* FPDF_ACTION;
typedef void* FPDF_LINK;
typedef void* FPDF_ANNOTATION;
typedef void* FPDF_PAGEOBJECT;
typedef int FPDF_BOOL;
typedef unsigned long FPDF_DWORD;
typedef unsigned short FPDF_WCHAR;
typedef const unsigned short* FPDF_WIDESTRING;
typedef const char* FPDF_BYTESTRING;
typedef const char* FPDF_STRING;
typedef int FPDF_ANNOTATION_SUBTYPE;
typedef int FPDFANNOT_COLORTYPE;      // 0 = stroke/text colour ("C"), 1 = interior colour ("IC")
typedef int FPDF_ANNOT_APPEARANCEMODE;

struct FS_MATRIX { float a, b, c, d, e, f; };
struct FS_RECTF { float left, top, right, bottom; };
struct FS_SIZEF { float width, height; };
struct FS_POINTF { float x, y; };
struct FS_QUADPOINTSF { float x1, y1, x2, y2, x3, y3, x4, y4; };
struct FPDF_LIBRARY_CONFIG {  // we pass version 2; the later fields exist so the struct matches the header's size
  int version;
  const char** m_pUserFontPaths;
  void* m_pIsolate;
  unsigned int m_v8EmbedderSlot;
  void* m_pPlatform;
  int m_RendererType;      // version 4
  int m_FontLibraryType;   // version 5
  FPDF_BOOL m_BrotliEnabled;  // version 6
  FPDF_BOOL m_IsolatePerDocument;  // version 7
};
struct FPDF_IMAGEOBJ_METADATA {  // fpdf_edit.h
  unsigned int width, height;
  float horizontal_dpi, vertical_dpi;
  unsigned int bits_per_pixel;  // bits per component x components: 1 bilevel, 8 grey, 24 RGB, 32 CMYK
  int colorspace;               // FPDF_COLORSPACE_*: 1 DeviceGray, 2 DeviceRGB, 3 DeviceCMYK, 4 CalGray, 10 Indexed
  int marked_content_id;
};
struct FPDF_FILEACCESS {
  unsigned long m_FileLen;
  int (*m_GetBlock)(void* param, unsigned long position, unsigned char* pBuf, unsigned long size);
  void* m_Param;
};
struct FPDF_FILEWRITE {
  int version;
  int (*WriteBlock)(FPDF_FILEWRITE* pThis, const void* pData, unsigned long size);
};

// ---- constants
enum : int {
  FPDF_ANNOT = 0x01, FPDF_LCD_TEXT = 0x02, FPDF_NO_NATIVETEXT = 0x04, FPDF_GRAYSCALE = 0x08, FPDF_REVERSE_BYTE_ORDER = 0x10,
  FPDF_RENDER_LIMITEDIMAGECACHE = 0x200, FPDF_RENDER_FORCEHALFTONE = 0x400, FPDF_PRINTING = 0x800,
  FPDF_RENDER_NO_SMOOTHTEXT = 0x1000, FPDF_RENDER_NO_SMOOTHIMAGE = 0x2000, FPDF_RENDER_NO_SMOOTHPATH = 0x4000,
  FPDFBitmap_Gray = 1, FPDFBitmap_BGR = 2, FPDFBitmap_BGRx = 3, FPDFBitmap_BGRA = 4,
  FPDF_ERR_SUCCESS = 0, FPDF_ERR_UNKNOWN = 1, FPDF_ERR_FILE = 2, FPDF_ERR_FORMAT = 3, FPDF_ERR_PASSWORD = 4, FPDF_ERR_SECURITY = 5, FPDF_ERR_PAGE = 6,
  FPDF_MATCHCASE = 1, FPDF_MATCHWHOLEWORD = 2, FPDF_CONSECUTIVE = 4,
  PDFACTION_UNSUPPORTED = 0, PDFACTION_GOTO = 1, PDFACTION_REMOTEGOTO = 2, PDFACTION_URI = 3, PDFACTION_LAUNCH = 4, PDFACTION_EMBEDDEDGOTO = 5,
  FPDF_ANNOT_TEXT = 1, FPDF_ANNOT_LINK = 2, FPDF_ANNOT_FREETEXT = 3, FPDF_ANNOT_LINE = 4, FPDF_ANNOT_SQUARE = 5, FPDF_ANNOT_CIRCLE = 6,
  FPDF_ANNOT_HIGHLIGHT = 9, FPDF_ANNOT_UNDERLINE = 10, FPDF_ANNOT_SQUIGGLY = 11, FPDF_ANNOT_STRIKEOUT = 12, FPDF_ANNOT_INK = 15, FPDF_ANNOT_POPUP = 16,
  FPDFANNOT_COLORTYPE_Color = 0, FPDFANNOT_COLORTYPE_InteriorColor = 1,
  FPDF_ANNOT_FLAG_HIDDEN = 1 << 1, FPDF_ANNOT_FLAG_PRINT = 1 << 2, FPDF_ANNOT_FLAG_NOVIEW = 1 << 5,
  FPDF_INCREMENTAL = 1, FPDF_NO_INCREMENTAL = 2, FPDF_REMOVE_SECURITY = 3,
  FPDF_PAGEOBJ_TEXT = 1, FPDF_PAGEOBJ_PATH = 2, FPDF_PAGEOBJ_IMAGE = 3, FPDF_PAGEOBJ_SHADING = 4, FPDF_PAGEOBJ_FORM = 5,
};

// ---- the function table. X(name, return type, (parameter types))
#define VS_PDFIUM_FUNCTIONS(X)                                                                                                     \
  X(FPDF_InitLibraryWithConfig, void, (const FPDF_LIBRARY_CONFIG*))                                                                \
  X(FPDF_DestroyLibrary, void, ())                                                                                                 \
  X(FPDF_GetLastError, unsigned long, ())                                                                                          \
  X(FPDF_LoadCustomDocument, FPDF_DOCUMENT, (FPDF_FILEACCESS*, FPDF_BYTESTRING))                                                   \
  X(FPDF_LoadMemDocument64, FPDF_DOCUMENT, (const void*, size_t, FPDF_BYTESTRING))                                                 \
  X(FPDF_CloseDocument, void, (FPDF_DOCUMENT))                                                                                     \
  X(FPDF_GetPageCount, int, (FPDF_DOCUMENT))                                                                                       \
  X(FPDF_GetPageSizeByIndexF, FPDF_BOOL, (FPDF_DOCUMENT, int, FS_SIZEF*))                                                          \
  X(FPDF_LoadPage, FPDF_PAGE, (FPDF_DOCUMENT, int))                                                                                \
  X(FPDF_ClosePage, void, (FPDF_PAGE))                                                                                             \
  X(FPDF_GetPageWidthF, float, (FPDF_PAGE))                                                                                        \
  X(FPDF_GetPageHeightF, float, (FPDF_PAGE))                                                                                       \
  X(FPDF_GetPageBoundingBox, FPDF_BOOL, (FPDF_PAGE, FS_RECTF*))                                                                    \
  X(FPDFPage_GetRotation, int, (FPDF_PAGE))                                                                                        \
  X(FPDFText_IsGenerated, int, (FPDF_TEXTPAGE, int))                                                                               \
  X(FPDF_GetMetaText, unsigned long, (FPDF_DOCUMENT, FPDF_BYTESTRING, void*, unsigned long))                                       \
  X(FPDF_GetPageLabel, unsigned long, (FPDF_DOCUMENT, int, void*, unsigned long))                                                  \
  X(FPDFBitmap_CreateEx, FPDF_BITMAP, (int, int, int, void*, int))                                                                 \
  X(FPDFBitmap_FillRect, FPDF_BOOL, (FPDF_BITMAP, int, int, int, int, FPDF_DWORD))                                                 \
  X(FPDFBitmap_Destroy, void, (FPDF_BITMAP))                                                                                       \
  X(FPDF_RenderPageBitmap, void, (FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int, int))                                           \
  X(FPDF_RenderPageBitmapWithMatrix, void, (FPDF_BITMAP, FPDF_PAGE, const FS_MATRIX*, const FS_RECTF*, int))                       \
  X(FPDFText_LoadPage, FPDF_TEXTPAGE, (FPDF_PAGE))                                                                                 \
  X(FPDFText_ClosePage, void, (FPDF_TEXTPAGE))                                                                                     \
  X(FPDFText_CountChars, int, (FPDF_TEXTPAGE))                                                                                     \
  X(FPDFText_GetUnicode, unsigned int, (FPDF_TEXTPAGE, int))                                                                       \
  X(FPDFText_GetCharBox, FPDF_BOOL, (FPDF_TEXTPAGE, int, double*, double*, double*, double*))                                      \
  X(FPDFText_GetLooseCharBox, FPDF_BOOL, (FPDF_TEXTPAGE, int, FS_RECTF*))                                                          \
  X(FPDFText_GetFontSize, double, (FPDF_TEXTPAGE, int))                                                                            \
  X(FPDFText_GetFontInfo, unsigned long, (FPDF_TEXTPAGE, int, void*, unsigned long, int*))                                         \
  X(FPDFText_GetCharIndexAtPos, int, (FPDF_TEXTPAGE, double, double, double, double))                                              \
  X(FPDFText_GetText, int, (FPDF_TEXTPAGE, int, int, unsigned short*))                                                             \
  X(FPDFText_CountRects, int, (FPDF_TEXTPAGE, int, int))                                                                           \
  X(FPDFText_GetRect, FPDF_BOOL, (FPDF_TEXTPAGE, int, double*, double*, double*, double*))                                         \
  X(FPDFText_FindStart, FPDF_SCHHANDLE, (FPDF_TEXTPAGE, FPDF_WIDESTRING, unsigned long, int))                                      \
  X(FPDFText_FindNext, FPDF_BOOL, (FPDF_SCHHANDLE))                                                                                \
  X(FPDFText_GetSchResultIndex, int, (FPDF_SCHHANDLE))                                                                             \
  X(FPDFText_GetSchCount, int, (FPDF_SCHHANDLE))                                                                                   \
  X(FPDFText_FindClose, void, (FPDF_SCHHANDLE))                                                                                    \
  X(FPDFLink_LoadWebLinks, FPDF_PAGELINK, (FPDF_TEXTPAGE))                                                                         \
  X(FPDFLink_CountWebLinks, int, (FPDF_PAGELINK))                                                                                  \
  X(FPDFLink_GetURL, int, (FPDF_PAGELINK, int, unsigned short*, int))                                                              \
  X(FPDFLink_CountRects, int, (FPDF_PAGELINK, int))                                                                                \
  X(FPDFLink_GetRect, FPDF_BOOL, (FPDF_PAGELINK, int, int, double*, double*, double*, double*))                                    \
  X(FPDFLink_CloseWebLinks, void, (FPDF_PAGELINK))                                                                                 \
  X(FPDFLink_Enumerate, FPDF_BOOL, (FPDF_PAGE, int*, FPDF_LINK*))                                                                  \
  X(FPDFLink_GetAnnotRect, FPDF_BOOL, (FPDF_LINK, FS_RECTF*))                                                                      \
  X(FPDFLink_GetDest, FPDF_DEST, (FPDF_DOCUMENT, FPDF_LINK))                                                                       \
  X(FPDFLink_GetAction, FPDF_ACTION, (FPDF_LINK))                                                                                  \
  X(FPDFAction_GetType, unsigned long, (FPDF_ACTION))                                                                              \
  X(FPDFAction_GetDest, FPDF_DEST, (FPDF_DOCUMENT, FPDF_ACTION))                                                                   \
  X(FPDFAction_GetURIPath, unsigned long, (FPDF_DOCUMENT, FPDF_ACTION, void*, unsigned long))                                      \
  X(FPDFDest_GetDestPageIndex, int, (FPDF_DOCUMENT, FPDF_DEST))                                                                    \
  X(FPDFDest_GetLocationInPage, FPDF_BOOL, (FPDF_DEST, FPDF_BOOL*, FPDF_BOOL*, FPDF_BOOL*, float*, float*, float*))                \
  X(FPDFBookmark_GetFirstChild, FPDF_BOOKMARK, (FPDF_DOCUMENT, FPDF_BOOKMARK))                                                     \
  X(FPDFBookmark_GetNextSibling, FPDF_BOOKMARK, (FPDF_DOCUMENT, FPDF_BOOKMARK))                                                    \
  X(FPDFBookmark_GetTitle, unsigned long, (FPDF_BOOKMARK, void*, unsigned long))                                                   \
  X(FPDFBookmark_GetDest, FPDF_DEST, (FPDF_DOCUMENT, FPDF_BOOKMARK))                                                               \
  X(FPDFBookmark_GetAction, FPDF_ACTION, (FPDF_BOOKMARK))                                                                          \
  X(FPDFPage_GetAnnotCount, int, (FPDF_PAGE))                                                                                      \
  X(FPDFPage_GetAnnot, FPDF_ANNOTATION, (FPDF_PAGE, int))                                                                          \
  X(FPDFPage_CreateAnnot, FPDF_ANNOTATION, (FPDF_PAGE, FPDF_ANNOTATION_SUBTYPE))                                                   \
  X(FPDFPage_CloseAnnot, void, (FPDF_ANNOTATION))                                                                                  \
  X(FPDFPage_RemoveAnnot, FPDF_BOOL, (FPDF_PAGE, int))                                                                             \
  X(FPDFAnnot_GetSubtype, FPDF_ANNOTATION_SUBTYPE, (FPDF_ANNOTATION))                                                              \
  X(FPDFAnnot_SetColor, FPDF_BOOL, (FPDF_ANNOTATION, FPDFANNOT_COLORTYPE, unsigned int, unsigned int, unsigned int, unsigned int)) \
  X(FPDFAnnot_GetColor, FPDF_BOOL, (FPDF_ANNOTATION, FPDFANNOT_COLORTYPE, unsigned int*, unsigned int*, unsigned int*, unsigned int*)) \
  X(FPDFAnnot_AppendAttachmentPoints, FPDF_BOOL, (FPDF_ANNOTATION, const FS_QUADPOINTSF*))                                         \
  X(FPDFAnnot_CountAttachmentPoints, size_t, (FPDF_ANNOTATION))                                                                    \
  X(FPDFAnnot_GetAttachmentPoints, FPDF_BOOL, (FPDF_ANNOTATION, size_t, FS_QUADPOINTSF*))                                          \
  X(FPDFAnnot_SetRect, FPDF_BOOL, (FPDF_ANNOTATION, const FS_RECTF*))                                                              \
  X(FPDFAnnot_GetRect, FPDF_BOOL, (FPDF_ANNOTATION, FS_RECTF*))                                                                    \
  X(FPDFAnnot_SetStringValue, FPDF_BOOL, (FPDF_ANNOTATION, FPDF_BYTESTRING, FPDF_WIDESTRING))                                      \
  X(FPDFAnnot_GetStringValue, unsigned long, (FPDF_ANNOTATION, FPDF_BYTESTRING, FPDF_WCHAR*, unsigned long))                       \
  X(FPDFAnnot_SetFlags, FPDF_BOOL, (FPDF_ANNOTATION, int))                                                                         \
  X(FPDFAnnot_GetObjectCount, int, (FPDF_ANNOTATION))                                                                              \
  X(FPDFAnnot_GetObject, FPDF_PAGEOBJECT, (FPDF_ANNOTATION, int))                                                                  \
  X(FPDFPage_CountObjects, int, (FPDF_PAGE))                                                                                       \
  X(FPDFPage_GetObject, FPDF_PAGEOBJECT, (FPDF_PAGE, int))                                                                         \
  X(FPDFPageObj_GetType, int, (FPDF_PAGEOBJECT))                                                                                   \
  X(FPDFPageObj_GetBounds, FPDF_BOOL, (FPDF_PAGEOBJECT, float*, float*, float*, float*))                                           \
  X(FPDFPageObj_GetMatrix, FPDF_BOOL, (FPDF_PAGEOBJECT, FS_MATRIX*))                                                               \
  X(FPDFImageObj_GetImageMetadata, FPDF_BOOL, (FPDF_PAGEOBJECT, FPDF_PAGE, FPDF_IMAGEOBJ_METADATA*))                               \
  X(FPDFFormObj_CountObjects, int, (FPDF_PAGEOBJECT))                                                                              \
  X(FPDFFormObj_GetObject, FPDF_PAGEOBJECT, (FPDF_PAGEOBJECT, unsigned long))                                                      \
  X(FPDFPageObj_GetFillColor, FPDF_BOOL, (FPDF_PAGEOBJECT, unsigned int*, unsigned int*, unsigned int*, unsigned int*))            \
  X(FPDFPageObj_GetStrokeColor, FPDF_BOOL, (FPDF_PAGEOBJECT, unsigned int*, unsigned int*, unsigned int*, unsigned int*))          \
  X(FPDFAnnot_GetFlags, int, (FPDF_ANNOTATION))                                                                                    \
  X(FPDFAnnot_SetBorder, FPDF_BOOL, (FPDF_ANNOTATION, float, float, float))                                                        \
  X(FPDF_SaveAsCopy, FPDF_BOOL, (FPDF_DOCUMENT, FPDF_FILEWRITE*, FPDF_DWORD))

struct Pdfium {
#define VS_PDFIUM_DECL(name, ret, params) ret(*name) params = nullptr;
  VS_PDFIUM_FUNCTIONS(VS_PDFIUM_DECL)
#undef VS_PDFIUM_DECL
  // Optional experimental Ink APIs: keep basic PDF reading usable with older compatible DLLs.
  int (*FPDFAnnot_AddInkStroke)(FPDF_ANNOTATION, const FS_POINTF*, size_t) = nullptr;
  unsigned long (*FPDFAnnot_GetInkListCount)(FPDF_ANNOTATION) = nullptr;
  unsigned long (*FPDFAnnot_GetInkListPath)(FPDF_ANNOTATION, unsigned long, FS_POINTF*, unsigned long) = nullptr;
  FPDF_BOOL (*FPDFAnnot_GetBorder)(FPDF_ANNOTATION, float*, float*, float*) = nullptr;
};

// Binds every function through `resolve` (GetProcAddress / dlsym on the loaded library). Missing symbols are
// listed in *err and the table stays unbound. Does not initialise the library: the worker thread does that.
bool bind(void* (*resolve)(void* ctx, const char* name), void* ctx, std::string* err);
void unbind();
bool bound();
const Pdfium& api();  // valid while bound()

}  // namespace pdf
}  // namespace vs
