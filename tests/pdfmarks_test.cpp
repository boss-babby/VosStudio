#include "pdfdoc.h"

#include <iostream>

using namespace vs::pdf;

namespace {
Bitmap whitePage(int w, int h) {
  Bitmap bm;
  bm.w = w;
  bm.h = h;
  bm.bgra.assign(size_t(w) * size_t(h) * 4, 255);
  return bm;
}
uint32_t pixel(const Bitmap& bm, int x, int y) {
  const uint8_t* p = bm.bgra.data() + (size_t(y) * size_t(bm.w) + size_t(x)) * 4;
  return (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}
bool blueMark(uint32_t c) {
  const int r = int((c >> 16) & 255), g = int((c >> 8) & 255), b = int(c & 255);
  return b > r + 20 && b > g + 20 && b > 40;
}
}

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* label) {
    if (!ok) { ++fails; std::cerr << "FAIL: " << label << '\n'; }
  };

  Mark ink;
  ink.kind = 5;
  ink.rgb = 0x2255AA;
  ink.strokeWidth = 2;
  InkStroke path;
  path.points = {{300, 600}, {330, 600}, {360, 600}};
  ink.strokes.push_back(path);

  Bitmap page = whitePage(612, 792);
  burnMarks(page, 1.0, 0, 612, 792, 0, 0, {ink});
  check(blueMark(pixel(page, 330, 600)), "freehand stroke is burned into an unrotated print bitmap");
  check(pixel(page, 330, 590) == 0xFFFFFF, "freehand burn stays local to its path");

  Bitmap turned = whitePage(792, 612);
  burnMarks(turned, 1.0, 1, 612, 792, 0, 0, {ink});
  check(blueMark(pixel(turned, 192, 330)), "freehand stroke follows the reader's quarter-turn rotation");
  check(pixel(turned, 330, 192) == 0xFFFFFF, "rotated stroke is not burned at the unrotated coordinates");

  if (fails) return 1;
  std::cout << "pdfmarks_test: all checks passed\n";
  return 0;
}
