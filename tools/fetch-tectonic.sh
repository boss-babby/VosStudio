#!/usr/bin/env bash
# Fetch the pinned official Tectonic Windows x64 executable. The executable is compressed into VOSStudio.exe as
# an RCDATA resource; it is unpacked to the user's LocalAppData on first Writer PDF preview.
# Tectonic's TeX support bundle is managed and cached by Tectonic itself (the first compile may need network access).
set -euo pipefail

VERSION="${1:-0.17.0}"
TARGET="x86_64-pc-windows-msvc"
case "$VERSION" in
  0.17.0) SHA256="f61ce51f0b0ade1015b7de7ef368541c5424e9756ecbd0d7af97d6d48030845f" ;;
  *) echo "fetch-tectonic: no reviewed SHA-256 pin for version $VERSION" >&2; exit 2 ;;
esac
ARCHIVE_NAME="tectonic-${VERSION}-${TARGET}.zip"
URL="https://github.com/tectonic-typesetting/tectonic/releases/download/tectonic%40${VERSION}/${ARCHIVE_NAME}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION_DIR="$ROOT/build/tectonic/$VERSION"
ARCHIVE="$VERSION_DIR/$ARCHIVE_NAME"
UNPACK="$VERSION_DIR/unpacked"
DEST="$VERSION_DIR/win-x64"

mkdir -p "$VERSION_DIR"
if [ ! -s "$ARCHIVE" ]; then
  rm -f "$ARCHIVE.part"
  echo "fetching official Tectonic $VERSION ($TARGET) from $URL"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$ARCHIVE.part" "$URL"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$ARCHIVE.part" "$URL"
  else
    echo "fetch-tectonic: curl or wget is required" >&2
    exit 1
  fi
  mv "$ARCHIVE.part" "$ARCHIVE"
fi

if command -v sha256sum >/dev/null 2>&1; then
  GOT="$(sha256sum "$ARCHIVE" | cut -d' ' -f1)"
else
  GOT="$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)"
fi
if [ "$GOT" != "$SHA256" ]; then
  echo "fetch-tectonic: SHA-256 mismatch for $ARCHIVE_NAME: $GOT (expected $SHA256)" >&2
  rm -f "$ARCHIVE"
  exit 1
fi

if ! command -v unzip >/dev/null 2>&1; then
  echo "fetch-tectonic: unzip is required" >&2
  exit 1
fi
rm -rf "$UNPACK"
mkdir -p "$UNPACK" "$DEST"
unzip -q "$ARCHIVE" -d "$UNPACK"
BINARY="$(find "$UNPACK" -type f -name tectonic.exe -print -quit)"
if [ -z "$BINARY" ] || [ ! -s "$BINARY" ]; then
  echo "fetch-tectonic: verified archive did not contain tectonic.exe" >&2
  rm -rf "$UNPACK"
  exit 1
fi
cp "$BINARY" "$DEST/tectonic.exe.part"
mv "$DEST/tectonic.exe.part" "$DEST/tectonic.exe"
rm -rf "$UNPACK"
echo "Tectonic $VERSION verified and unpacked to $DEST/tectonic.exe"
