#include "inflate.h"

#include <cstring>

namespace vs {

namespace {

// Canonical Huffman decoding tables as in RFC 1951 §3.2.2 / zlib's puff: `count[len]` codes of each length and the
// symbols ordered by code. Decoding walks the lengths bit by bit — simple, and fast enough for a few megabytes.
struct Huff {
  uint16_t count[16] = {0};
  uint16_t symbol[320] = {0};
  bool build(const uint8_t* lengths, int n) {
    memset(count, 0, sizeof count);
    for (int i = 0; i < n; i++) count[lengths[i]]++;
    if (count[0] == n) return true;  // no codes at all (allowed for the distance tree)
    int left = 1;
    for (int len = 1; len < 16; len++) { left <<= 1; left -= count[len]; if (left < 0) return false; }  // over-subscribed
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = uint16_t(offs[len] + count[len]);
    for (int i = 0; i < n; i++) if (lengths[i]) symbol[offs[lengths[i]]++] = uint16_t(i);
    return true;
  }
};

struct State {
  const uint8_t* in;
  size_t n, pos = 0;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  string* out;
  bool bad = false;
  int bits(int need) {
    uint32_t v = bitbuf;
    while (bitcnt < need) {
      if (pos >= n) { bad = true; return 0; }
      v |= uint32_t(in[pos++]) << bitcnt;
      bitcnt += 8;
    }
    bitbuf = v >> need;
    bitcnt -= need;
    return int(v & ((1u << need) - 1));
  }
  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
      code |= bits(1);
      if (bad) return -1;
      int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
};

const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(State& s, const Huff& lencode, const Huff& distcode) {
  string& out = *s.out;
  for (;;) {
    int sym = s.decode(lencode);
    if (sym < 0) return false;
    if (sym < 256) out.push_back(char(sym));
    else if (sym == 256) return true;
    else {
      sym -= 257;
      if (sym >= 29) return false;
      int len = kLenBase[sym] + s.bits(kLenExtra[sym]);
      int ds = s.decode(distcode);
      if (ds < 0 || ds >= 30) return false;
      size_t dist = kDistBase[ds] + size_t(s.bits(kDistExtra[ds]));
      if (s.bad || dist > out.size()) return false;
      size_t from = out.size() - dist;
      for (int i = 0; i < len; i++) out.push_back(out[from + size_t(i)]);  // byte by byte: overlapping copies are the point
    }
  }
}

bool stored(State& s) {
  s.bitbuf = 0;
  s.bitcnt = 0;  // drop to a byte boundary
  if (s.pos + 4 > s.n) return false;
  unsigned len = s.in[s.pos] | (s.in[s.pos + 1] << 8), nlen = s.in[s.pos + 2] | (s.in[s.pos + 3] << 8);
  s.pos += 4;
  if (len != (~nlen & 0xFFFF) || s.pos + len > s.n) return false;
  s.out->append(reinterpret_cast<const char*>(s.in + s.pos), len);
  s.pos += len;
  return true;
}

bool fixedBlock(State& s) {
  struct Tables {
    Huff lencode, distcode;
    Tables() {
      uint8_t lengths[288];
      int i = 0;
      for (; i < 144; i++) lengths[i] = 8;
      for (; i < 256; i++) lengths[i] = 9;
      for (; i < 280; i++) lengths[i] = 7;
      for (; i < 288; i++) lengths[i] = 8;
      lencode.build(lengths, 288);
      uint8_t dl[30];
      for (i = 0; i < 30; i++) dl[i] = 5;
      distcode.build(dl, 30);
    }
  };
  // C++ guarantees thread-safe initialization of a function-local static. PDFium and Tectonic can both be
  // unpacked on different worker threads during first use, so the shared fixed Huffman tables must be published
  // atomically rather than guarded by a racy boolean.
  static const Tables tables;
  return codes(s, tables.lencode, tables.distcode);
}

bool dynamicBlock(State& s) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  int nlen = s.bits(5) + 257, ndist = s.bits(5) + 1, ncode = s.bits(4) + 4;
  if (s.bad || nlen > 286 || ndist > 30) return false;
  uint8_t lengths[320] = {0};
  for (int i = 0; i < ncode; i++) lengths[order[i]] = uint8_t(s.bits(3));
  Huff lencode, distcode;
  if (!lencode.build(lengths, 19)) return false;
  int index = 0;
  while (index < nlen + ndist) {
    int sym = s.decode(lencode);
    if (sym < 0) return false;
    if (sym < 16) lengths[index++] = uint8_t(sym);
    else {
      int len = 0, rep;
      if (sym == 16) { if (index == 0) return false; len = lengths[index - 1]; rep = 3 + s.bits(2); }
      else if (sym == 17) rep = 3 + s.bits(3);
      else rep = 11 + s.bits(7);
      if (s.bad || index + rep > nlen + ndist) return false;
      while (rep--) lengths[index++] = uint8_t(len);
    }
  }
  if (lengths[256] == 0) return false;  // no end-of-block code
  if (!lencode.build(lengths, nlen)) return false;
  if (!distcode.build(lengths + nlen, ndist)) return false;
  return codes(s, lencode, distcode);
}

}  // namespace

