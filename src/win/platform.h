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
#include <map>
#include <mutex>
#include <thread>

#include "../core/json.h"

namespace vs {
struct Scene;  // figure.h
namespace win {

std::wstring widen(const string& s);
string narrow(const std::wstring& s);

struct FileFilter { string name, pattern; };  // {"Bibliographic files", "*.txt;*.csv"}
vector<string> openFileDialog(HWND owner, const string& title, const vector<FileFilter>& filters, bool multi);
string saveFileDialog(HWND owner, const string& title, const vector<FileFilter>& filters, const string& defName, const string& defExt);

bool setClipboardText(HWND owner, const string& utf8);
string getClipboardText(HWND owner);
bool setClipboardImage(HWND owner, int w, int h, const uint8_t* bgra);  // top-down BGRA
// text + "HTML Format" (+ picture as DIB / "PNG" bytes / Enhanced Metafile); any part may be empty
bool setClipboardRich(HWND owner, const string& utf8, const string& cfHtml, int w = 0, int h = 0, const uint8_t* bgra = nullptr, const string* emf = nullptr, const string* png = nullptr);
string renderSceneEMF(const Scene& sc, double wPt = 0, double hPt = 0);  // emf.cpp: a vector picture of a figure, sized in points (0 = the scene's size)

// Microsoft Word automation (word.cpp) — used to hand the bibliography sources of copied citations to the document
// open in Word, so a paste there gives citations Word owns (References > Manage Sources / Bibliography / Style).
struct WordSource { string tag, xml; };  // <b:Source>…</b:Source> with its b:Tag
struct WordResult { bool reachable = false; int added = 0, present = 0; string document, error; };
bool wordRunning();                                                                   // a Word instance is registered (no dialog, cheap)
WordResult wordAddSources(const vector<WordSource>& sources, bool startWord);        // into the active document's current list (Documents.Add when none)
WordResult wordPasteAtCursor(const vector<WordSource>& sources, bool startWord);     // sources + Selection.Paste of the current clipboard + field update, Word to the front
string getClipboardHtml(HWND owner);  // the raw "HTML Format" buffer ("" = none)
string clipboardFilesDir();           // %TEMP%\VOSStudio\clip (created): pictures and parts referenced by copied HTML
string fileUrl(const string& path);   // C:\a b\c.png -> file:///C:/a%20b/c.png

string readFileU(const string& utf8Path, bool* ok = nullptr);
bool writeFileU(const string& utf8Path, const string& data);
string reportsDir();  // Documents\\VOSStudio\\Reports (created)
string attachmentsDir();  // Documents\\VOSStudio\\Attachments (created): downloaded PDFs of projects that are not saved yet
bool ensureDir(const string& path);  // creates one directory level (true when it exists afterwards)
string exportsDir();  // Documents\\VOSStudio\\Exports (created): files the assistant writes without a full path
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
// A download to a file (WinHTTP; redirects and cookies handled by WinHTTP itself). The first chunk is shown to
// `accept` before anything is written: return false to refuse the body (an HTML page instead of a PDF) — the
// connection is dropped and nothing is stored. `progress(received, total)` (total -1 when unknown) may return false
// to cancel. Stops at maxBytes. Fills the answer's status, content type/length, the final URL and the headers named
// in `wantHeaders` (case-insensitive; e.g. X-RateLimit-Remaining-USD). Call from a Job.
struct HttpDownload {
  int status = 0;
  string err, contentType, finalUrl;
  long long length = -1, received = 0;
  bool accepted = false, complete = false;  // accept() said yes; the body was read to its end (or to Content-Length)
  string first;                             // the first bytes (at most 256) of a refused body, to tell the user what came instead
  std::map<string, string> headers;         // lower-case names
};
bool httpDownload(const string& url, const vector<std::pair<string, string>>& headers, const string& path, HttpDownload& out,
                  const std::function<bool(const char*, size_t)>& accept, const std::function<bool(long long, long long)>& progress,
                  long long maxBytes, int timeoutMs, const vector<string>& wantHeaders);
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
