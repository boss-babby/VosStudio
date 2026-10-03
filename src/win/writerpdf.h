// VOSStudio Native — the Writer's optional, asynchronous compiled-PDF preview.
#pragma once

#include "../core/pdfdoc.h"
#include "app.h"

namespace vs {
namespace win {

struct WriterPdfPagePreview {
  float w = 612, h = 792;              // PDF points
  Com<ID2D1Bitmap> bitmap;             // low-resolution page preview; kept in a small LRU
  int wantedW = 0;                     // target bitmap width, quantised on viewport resize
  uint32_t requestToken = 0;
  bool requested = false, failed = false;
};

struct WriterPdfPreview {
  int docId = -1;                      // opened on the shared PDFium worker, separate from the reading-library document
  uint32_t generation = 0;
  bool loading = false, ready = false, failed = false;
  string path, error;
  pdf::DocInfo info;
  vector<WriterPdfPagePreview> pages;
  vector<float> pageTop, pageBottom, pageW, pageH;
  float layoutW = -1, layoutScale = -1, contentH = 0;
  vector<int> bitmapLru;               // at most eight visible/visited page bitmaps
};

}  // namespace win
}  // namespace vs
