// VOSStudio Native — entry point
#include "app.h"

#include <shellapi.h>

using namespace vs::win;

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::string script;
  std::vector<std::string> open;
  for (int i = 1; i < argc; i++) {
    std::string a = narrow(argv[i]);
    if (a == "--script" && i + 1 < argc) script = narrow(argv[++i]);
    else open.push_back(a);
  }
  LocalFree(argv);
  static App app;
  if (!app.init(hi, show, script, open)) return 1;
  return app.run();
}

// MinGW without -municode calls WinMain; forward it
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR, int show) { return wWinMain(hi, hp, nullptr, show); }
