# PDFium — the PDF reader's engine

VOSStudio reads, renders, searches and annotates PDF files with **PDFium**, the PDF engine of Chromium
(https://pdfium.googlesource.com/pdfium/), used as an unmodified binary from the *pdfium-binaries* project
(https://github.com/bblanchon/pdfium-binaries, release `chromium/8076`, PDFium 156.0.8076.0).

PDFium is not compiled here and is never committed to this repository. `tools/fetch-pdfium.sh` downloads the
release archive, checks its SHA-256 against the value recorded in the script, and unpacks it into

    third_party/pdfium/win-x64/    bin/pdfium.dll, include/, LICENSE, licenses/   (embedded into VOSStudio.exe)
    third_party/pdfium/linux-x64/  lib/libpdfium.so, include/, LICENSE, licenses/ (used by `make test` on Linux)

Both directories are ignored by git.

## How the DLL travels inside the executable

`make` compresses `pdfium.dll` with the application's own deflate encoder (`tools/packres`, container described in
`src/core/inflate.h`) and links the result into the executable as the `PDFIUM_DLL` RCDATA resource, next to
`PDFIUM_LICENSES` (this file plus every notice below). On first use the application unpacks the DLL to
`%LOCALAPPDATA%\VOSStudio\pdfium\<build>\pdfium.dll` (with `LICENSES.txt` beside it), verifies the CRC, and loads
it with `LoadLibrary`. No separate DLL is shipped or installed. If the resource is absent (a build made without
the download), the application looks for `pdfium.dll` next to `VOSStudio.exe`.

The C API the application binds is listed in one place, `src/core/pdfium.h` (an X-macro table, resolved with
`GetProcAddress`/`dlsym`, no import library). `tools/pdfium_sigcheck.cpp` compiles that table against the release's
headers and fails on any signature or struct-layout difference — run it whenever the release is bumped.

## Licences

PDFium is licensed under the BSD 3-Clause licence (`licenses/pdfium.txt` in the release; Copyright 2014 The PDFium
Authors). The binary also contains the third-party components whose notices ship in `licenses/`:
abseil, agg23, fast_float, FreeType, ICU, Little-CMS, libjpeg-turbo, OpenJPEG, libpng, llvm-libc, simdutf and zlib.
The pdfium-binaries build scripts are MIT-licensed (`LICENSE` in the release, Copyright Benoit Blanchon).

All of these notices are reproduced in the application (Settings → About → Third-party notices) and written to
`LICENSES.txt` next to the extracted DLL, as the licences require.
