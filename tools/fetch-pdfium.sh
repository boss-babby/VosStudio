#!/usr/bin/env bash
# Downloads the PDFium binary release the reader is built against (bblanchon/pdfium-binaries, a plain build of
# Chromium's PDFium) and unpacks it under third_party/pdfium/<platform>/. The archive's SHA-256 is checked against
# the value recorded here, so a build always embeds exactly the reviewed binary.
#   tools/fetch-pdfium.sh win-x64      -> third_party/pdfium/win-x64/bin/pdfium.dll   (embedded into VOSStudio.exe)
#   tools/fetch-pdfium.sh linux-x64    -> third_party/pdfium/linux-x64/lib/libpdfium.so (for `make test` on Linux)
# Needs curl (or wget) and tar. Bump RELEASE + the hashes together; then re-run tools/pdfium_sigcheck.
set -euo pipefail
RELEASE="chromium/8076"
case "${1:-}" in
  win-x64)   FILE="pdfium-win-x64.tgz";   SHA="808d36da9bc5a3104315fb307c80998121f565ee53953633bf33e80d7429e5ac" ;;
  linux-x64) FILE="pdfium-linux-x64.tgz"; SHA="d9d67bc40af03aef4fe28a60b19b1086f28ace019c8c9caf19cb7fe3d14ceca3" ;;
  *) echo "usage: $0 win-x64|linux-x64" >&2; exit 2 ;;
esac
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$HERE/third_party/pdfium/$1"
TGZ="$HERE/third_party/pdfium/$FILE"
URL="https://github.com/bblanchon/pdfium-binaries/releases/download/$RELEASE/$FILE"
mkdir -p "$HERE/third_party/pdfium"
if [ ! -s "$TGZ" ]; then
  echo "fetching $URL"
  if command -v curl >/dev/null 2>&1; then curl -fsSL --retry 3 -o "$TGZ.part" "$URL"; else wget -q -O "$TGZ.part" "$URL"; fi
  mv "$TGZ.part" "$TGZ"
fi
GOT="$( (sha256sum "$TGZ" 2>/dev/null || shasum -a 256 "$TGZ") | cut -d' ' -f1)"
if [ "$GOT" != "$SHA" ]; then echo "checksum mismatch for $FILE: $GOT (expected $SHA)" >&2; rm -f "$TGZ"; exit 1; fi
rm -rf "$DEST"; mkdir -p "$DEST"
tar -xzf "$TGZ" -C "$DEST"
echo "pdfium $RELEASE unpacked into $DEST"
