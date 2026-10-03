// The PDF engine's loader (pdfload.cpp): finds or unpacks pdfium.dll and binds src/core/pdfium.h.
#pragma once
#include "platform.h"

namespace vs {
namespace win {

struct PdfEngine {
  bool ready = false;     // bound; the worker may initialise the library
  bool embedded = false;  // came out of the executable's resource (else pdfium.dll next to the executable)
  bool unpacked = false;  // this call wrote the DLL (first use of this build)
  string path, licensesPath, error;
};

PdfEngine pdfEngineLoad();  // call on the PDF worker thread (first use writes ~7 MB); idempotent
string engineThirdPartyNoticesText();  // notices for the bundled PDFium and Tectonic engines

}  // namespace win
}  // namespace vs
