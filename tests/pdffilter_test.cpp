#include "pdfdoc.h"

#include <iostream>

using namespace vs::pdf;

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* label) {
    if (!ok) { ++fails; std::cerr << "FAIL: " << label << '\n'; }
  };

  Bitmap gray;
  gray.w = 1;
  gray.h = 1;
  gray.bgra = {0, 0, 0, 255};  // black, BGRA
  filterPage(gray, 1);
  check(gray.bgra == std::vector<uint8_t>({0, 0, 0, 255}), "warm paper leaves black ink black");

  Bitmap warm;
  warm.w = 2;
  warm.h = 1;
  warm.bgra = {255, 255, 255, 255, 0, 100, 200, 173};
  filterPage(warm, 1);
  check(warm.bgra[0] == 240 && warm.bgra[1] == 251 && warm.bgra[2] == 255 && warm.bgra[3] == 255,
        "warm paper gently shifts white toward cream");
  check(warm.bgra[4] == 0 && warm.bgra[5] == 99 && warm.bgra[6] == 200 && warm.bgra[7] == 173,
        "warm filter preserves red and alpha while subtly warming color");

  Bitmap mono;
  mono.w = 1;
  mono.h = 1;
  mono.bgra = {200, 150, 100, 91};
  filterPage(mono, 2);
  check(mono.bgra == std::vector<uint8_t>({143, 143, 143, 91}), "grayscale uses luminance and preserves alpha");

  Bitmap unchanged;
  unchanged.w = 1;
  unchanged.h = 1;
  unchanged.bgra = {1, 2, 3, 4};
  filterPage(unchanged, 0);
  check(unchanged.bgra == std::vector<uint8_t>({1, 2, 3, 4}), "mode zero leaves the bitmap unchanged");
  filterPage(unchanged, 9);
  check(unchanged.bgra == std::vector<uint8_t>({1, 2, 3, 4}), "unsupported mode leaves the bitmap unchanged");

  Bitmap shortBuffer;
  shortBuffer.w = 4;
  shortBuffer.h = 3;
  shortBuffer.bgra = {1, 2, 3, 4};
  filterPage(shortBuffer, 2);
  check(shortBuffer.bgra == std::vector<uint8_t>({1, 2, 3, 4}), "malformed partial bitmap is ignored safely");

  if (fails) return 1;
  std::cout << "pdffilter_test: all checks passed\n";
  return 0;
}