bool inflateRaw(const uint8_t* data, size_t n, string& out, size_t expect, string* err) {
  out.clear();
  if (expect) out.reserve(expect);
  State s{data, n, 0, 0, 0, &out};
  int last;
  do {
    last = s.bits(1);
    int type = s.bits(2);
    if (s.bad) { if (err) *err = "deflate: truncated"; return false; }
    bool ok = type == 0 ? stored(s) : type == 1 ? fixedBlock(s) : type == 2 ? dynamicBlock(s) : false;
    if (!ok || s.bad) { if (err) *err = "deflate: malformed block"; return false; }
  } while (!last);
  return true;
}

bool inflateZlib(const uint8_t* data, size_t n, string& out, size_t expect, string* err) {
  if (n < 2 || (data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0) { if (err) *err = "zlib: bad header"; return false; }
  if (data[1] & 0x20) { if (err) *err = "zlib: preset dictionary"; return false; }
  return inflateRaw(data + 2, n - 2, out, expect, err);
}

namespace {
void put32(string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back(char((v >> (8 * i)) & 0xFF)); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

string packBlob(uint32_t tag, const string& raw) {
  string z;
  if (!raw.empty()) {
    string zl = zlibCompress(raw);  // zlib wrapper: 2-byte header, 4-byte Adler-32 trailer — stored raw here
    if (zl.size() > 6) z = zl.substr(2, zl.size() - 6);
  }
  string out = "VSPK";
  put32(out, tag);
  put32(out, uint32_t(raw.size()));
  put32(out, uint32_t(z.size()));
  out += z;
  put32(out, crc32(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()));
  return out;
}

bool blobInfo(const uint8_t* data, size_t n, uint32_t* tag, uint32_t* rawSize) {
  if (n < 20 || memcmp(data, "VSPK", 4) != 0) return false;
  if (tag) *tag = get32(data + 4);
  if (rawSize) *rawSize = get32(data + 8);
  return true;
}

bool unpackBlob(const uint8_t* data, size_t n, string& raw, uint32_t* tag, string* err) {
  uint32_t rawSize = 0;
  if (!blobInfo(data, n, tag, &rawSize)) { if (err) *err = "not an embedded blob"; return false; }
  uint32_t zSize = get32(data + 12);
  if (size_t(zSize) + 20 > n) { if (err) *err = "embedded blob truncated"; return false; }
  if (rawSize == 0) { raw.clear(); return true; }
  if (!inflateRaw(data + 16, zSize, raw, rawSize, err)) return false;
  if (raw.size() != rawSize) { if (err) *err = "embedded blob: size mismatch"; return false; }
  uint32_t crc = get32(data + 16 + zSize);
  if (crc32(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) != crc) { if (err) *err = "embedded blob: checksum mismatch"; return false; }
  return true;
}

}  // namespace vs
