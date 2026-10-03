# VOSStudio Native: Makefile (MSYS2 MinGW-w64 on Windows, or cross-compiling from Linux)
#   MSYS2 (UCRT64/MINGW64 shell):  make -j
#   Linux cross-compile:           make -j CROSS=x86_64-w64-mingw32-
#   Windows app build:             downloads/checksums and embeds PDFium + Tectonic (internet required on first build)
#   Core unit tests (host g++):    make test        (pdf_test needs `make pdfium` once: downloads PDFium, ~15 MB)
CROSS   ?=
CXX      = $(CROSS)g++
WINDRES  = $(CROSS)windres
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX
LDFLAGS  += -mwindows -static -static-libgcc -static-libstdc++
LIBS     = -lgdiplus -ld3d11 -ld2d1 -ldwrite -ld3dcompiler -ldxgi -lwindowscodecs -lole32 -loleaut32 -luuid -lshell32 -lcomdlg32 -lwinhttp -lcrypt32 -ldwmapi -ldbghelp -luser32 -lgdi32

CORE = $(wildcard src/core/*.cpp)
WIN  = $(wildcard src/win/*.cpp)
OBJ  = $(patsubst src/%.cpp,build/%.o,$(CORE) $(WIN)) build/res.o
EXE  = VOSStudio.exe

all: $(EXE)

$(EXE): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

build/%.o: src/%.cpp $(wildcard src/core/*.h) $(wildcard src/win/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# PDFium and Tectonic are pinned binary dependencies. Both are verified by SHA-256, packed into the executable as
# compressed RCDATA resources, and unpacked under LocalAppData only when needed. A Windows application build fails
# closed if either download is missing: CI artifacts must never silently omit the PDF or LaTeX engine.
PDFIUM_WIN   = third_party/pdfium/win-x64
PDFIUM_LINUX = third_party/pdfium/linux-x64
PDFIUM_LIC   = $(wildcard $(PDFIUM_WIN)/LICENSE $(PDFIUM_WIN)/licenses/*)
TECTONIC_VERSION = 0.17.0
TECTONIC_EXE = build/tectonic/$(TECTONIC_VERSION)/win-x64/tectonic.exe

pdfium: $(PDFIUM_WIN)/bin/pdfium.dll $(PDFIUM_LINUX)/lib/libpdfium.so
$(PDFIUM_WIN)/bin/pdfium.dll: tools/fetch-pdfium.sh
	bash tools/fetch-pdfium.sh win-x64
$(PDFIUM_WIN)/LICENSE: $(PDFIUM_WIN)/bin/pdfium.dll
	@test -s $@ || { echo "PDFium license bundle is missing: $@" >&2; exit 1; }
$(PDFIUM_LINUX)/lib/libpdfium.so: tools/fetch-pdfium.sh
	bash tools/fetch-pdfium.sh linux-x64

$(TECTONIC_EXE): tools/fetch-tectonic.sh
	bash tools/fetch-tectonic.sh $(TECTONIC_VERSION)

tectonic: $(TECTONIC_EXE)

build/host/packres: tools/packres.cpp build/host/libcore.a
	@mkdir -p build/host
	$(HOSTCXX) -std=c++17 $(HOSTFLAGS) -Isrc/core $< build/host/libcore.a -o $@ -pthread

build/pdfium.bin: build/host/packres $(PDFIUM_WIN)/bin/pdfium.dll
	@mkdir -p build
	./build/host/packres pack PDFM $@ $(PDFIUM_WIN)/bin/pdfium.dll

build/tectonic.bin: build/host/packres $(TECTONIC_EXE)
	@mkdir -p build
	./build/host/packres pack TECT $@ $(TECTONIC_EXE)

build/engine_licenses.bin: build/host/packres $(PDFIUM_WIN)/bin/pdfium.dll $(PDFIUM_WIN)/LICENSE $(PDFIUM_LIC) third_party/pdfium/README.md third_party/tectonic/LICENSE
	@mkdir -p build
	./build/host/packres bundle TEXT $@ third_party/pdfium/README.md $(PDFIUM_LIC) third_party/tectonic/LICENSE

build/res.o: res/vosstudio.rc res/vosstudio.ico res/vosstudio.manifest build/pdfium.bin build/tectonic.bin build/engine_licenses.bin
	@mkdir -p build
	$(WINDRES) -I res -I build res/vosstudio.rc -O coff -o $@

# portable core tests (any C++17 compiler, no Windows needed). The core is compiled once into a host library;
# every tests/*_test.cpp links against it and runs. `make test` is what CI runs.
HOSTCXX      ?= g++
HOSTFLAGS    ?= -O2
TESTS         = $(patsubst tests/%.cpp,%,$(wildcard tests/*_test.cpp))
CORE_HOST_OBJ = $(patsubst src/core/%.cpp,build/host/core/%.o,$(CORE))

build/host/core/%.o: src/core/%.cpp $(wildcard src/core/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCXX) -std=c++17 $(HOSTFLAGS) -Isrc/core -c $< -o $@

build/host/libcore.a: $(CORE_HOST_OBJ)
	ar rcs $@ $^

ifeq ($(OS),Windows_NT)
HOSTLIBS ?=
else
HOSTLIBS ?= -ldl
endif

build/host/%: tests/%.cpp build/host/libcore.a
	@mkdir -p build/host
	$(HOSTCXX) -std=c++17 $(HOSTFLAGS) -Isrc/core $< build/host/libcore.a -o $@ -pthread $(HOSTLIBS)

test: $(addprefix build/host/,$(TESTS))
	@set -e; for t in $(TESTS); do echo "== $$t"; ./build/host/$$t > build/host/$$t.log 2>&1 || { cat build/host/$$t.log; exit 1; }; tail -n 1 build/host/$$t.log; done
	@echo "all $(words $(TESTS)) test programs passed"

# Two Word documents written by the report writer (a typical report and a stress document with every Markdown
# construct the assistant produces). CI validates them with the Open XML SDK; open them in Word after changing docx.cpp.
docx-samples: build/host/docx_test
	./build/host/docx_test build/host/sample.docx build/host/stress.docx

clean:
	rm -rf build $(EXE)

.PHONY: all test clean docx-samples pdfium tectonic
