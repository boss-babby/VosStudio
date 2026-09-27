// VOSStudio Native — Win32 platform services
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "../core/json.h"

namespace vs {
namespace win {

std::wstring widen(const string& s);
string narrow(const std::wstring& s);

struct FileFilter { string name, pattern; };  // {"Bibliographic files", "*.txt;*.csv"}
vector<string> openFileDialog(HWND owner, const string& title, const vector<FileFilter>& filters, bool multi);
string saveFileDialog(HWND owner, const string& title, const vector<FileFilter>& filters, const string& defName, const string& defExt);

bool setClipboardText(HWND owner, const string& utf8);
string getClipboardText(HWND owner);
bool setClipboardImage(HWND owner, int w, int h, const uint8_t* bgra);  // top-down BGRA

string readFileU(const string& utf8Path, bool* ok = nullptr);
bool writeFileU(const string& utf8Path, const string& data);
string reportsDir();  // Documents\\VOSStudio\\Reports (created)
string appDataDir();  // %APPDATA%\VOSStudio (created)
string exeDir();
void openUrl(const string& url);
void revealInExplorer(const string& path);

// Settings persisted as JSON in %APPDATA%\VOSStudio\settings.json
struct Settings {
  Json j = Json::object();
  void load();
  void save() const;
  vector<string> recent() const;
  void addRecent(const string& path);
};

// HTTP GET (WinHTTP). Returns body; status in *status. Runs synchronously — call from a Job.
string httpGet(const string& url, int* status, string* err);
// General HTTP(S) request (WinHTTP) with headers and body. onData receives the body as it arrives (streaming);
// return false from it to cancel. Returns the whole body. Call from a Job.
string httpRequest(const string& method, const string& url, const vector<std::pair<string, string>>& headers, const string& body, int* status, string* err,
                   const std::function<bool(const char*, size_t)>& onData = nullptr, int timeoutMs = 180000);
// Per-user encryption for secrets kept in settings.json (DPAPI). Returns base64; empty on failure.
string protectSecret(const string& plain);
string unprotectSecret(const string& b64);

// Background job with progress + cancel. The UI polls done()/progress each frame.
class Job {
 public:
  std::atomic<double> progress{0};
  std::atomic<bool> cancel{false}, finished{false};
  string title, error;
  std::function<void()> onDone;  // run on the UI thread when finished (by the App)
  HWND notify = nullptr;          // posted WM_NULL when the job finishes (wakes an idle message loop)
  void start(std::function<void(Job&)> fn);
  bool running() const { return started_ && !finished; }
  void join();
  ~Job() { cancel = true; join(); }
 private:
  std::thread th_;
  bool started_ = false;
};

double nowSeconds();

}  // namespace win
}  // namespace vs
