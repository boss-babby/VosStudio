#include "pdfdoc.h"

#include <iostream>

using namespace vs;
using namespace vs::pdf;

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* label) {
    if (!ok) { ++fails; std::cerr << "FAIL: " << label << '\n'; }
  };

  check(joinSelectionLines("bio-\nmetrics") == "biometrics", "ASCII hyphen at a wrapped lowercase word");
  check(joinSelectionLines("cross-\r\nborder") == "crossborder", "CRLF wrapped word");
  check(joinSelectionLines(u8"é-\ncole") == u8"école", "UTF-8 word following a hyphen");
  check(joinSelectionLines(u8"inter‐\nnational") == "international", "Unicode hyphen at a wrapped word");
  check(joinSelectionLines(u8"co\u00AD\noperate") == "cooperate", "soft hyphen at a wrapped word");
  check(joinSelectionLines("well-\nKnown") == "well- Known", "uppercase next line is not a word-wrap join");
  check(joinSelectionLines(u8"well–\nknown") == u8"well– known", "en dash is not deleted");
  check(joinSelectionLines("a\n  b\n c") == "a b c", "paragraph/line whitespace is collapsed");
  check(joinSelectionLines("\n\t") == "", "whitespace-only selection");

  if (fails) return 1;
  std::cout << "pdftext_test: all checks passed\n";
  return 0;
}
