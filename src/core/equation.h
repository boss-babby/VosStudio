// LaTeX math helpers shared by Writer, HTML/Word export, and host tests.
#pragma once
#include "common.h"

namespace vs {

// Convert the supported LaTeX math subset to native Office Math Markup Language (OMML),
// MathML, or a readable linear Unicode preview. Unsupported TeX commands are kept as text
// so the source is never silently discarded; syntax/size errors return an empty export.
string mathToOmml(const string& latex, string* error = nullptr);
string mathToMathML(const string& latex, string* error = nullptr);
string mathPreviewText(const string& latex);

}  // namespace vs
