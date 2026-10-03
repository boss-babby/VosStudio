// A small DEFLATE decoder (RFC 1951: stored, fixed and dynamic Huffman blocks) and the container the application
// uses for the data it embeds in its own executable (the PDF engine, licence texts): a 16-byte header, the raw
// deflate stream, and a CRC-32 of the original bytes. Everything is portable; the shell only supplies the bytes.
#pragma once
#include "common.h"

namespace vs {

// Decodes a raw deflate stream (no zlib / gzip wrapper). Returns false on malformed input. `expect` (when > 0)
// is the known size of the output and lets the decoder reserve it once.
bool inflateRaw(const uint8_t* data, size_t n, string& out, size_t expect = 0, string* err = nullptr);
// The same for a zlib stream (2-byte header, Adler-32 trailer ignored).
bool inflateZlib(const uint8_t* data, size_t n, string& out, size_t expect = 0, string* err = nullptr);

// The embedded-blob container:  "VSPK" | u32 tag | u32 rawSize | u32 zSize | deflate bytes | u32 crc32(raw)
// tag identifies the content (e.g. the PDFium build number). A blob with rawSize == 0 means "not embedded".
string packBlob(uint32_t tag, const string& raw);                  // deflate (the in-house encoder) + header + crc
bool unpackBlob(const uint8_t* data, size_t n, string& raw, uint32_t* tag = nullptr, string* err = nullptr);
bool blobInfo(const uint8_t* data, size_t n, uint32_t* tag, uint32_t* rawSize);  // header only, no decoding

}  // namespace vs
