// packres: build-time helper that turns files into the compressed blobs the executable carries as RCDATA resources
// (see src/core/inflate.h for the container). Built with the host compiler (`make build/host/packres`).
//   packres pack <tag> <out> <file>            one file  -> blob   (tag = 4 ASCII chars, e.g. PDFM)
//   packres bundle <tag> <out> <file>...       text files -> one blob, each preceded by "==== <basename> ====\n"
// When the input file of `pack` does not exist, an *absent* blob (header only, raw size 0) is written and a warning
// printed, so a build without the PDFium download still links: the application then looks for pdfium.dll next to
// the executable instead. This keeps `make` working offline.
#include <cstdio>
#include <cstring>
#include <iostream>

#include "common.h"
#include "inflate.h"

using namespace vs;

static uint32_t tagOf(const char* s) {
  if (strlen(s) != 4) { std::cerr << "packres: tag must be 4 characters\n"; exit(2); }
  return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint32_t(uint8_t(s[3]));
}

int main(int argc, char** argv) {
  if (argc < 4) { std::cerr << "usage: packres pack <tag> <out> <file> | packres bundle <tag> <out> <file>...\n"; return 2; }
  string mode = argv[1], out = argv[3];
  uint32_t tag = tagOf(argv[2]);
  string raw;
  if (mode == "pack") {
    if (argc != 5) { std::cerr << "packres pack <tag> <out> <file>\n"; return 2; }
    string in = argv[4];
    if (!fileExistsU(in)) {
      std::cerr << "packres: WARNING " << in << " is missing — writing an absent blob (the PDF reader will need pdfium.dll next to the executable)\n";
      string blob = packBlob(tagOf("ABSN"), "");
      return writeFile(out, blob) ? 0 : 1;
    }
    bool ok = false;
    raw = readFile(in, &ok);
    if (!ok) { std::cerr << "packres: cannot read " << in << "\n"; return 1; }
  } else if (mode == "bundle") {
    for (int i = 4; i < argc; i++) {
      bool ok = false;
      string s = readFile(argv[i], &ok);
      if (!ok) { std::cerr << "packres: cannot read " << argv[i] << "\n"; return 1; }
      string name = argv[i];
      size_t k = name.find_last_of("/\\");
      if (k != string::npos) name = name.substr(k + 1);
      raw += "==== " + name + " ====\n" + s;
      if (raw.empty() || raw.back() != '\n') raw += '\n';
      raw += '\n';
    }
  } else { std::cerr << "packres: unknown mode " << mode << "\n"; return 2; }
  string blob = packBlob(tag, raw);
  // paranoia: what we wrote must come back byte for byte
  string back;
  string err;
  if (!unpackBlob(reinterpret_cast<const uint8_t*>(blob.data()), blob.size(), back, nullptr, &err) || back != raw) { std::cerr << "packres: self-check failed: " << err << "\n"; return 1; }
  if (!writeFile(out, blob)) { std::cerr << "packres: cannot write " << out << "\n"; return 1; }
  std::cout << "packres: " << out << " " << raw.size() << " -> " << blob.size() << " bytes\n";
  return 0;
}
