@echo off
rem VOSStudio Native - build with MSYS2 MinGW-w64 (install: pacman -S mingw-w64-ucrt-x86_64-gcc make curl unzip tar)
rem Run from an MSYS2 UCRT64 shell, or make sure g++/windres/make are on PATH.
where mingw32-make >nul 2>nul && (set MAKE=mingw32-make) || (set MAKE=make)
%MAKE% -j%NUMBER_OF_PROCESSORS% %*
if errorlevel 1 (
  echo.
  echo Build failed. Check that MSYS2 MinGW-w64 g++, make, curl, unzip and tar are installed.
  exit /b 1
)
echo.
echo Built VOSStudio.exe
