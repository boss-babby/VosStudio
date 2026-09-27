# VOSStudio Native: Makefile (MSYS2 MinGW-w64 on Windows, or cross-compiling from Linux)
#   MSYS2 (UCRT64/MINGW64 shell):  make -j
#   Linux cross-compile:           make -j CROSS=x86_64-w64-mingw32-
#   Core unit tests (host g++):    make test
CROSS   ?=
CXX      = $(CROSS)g++
WINDRES  = $(CROSS)windres
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX
LDFLAGS  += -mwindows -static -static-libgcc -static-libstdc++
LIBS     = -ld3d11 -ld2d1 -ldwrite -ld3dcompiler -ldxgi -lwindowscodecs -lole32 -loleaut32 -luuid -lshell32 -lcomdlg32 -lwinhttp -lcrypt32 -ldwmapi -ldbghelp -luser32 -lgdi32

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

build/res.o: res/vosstudio.rc res/vosstudio.ico res/vosstudio.manifest
	@mkdir -p build
	$(WINDRES) -I res res/vosstudio.rc -O coff -o $@

# portable core tests (any C++17 compiler, no Windows needed)
test:
	@mkdir -p build
	g++ -std=c++17 -O2 tests/core_test.cpp $(CORE) -o build/core_test -pthread && ./build/core_test
	g++ -std=c++17 -O2 tests/project_test.cpp $(CORE) -o build/project_test -pthread && ./build/project_test
	g++ -std=c++17 -O2 tests/figure_test.cpp $(CORE) -o build/figure_test -pthread && ./build/figure_test
	g++ -std=c++17 -O2 tests/charts_test.cpp $(CORE) -o build/charts_test -pthread && ./build/charts_test
	g++ -std=c++17 -O2 tests/geo_test.cpp $(CORE) -o build/geo_test -pthread && ./build/geo_test
	g++ -std=c++17 -O2 tests/insights_test.cpp $(CORE) -o build/insights_test -pthread && ./build/insights_test
	g++ -std=c++17 -O2 tests/export_test.cpp $(CORE) -o build/export_test -pthread && ./build/export_test
	g++ -std=c++17 -O2 tests/semantic_test.cpp $(CORE) -o build/semantic_test -pthread && ./build/semantic_test
	g++ -std=c++17 -O2 tests/compat_test.cpp $(CORE) -o build/compat_test -pthread && ./build/compat_test
	g++ -std=c++17 -O2 tests/ai_test.cpp $(CORE) -o build/ai_test -pthread && ./build/ai_test

clean:
	rm -rf obj $(EXE)

.PHONY: all test clean
