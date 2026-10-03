// VOSStudio Native: Live AI — a real-time voice / text session with the Gemini Live API, shown in a light pop-up on the
// right. The model hears the microphone (16 kHz PCM over the WebSocket), answers with speech (24 kHz PCM played through
// WASAPI) or text, and works the application through the agent's tools (agentExecute), which run one at a time on the
// UI thread. Protocol details: src/core/live.h. Extended Thinking models keep an interaction "IN_PROGRESS" while they
// reason and wait for tools, and speak short fillers in the meantime.
#include "app.h"
#include "version.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <thread>

namespace vs {
namespace win {

string nowStamp();  // assistant.cpp

namespace {

// ================================================================= WebSocket (WinHTTP)
struct WsEvent {
  enum Kind { Open, Message, Closed, Error } kind = Message;
  string text;  // message / close reason / error
  int code = 0;  // close status
};

// One connection: a receiver thread (also performs the upgrade) and a sender thread with a queue. Events are polled
// by the UI thread; the window is woken with WM_NULL. close() may block for a moment, so the App runs it off-thread.
class WebSocket {
 public:
  explicit WebSocket(HWND wake) : wake_(wake) {}
  ~WebSocket() { close(); }
  void connect(const string& url, string first) {
    th_ = std::thread([this, url, f = std::move(first)]() mutable { run(url, std::move(f)); });
  }
  void send(string msg) {
    { std::lock_guard<std::mutex> lk(mu_); if (closing_) return; out_.push_back(std::move(msg)); }
    cv_.notify_all();
  }
  bool poll(vector<WsEvent>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (events_.empty()) return false;
    out.insert(out.end(), std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
    events_.clear();
    return true;
  }
  bool connected() const { return alive_; }
  void close() {
    bool first = !closing_.exchange(true);
    cv_.notify_all();
    if (!first) { joinAll(); return; }
    HINTERNET ws = nullptr, req = nullptr;
    { std::lock_guard<std::mutex> lk(mu_); ws = ws_; req = req_; req_ = nullptr; }
    if (req) WinHttpCloseHandle(req);  // aborts a pending upgrade
    if (ws && !done_) {
      WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);  // close frame; the receiver ends on the reply
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(1500), [&] { return done_.load(); });
    }
    { std::lock_guard<std::mutex> lk(mu_); ws = ws_; ws_ = nullptr; }
    if (ws) WinHttpCloseHandle(ws);  // cancels a receive that is still pending
    joinAll();
    if (con_) { WinHttpCloseHandle(con_); con_ = nullptr; }
    if (ses_) { WinHttpCloseHandle(ses_); ses_ = nullptr; }
  }

 private:
  void joinAll() {
    if (th_.joinable() && th_.get_id() != std::this_thread::get_id()) th_.join();
    if (sendTh_.joinable() && sendTh_.get_id() != std::this_thread::get_id()) sendTh_.join();
  }
  void push(WsEvent e) {
    { std::lock_guard<std::mutex> lk(mu_); events_.push_back(std::move(e)); }
    if (wake_) PostMessageW(wake_, WM_NULL, 0, 0);
  }
  static string winErr(DWORD e) {
    switch (e) {
      case ERROR_WINHTTP_TIMEOUT: return "the connection timed out";
      case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the server name could not be resolved";
      case ERROR_WINHTTP_CANNOT_CONNECT: return "the connection was refused";
      case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection failed";
      case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was lost";
      case ERROR_WINHTTP_OPERATION_CANCELLED: return "the connection was closed";
      default: return "network error " + std::to_string(e);
    }
  }
  void run(string url, string first) {
    bool secure = true;  // WinHttpCrackUrl understands only http(s); the upgrade option turns the request into a WebSocket
    if (url.rfind("wss://", 0) == 0) url = "https://" + url.substr(6);
    else if (url.rfind("ws://", 0) == 0) { url = "http://" + url.substr(5); secure = false; }
    std::wstring wu = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    std::vector<wchar_t> pathB(wu.size() + 16, 0), extraB(wu.size() + 16, 0);
    wchar_t host[256] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pathB.data(); uc.dwUrlPathLength = DWORD(pathB.size() - 1);
    uc.lpszExtraInfo = extraB.data(); uc.dwExtraInfoLength = DWORD(extraB.size() - 1);
    if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) { finish(WsEvent{WsEvent::Error, "Invalid address (error " + std::to_string(GetLastError()) + ")", 0}); return; }
    ses_ = WinHttpOpen(L"VOSStudio/" VOS_VERSION_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses_) { finish(WsEvent{WsEvent::Error, "WinHTTP unavailable", 0}); return; }
    WinHttpSetTimeouts(ses_, 10000, 15000, 30000, 24 * 3600 * 1000);  // a live session may stay silent for a long time
    con_ = WinHttpConnect(ses_, host, uc.nPort, 0);
    std::wstring full = std::wstring(pathB.data()) + extraB.data();
    HINTERNET req = con_ ? WinHttpOpenRequest(con_, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (!req) { finish(WsEvent{WsEvent::Error, "The connection could not be opened", 0}); return; }
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closing_) { WinHttpCloseHandle(req); req = nullptr; } else req_ = req;
    }
    if (!req) { finish(WsEvent{WsEvent::Closed, "", 1000}); return; }
    WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    DWORD rt = 24 * 3600 * 1000;  // the socket stays open while nobody talks
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &rt, sizeof rt);
    bool sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr);
    if (!sent) {
      DWORD e = GetLastError();
      dropReq();
      finish(closing_ ? WsEvent{WsEvent::Closed, "", 1000} : WsEvent{WsEvent::Error, "Could not connect: " + winErr(e), 0});
      return;
    }
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    if (code != 101) {
      string body;
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 65536) {
        string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &got)) break;
        body.append(chunk.data(), got);
      }
      dropReq();
      finish(WsEvent{WsEvent::Error, live::describeUpgradeFailure(int(code), body), int(code)});
      return;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    dropReq();
    if (!ws) { finish(WsEvent{WsEvent::Error, "The WebSocket upgrade failed", 0}); return; }
    DWORD ka = 20000;
    WinHttpSetOption(ws, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &ka, sizeof ka);
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (closing_) { WinHttpCloseHandle(ws); ws = nullptr; } else ws_ = ws;
    }
    if (!ws) { finish(WsEvent{WsEvent::Closed, "", 1000}); return; }
    DWORD r = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(first.data()), DWORD(first.size()));
    if (r != NO_ERROR) { finish(WsEvent{WsEvent::Error, "The session setup could not be sent: " + winErr(r), 0}); return; }
    alive_ = true;
    sendTh_ = std::thread([this, ws]() { sender(ws); });
    push(WsEvent{WsEvent::Open, "", 0});
    vector<char> buf(64 * 1024);
    string msg;
    for (;;) {
      DWORD read = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
      DWORD rc = WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), &read, &type);
      if (rc != NO_ERROR) {
        finish(closing_ ? WsEvent{WsEvent::Closed, "", 1000} : WsEvent{WsEvent::Error, "The connection was lost (" + winErr(rc) + ")", 0});
        return;
      }
      if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        USHORT st = 0;
        char reason[128] = {0};
        DWORD rl = 0;
        WinHttpWebSocketQueryCloseStatus(ws, &st, reason, sizeof reason - 1, &rl);
        finish(WsEvent{WsEvent::Closed, string(reason, std::min<size_t>(rl, sizeof reason - 1)), int(st)});
        return;
      }
      msg.append(buf.data(), read);
      if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
        push(WsEvent{WsEvent::Message, std::move(msg), 0});
        msg.clear();
      }
    }
  }
  void dropReq() {
    HINTERNET req = nullptr;
    { std::lock_guard<std::mutex> lk(mu_); req = req_; req_ = nullptr; }
    if (req) WinHttpCloseHandle(req);
  }
  void finish(WsEvent e) {
    alive_ = false;
    push(std::move(e));
    { std::lock_guard<std::mutex> lk(mu_); done_ = true; }
    cv_.notify_all();
  }
  void sender(HINTERNET ws) {
    for (;;) {
      string m;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return closing_ || done_ || !out_.empty(); });
        if ((closing_ || done_) && out_.empty()) return;
        if (out_.empty()) continue;
        m = std::move(out_.front());
        out_.pop_front();
      }
      DWORD r = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(m.data()), DWORD(m.size()));
      if (r != NO_ERROR) {
        if (!closing_ && !done_) push(WsEvent{WsEvent::Error, "A message could not be sent (" + winErr(r) + ")", 0});
        return;
      }
    }
  }
  HWND wake_;
  HINTERNET ses_ = nullptr, con_ = nullptr, req_ = nullptr, ws_ = nullptr;
  std::thread th_, sendTh_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<string> out_;
  vector<WsEvent> events_;
  std::atomic<bool> closing_{false}, alive_{false}, done_{false};
};

// ================================================================= audio (WASAPI, shared mode)
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct DevFormat {
  int rate = 0, channels = 0, bits = 0;
  bool isFloat = false;
  bool ok() const { return rate > 0 && channels > 0 && (isFloat ? bits == 32 : (bits == 16 || bits == 32 || bits == 24)); }
};

DevFormat readFormat(const WAVEFORMATEX* f) {
  DevFormat d;
  if (!f) return d;
  d.rate = int(f->nSamplesPerSec);
  d.channels = f->nChannels;
  d.bits = f->wBitsPerSample;
  if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) d.isFloat = true;
  else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const WAVEFORMATEXTENSIBLE* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    d.isFloat = IsEqualGUID(x->SubFormat, kSubtypeFloat);
    if (!d.isFloat && !IsEqualGUID(x->SubFormat, kSubtypePcm)) d.rate = 0;
  } else if (f->wFormatTag != WAVE_FORMAT_PCM) d.rate = 0;
  return d;
}

float sampleAt(const BYTE* frame, const DevFormat& f, int ch) {
  const BYTE* p = frame + ch * (f.bits / 8);
  if (f.isFloat) { float v; memcpy(&v, p, 4); return v; }
  if (f.bits == 16) { int16_t v; memcpy(&v, p, 2); return v / 32768.f; }
  if (f.bits == 32) { int32_t v; memcpy(&v, p, 4); return float(v / 2147483648.0); }
  int32_t v = (int32_t(p[2]) << 24 | int32_t(p[1]) << 16 | int32_t(p[0]) << 8) >> 8;  // 24-bit
  return v / 8388608.f;
}

void writeSample(BYTE* frame, const DevFormat& f, int ch, float v) {
  BYTE* p = frame + ch * (f.bits / 8);
  v = std::max(-1.f, std::min(1.f, v));
  if (f.isFloat) { memcpy(p, &v, 4); return; }
  if (f.bits == 16) { int16_t s = int16_t(std::lround(v * 32767)); memcpy(p, &s, 2); return; }
  if (f.bits == 32) { int32_t s = int32_t(std::llround(v * 2147483647.0)); memcpy(p, &s, 4); return; }
  int32_t s = int32_t(std::lround(v * 8388607));
  p[0] = BYTE(s & 0xff); p[1] = BYTE((s >> 8) & 0xff); p[2] = BYTE((s >> 16) & 0xff);
}

// Opens the default device of a data flow at the wanted mono 16-bit rate; Windows converts when it can
// (AUTOCONVERTPCM), otherwise the device's own mix format is used and the caller resamples.
bool openClient(EDataFlow flow, int wantRate, Com<IAudioClient>& client, DevFormat& used, string* err, bool voice = false) {
  Com<IMMDeviceEnumerator> en;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(en.put()));
  if (FAILED(hr)) { if (err) *err = "the audio system is unavailable"; return false; }
  Com<IMMDevice> dev;
  hr = en->GetDefaultAudioEndpoint(flow, flow == eCapture ? eCommunications : eConsole, dev.put());
  if (FAILED(hr)) { if (err) *err = flow == eCapture ? "no microphone was found" : "no speaker was found"; return false; }
  const REFERENCE_TIME dur = 1000000;  // 100 ms
  HRESULT lastHr = S_OK;
  auto tryInit = [&](const WAVEFORMATEX* f, DWORD flags, Com<IAudioClient>& c, bool voiceMode) {
    c.reset();
    lastHr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(c.put()));
    if (FAILED(lastHr)) return false;
    if (voiceMode) {  // "Communications" stream: the device's own voice processing (noise suppression, echo cancellation, gain) where the driver offers it
      Com<IAudioClient2> c2;
      if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(c2.put())))) {
        AudioClientProperties props{};
        props.cbSize = sizeof(props);
        props.bIsOffload = FALSE;
        props.eCategory = AudioCategory_Communications;
        props.Options = AUDCLNT_STREAMOPTIONS_NONE;
        c2->SetClientProperties(&props);  // best effort: a refusal only means the plain stream
      }
    }
    lastHr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, f, nullptr);
    return SUCCEEDED(lastHr);
  };
  auto denied = [&]() { return lastHr == E_ACCESSDENIED; };
  WAVEFORMATEX want{};
  want.wFormatTag = WAVE_FORMAT_PCM;
  want.nChannels = 1;
  want.nSamplesPerSec = DWORD(wantRate);
  want.wBitsPerSample = 16;
  want.nBlockAlign = 2;
  want.nAvgBytesPerSec = DWORD(wantRate * 2);
  const DWORD wantFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (tryInit(&want, wantFlags, client, voice) || (voice && !denied() && tryInit(&want, wantFlags, client, false))) {
    used = readFormat(&want);
    return true;
  }
  if (denied()) { if (err) *err = "Windows blocks microphone access for desktop apps (Settings \xE2\x86\x92 Privacy \xE2\x86\x92 Microphone)"; return false; }
  Com<IAudioClient> probe;
  if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(probe.put())))) { if (err) *err = "the audio device could not be opened"; return false; }
  WAVEFORMATEX* mix = nullptr;
  if (FAILED(probe->GetMixFormat(&mix)) || !mix) { if (err) *err = "the audio device format is unknown"; return false; }
  DevFormat mf = readFormat(mix);
  bool ok = mf.ok() && (tryInit(mix, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, client, voice) || (voice && tryInit(mix, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, client, false)));
  CoTaskMemFree(mix);
  if (!ok) { if (err) *err = denied() ? "Windows blocks microphone access for desktop apps (Settings \xE2\x86\x92 Privacy \xE2\x86\x92 Microphone)" : "the audio device format is not supported"; return false; }
  used = mf;
  return true;
}

// Microphone → 16 kHz PCM chunks (delivered on the capture thread); model speech → speaker at 24 kHz.
class Audio {
 public:
  ~Audio() { stop(); }
  void setVoiceProcessing(bool v) { voice_ = v; }  // before startCapture
  // onChunk(pcm, n, muted): 40 ms of 16 kHz mono; muted = the assistant is speaking and barge-in is off (the audio is
  // still the real microphone, so that the detector can notice a deliberate interruption; the caller decides what to send)
  bool startCapture(std::function<void(const int16_t*, size_t, bool)> onChunk, string* err) {
    if (capTh_.joinable()) return true;
    onChunk_ = std::move(onChunk);
    capStop_ = false;
    capOk_ = -1;
    capErr_.clear();
    capTh_ = std::thread([this] { captureLoop(); });
    for (int i = 0; i < 200 && capOk_ < 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));  // the device opens within a moment
    if (capOk_ != 1) {
      capStop_ = true;
      if (capTh_.joinable()) capTh_.join();
      if (err) *err = capErr_.empty() ? string("the microphone could not be opened") : capErr_;
      capturing_ = false;
      return false;
    }
    return true;
  }
  void stopCapture() {
    capStop_ = true;
    if (capTh_.joinable()) capTh_.join();
    capturing_ = false;
    inLevel_ = 0;
  }
  bool capturing() const { return capturing_; }
  bool startPlayback(string* err) {
    if (playTh_.joinable()) return true;
    playStop_ = false;
    playOk_ = -1;
    playErr_.clear();
    playTh_ = std::thread([this] { renderLoop(); });
    for (int i = 0; i < 200 && playOk_ < 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (playOk_ != 1) {
      playStop_ = true;
      if (playTh_.joinable()) playTh_.join();
      if (err) *err = playErr_.empty() ? string("the speaker could not be opened") : playErr_;
      return false;
    }
    return true;
  }
  void stopPlayback() {
    playStop_ = true;
    if (playTh_.joinable()) playTh_.join();
    std::lock_guard<std::mutex> lk(qmu_);
    q_.clear();
    qHead_ = 0;
    queued_ = 0;
  }
  void play(const string& pcm) {  // 16-bit little-endian mono 24 kHz
    std::lock_guard<std::mutex> lk(qmu_);
    size_t n = pcm.size() / 2;
    size_t at = q_.size();
    if (at == qHead_) firstQueuedT_ = nowSeconds();  // the queue was empty: the reserve clock starts now
    q_.resize(at + n);
    for (size_t i = 0; i < n; i++) q_[at + i] = int16_t(uint16_t(uint8_t(pcm[2 * i])) | (uint16_t(uint8_t(pcm[2 * i + 1])) << 8));
    queued_ = q_.size() - qHead_;
    lastPlayT_ = nowSeconds() + double(queued_) / live::kOutputRate;
  }
  void flush() {  // barge-in: drop everything that has not been played yet
    { std::lock_guard<std::mutex> lk(qmu_); q_.clear(); qHead_ = 0; queued_ = 0; qPos_ = 0; primed_ = false; }
    flushReq_ = true;
    lastPlayT_ = 0;
  }
  bool playing() const { return queued_ > 0 || nowSeconds() < lastPlayT_ + 0.25; }
  double queuedSeconds() const { return double(queued_.load()) / live::kOutputRate; }
  float inLevel() const { return inLevel_; }
  float outLevel() const { return outLevel_; }
  void setBargeIn(bool b) { bargeIn_ = b; }
  bool micPaused() const { return capturing_ && !bargeIn_ && playing(); }
  void stop() { stopCapture(); stopPlayback(); }

 private:
  void captureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com<IAudioClient> ac;
    DevFormat f;
    string err;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Com<IAudioCaptureClient> cc;
    bool ok = openClient(eCapture, live::kInputRate, ac, f, &err, voice_) && SUCCEEDED(ac->SetEventHandle(ev)) &&
              SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cc.put()))) && SUCCEEDED(ac->Start());
    if (!ok) { capErr_ = err.empty() ? "the microphone could not be started" : err; capOk_ = 0; CloseHandle(ev); CoUninitialize(); return; }
    capturing_ = true;
    capOk_ = 1;
    const size_t chunkN = size_t(live::kInputRate / 25);  // 40 ms
    vector<int16_t> chunk;
    vector<float> in;
    double pos = 0;
    const double ratio = double(f.rate) / live::kInputRate;
    const int frameBytes = f.channels * f.bits / 8;
    while (!capStop_) {
      WaitForSingleObject(ev, 100);
      UINT32 pk = 0;
      while (!capStop_ && SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (f.rate == live::kInputRate && f.channels == 1 && !f.isFloat && f.bits == 16) {
          size_t at = chunk.size();
          chunk.resize(at + frames);
          if (silent) std::fill(chunk.begin() + long(at), chunk.end(), int16_t(0));
          else memcpy(&chunk[at], data, size_t(frames) * 2);
        } else {
          for (UINT32 i = 0; i < frames; i++) {
            float v = 0;
            if (!silent) { for (int ch = 0; ch < f.channels; ch++) v += sampleAt(data + i * frameBytes, f, ch); v /= float(f.channels); }
            in.push_back(v);
          }
          while (pos + 1 < double(in.size())) {  // linear resampling to 16 kHz
            size_t k = size_t(pos);
            float fr = float(pos - double(k));
            float v = in[k] * (1 - fr) + in[k + 1] * fr;
            chunk.push_back(int16_t(std::lround(std::max(-1.f, std::min(1.f, v)) * 32767)));
            pos += ratio;
          }
          size_t drop = size_t(pos);
          if (drop > 0 && drop <= in.size()) { in.erase(in.begin(), in.begin() + long(drop)); pos -= double(drop); }
        }
        cc->ReleaseBuffer(frames);
        while (chunk.size() >= chunkN) {
          bool mute = micPaused();
          inLevel_ = mute ? 0.f : live::rmsLevel(chunk.data(), chunkN);
          if (onChunk_) onChunk_(chunk.data(), chunkN, mute);
          chunk.erase(chunk.begin(), chunk.begin() + long(chunkN));
        }
      }
    }
    ac->Stop();
    cc.reset();
    ac.reset();
    CloseHandle(ev);
    CoUninitialize();
  }
  void renderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com<IAudioClient> ac;
    DevFormat f;
    string err;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Com<IAudioRenderClient> rc;
    UINT32 bufFrames = 0;
    bool ok = openClient(eRender, live::kOutputRate, ac, f, &err) && SUCCEEDED(ac->SetEventHandle(ev)) && SUCCEEDED(ac->GetBufferSize(&bufFrames)) &&
              SUCCEEDED(ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(rc.put())));
    if (ok) {
      BYTE* p = nullptr;
      if (SUCCEEDED(rc->GetBuffer(bufFrames, &p))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
      ok = SUCCEEDED(ac->Start());
    }
    if (!ok) { playErr_ = err.empty() ? "the speaker could not be started" : err; playOk_ = 0; CloseHandle(ev); CoUninitialize(); return; }
    playOk_ = 1;
    const int frameBytes = f.channels * f.bits / 8;
    const double step = double(live::kOutputRate) / f.rate;  // queue samples per device frame
    while (!playStop_) {
      WaitForSingleObject(ev, 100);
      if (flushReq_.exchange(false)) { ac->Stop(); ac->Reset(); ac->Start(); }
      UINT32 pad = 0;
      if (FAILED(ac->GetCurrentPadding(&pad))) continue;
      UINT32 avail = bufFrames > pad ? bufFrames - pad : 0;
      if (!avail) continue;
      BYTE* p = nullptr;
      if (FAILED(rc->GetBuffer(avail, &p))) continue;
      double acc = 0;
      {
        std::lock_guard<std::mutex> lk(qmu_);
        // a small reserve before playing: the server sends speech in bursts, and playing each burst as it lands leaves
        // audible gaps in the middle of sentences; 250 ms (or 400 ms of waiting, for very short replies) smooths them out
        size_t have = q_.size() - qHead_;
        if (!primed_ && have > 0 && (have >= size_t(live::kOutputRate / 4) || nowSeconds() - firstQueuedT_ > 0.4)) primed_ = true;
        bool play = primed_ && have > 0;
        if (primed_ && have == 0) primed_ = false;  // ran dry: gather a reserve again before going on
        for (UINT32 i = 0; i < avail; i++) {
          float v = 0;
          size_t k = qHead_ + size_t(qPos_);
          if (play && k < q_.size()) {
            float a = q_[k] / 32768.f, b = k + 1 < q_.size() ? q_[k + 1] / 32768.f : a;
            float fr = float(qPos_ - std::floor(qPos_));
            v = a * (1 - fr) + b * fr;
            qPos_ += step;
          }
          acc += double(v) * v;
          for (int ch = 0; ch < f.channels; ch++) writeSample(p + i * frameBytes, f, ch, v);
        }
        size_t adv = size_t(qPos_);
        qHead_ += adv;
        qPos_ -= double(adv);
        if (qHead_ >= q_.size()) { q_.clear(); qHead_ = 0; qPos_ = 0; }
        else if (qHead_ > (1u << 20)) { q_.erase(q_.begin(), q_.begin() + long(qHead_)); qHead_ = 0; }
        queued_ = q_.size() - qHead_;
      }
      rc->ReleaseBuffer(avail, 0);
      double rms = std::sqrt(acc / std::max<UINT32>(1, avail));
      outLevel_ = float(std::max(0.0, std::min(1.0, (20 * std::log10(std::max(rms, 1e-6)) + 50) / 50)));
    }
    ac->Stop();
    rc.reset();
    ac.reset();
    CloseHandle(ev);
    CoUninitialize();
  }
  std::function<void(const int16_t*, size_t, bool)> onChunk_;
  std::thread capTh_, playTh_;
  std::atomic<bool> capStop_{false}, playStop_{false}, capturing_{false}, flushReq_{false}, bargeIn_{false}, voice_{true};
  bool primed_ = false;        // playback thread, under qmu_
  double firstQueuedT_ = 0;    // under qmu_
  std::atomic<int> capOk_{-1}, playOk_{-1};
  std::atomic<float> inLevel_{0}, outLevel_{0};
  std::atomic<size_t> queued_{0};
  std::atomic<double> lastPlayT_{0};
  string capErr_, playErr_;
  std::mutex qmu_;
  vector<int16_t> q_;
  size_t qHead_ = 0;
  double qPos_ = 0;
};

string firstLineOf(const string& s, size_t n = 90) {
  string t = trim(s);
  size_t p = t.find('\n');
  if (p != string::npos) t = t.substr(0, p);
  while (!t.empty() && (t[0] == '#' || t[0] == '-' || t[0] == ' ')) t.erase(0, 1);
  return truncate(t, n);
}

}  // namespace

namespace {
// The microphone gate: runs the speech detector on the capture thread, keeps a pre-roll of the last chunks so that the
// beginning of an utterance is not lost, and hands Start / End events to the UI thread.
struct MicGate {
  explicit MicGate(HWND wake) : wake_(wake) {}
  live::SpeechDetector det;
  live::Agc agc;                     // capture thread only
  std::deque<vector<int16_t>> pre;   // capture thread only: the last ~600 ms
  std::atomic<bool> active{false};
  std::atomic<float> level{-100.f};
  live::SpeechDetector::Event feed(const int16_t* pcm, size_t n) {
    pre.emplace_back(pcm, pcm + n);
    while (pre.size() > 15) pre.pop_front();
    live::SpeechDetector::Event ev = det.feed(pcm, n);
    active = det.active();
    level = det.levelDb();
    if (ev != live::SpeechDetector::None) {
      { std::lock_guard<std::mutex> lk(m_); events_.push_back(int(ev)); }
      if (wake_) PostMessageW(wake_, WM_NULL, 0, 0);
    }
    return ev;
  }
  static constexpr int BargeIn = 3;  // event: the user spoke up while the assistant was talking (stop the playback)
  void bargeIn() {
    { std::lock_guard<std::mutex> lk(m_); events_.push_back(BargeIn); }
    if (wake_) PostMessageW(wake_, WM_NULL, 0, 0);
  }
  vector<int> take() { std::lock_guard<std::mutex> lk(m_); vector<int> v(events_.begin(), events_.end()); events_.clear(); return v; }
  // The current utterance is kept (up to 90 s) so that it can be sent again when the server lost it — measured with
  // tests/live_probe.py: now and then an utterance gets neither an ACTIVITY_END echo nor a transcript, and the model
  // later reports "<no speech>" for that turn.
  void beginUtterance() { std::lock_guard<std::mutex> lk(um_); utter_.clear(); }
  void record(const int16_t* pcm, size_t n) {
    std::lock_guard<std::mutex> lk(um_);
    if (utter_.size() + n <= size_t(live::kInputRate) * 90) utter_.insert(utter_.end(), pcm, pcm + n);
  }
  vector<int16_t> utterance() { std::lock_guard<std::mutex> lk(um_); return utter_; }
 private:
  HWND wake_;
  std::mutex m_, um_;
  std::deque<int> events_;
  vector<int16_t> utter_;
};
}  // namespace

struct App::LiveSession {
  std::shared_ptr<WebSocket> ws;
  std::shared_ptr<Audio> audio;
  std::shared_ptr<MicGate> gate;   // while the microphone is on
};

// ================================================================= settings and prompt
string App::liveKey() const {
  string k = unprotectSecret(settings.j["aiKey1"].str());  // provider 1 = Google Gemini
  if (k.empty()) k = settings.j["aiKeyPlain1"].str();
  return trim(k);
}

string App::liveModel() const {
  string m = trim(settings.j["liveModel"].str());
  return m.empty() ? string(live::kDefaultModel) : m;
}

string App::liveSystemPrompt() const {
  string s =
      "You are VOSStudio Live, the real-time assistant built into VOSStudio, a desktop application for bibliometric science mapping (like VOSviewer). "
      "You talk with a researcher while they work and you operate the whole application for them through the function tools: load samples, files and "
      "projects, read the records and the map, choose the analysis and build maps, re-cluster, name clusters, clean and merge terms, switch views, pages and tabs, "
      "change the look, focus items and clusters, open document previews and charts, compare periods, use the geography view, search OpenAlex, read papers, "
      "write a report with charts and sections, export figures, views, screenshots and the report as files, and save the project. Typed tools exist for the "
      "common tasks; run_commands reaches everything else in the interface (list_commands gives the exact syntax) and get_ui_state tells you what is on screen. "
      "Google Search, when available, is for questions about the wider literature and the world; the tools are the only source of facts about the user's data.\n\n"
      "Rules:\n"
      "- Get facts from the tools. Never invent numbers, items, clusters or documents.\n"
      "- Act rather than ask: when the user asks for something, do it with the tools right away (look at get_ui_state or the overview first when you need "
      "context), then report briefly. Chain as many tools as the task needs; do not stop half-way to ask whether to continue. Ask only when a choice would "
      "destroy work (replacing loaded records or a project) and the user did not make it clear.\n"
      "- Speak like a knowledgeable colleague: short, specific answers (one to three sentences) unless the user asks for detail. Summarise tool results; "
      "never read tables or lists out loud. No technical jargon about the application's internals.\n"
      "- build_map, load_data, search_openalex, check_stability and command sequences with a build take several seconds; say what you are doing while they run. "
      "When the user has switched the shield in the Live panel to ask-first, tools that change the project wait for their approval; if such a tool takes long, "
      "mention that they may need to allow it.\n"
      "- Files: when the user names a file without a folder, the tools write it to Documents\\VOSStudio\\Exports and tell you the full path; say where the file is.\n"
      "- If a tool fails, adapt the arguments or choose another tool. If the user declines a change, do not repeat it.\n"
      "- Seeing: you receive no video. When the user asks you to look at the screen, the map, a chart or a layout, call look_at_screen: one still picture "
      "of the canvas (or the window) arrives as an image; call it again when the view changes. The user can also send you a picture with the camera button. "
      "Combine what you see with what the tools know (names, numbers).\n"
      "- Reports: write_report drafts a long report (several sections of several hundred words each, grounded in the records) with the application's text model; "
      "get_report with full=true shows the exact text and edit_section changes one section. Use them for anything longer than a few lines; add_section is for a short note.\n"
      "- The loaded records are the user's work. Never replace or discard them on your own: to add data use append; for a new topic or a new project call "
      "new_project first (it refuses while unsaved work exists - then ask whether to save or discard); pass replace or discard_unsaved only when the user clearly "
      "asked to start over. The tools refuse destructive calls and tell you the alternatives.\n"
      "- Say only what the tool results confirm. Each result describes what really happened on screen (opened, shown, loaded, refused, not found); when something "
      "was refused or not found, say that instead of claiming it was done.\n"
      "- Showing things: show_papers puts the whole list of papers on screen (sortable, filterable); show_chart puts a figure into the main area, large. To explain "
      "the data with figures, call show_chart for each figure right before you talk about it and describe what the result says the figure shows; several in a "
      "row make a guided walk-through. show_chart none brings the map back. show_compare puts two periods, two imported files or two thresholds side by side "
      "(or as a difference map); focus_cluster with linked selection makes every page, the papers table and the geo view follow that cluster, focus_cluster 0 clears it.\n"
      "- Answer in the language the user uses.\n\n"
      "Current state of the application:\n";
  if (P && (hasCorpus() || hasMap())) {
    ai::ContextOpts o;
    o.corpus = hasCorpus();
    o.map = hasMap();
    o.topDocs = false;
    o.trends = false;
    o.maxItemsPerCluster = 6;
    string ctx = ai::buildContext(*P, o);
    const size_t cap = 6000;  // a light first turn; the tools give the details on demand
    if (ctx.size() > cap) {
      size_t cut = ctx.rfind('\n', cap);
      ctx = ctx.substr(0, cut == string::npos ? cap : cut) + "\n(\xE2\x80\xA6 shortened; use the tools for the rest)\n";
    }
    s += ctx;
  } else s += "No records are loaded and there is no map yet. The user can load files or you can search OpenAlex.\n";
  if (!wdoc.empty()) s += "\nThe document in the writer has " + writerSummary() + " (see get_report).\n";
  return s;
}

// ================================================================= session control
void App::liveOpen(bool talk) {
  if (!live.posLoaded) {  // where the panel and the orb were dragged last time (fractions of the window)
    live.posLoaded = true;
    auto pos = [&](const char* key, float& x, float& y) {
      const Json& a = settings.j[key];
      if (a.t != Json::Arr || a.a.size() < 2 || g.W <= 0 || g.H <= 0) return;
      x = float(clampv(a[0].num(-1), 0.0, 1.0) * g.W);
      y = float(clampv(a[1].num(-1), 0.0, 1.0) * g.H);
      if (a[0].num(-1) < 0 || a[1].num(-1) < 0) x = y = -1;
    };
    pos("livePanelPos", live.panelX, live.panelY);
    pos("liveOrbPos", live.orbX, live.orbY);
  }
  live.open = true;
  live.stick = true;
  if (talk) live.talk = true;
  if (!live.s || live.phase == 0) liveConnect(talk || live.talk);
  else if (talk && !live.audioMode) liveConnect(true);  // switch to spoken replies
  else if (talk) liveStartMic();
  if (!talk) live.focusWanted = true;
}

void App::liveClose() {
  liveSaveToChat();
  liveDisconnect(false);
  live.open = false;
  live.talk = false;
  live.queue.clear();
  live.pendingText.clear();
  live.pendingResults.clear();
  live.log.clear();
  live.error.clear();
  live.changes = 0;
  live.reconnects = 0;
  live.tokens = -1;
  live.focusWanted = false;
  live.savedEntries = 0;
  live.turnCalls = live.nudges = 0;
  if (live.inputId && ui.focus == live.inputId) ui.focus = 0;
}

string App::liveCarryContext() const {
  string out;
  size_t n = live.log.entries.size(), start = n > 24 ? n - 24 : 0;
  for (size_t i = start; i < n; i++) {
    const live::Entry& e = live.log.entries[i];
    if (e.kind == live::Entry::User) out += "User: " + truncate(e.text, 500) + "\n";
    else if (e.kind == live::Entry::Model) out += "You: " + truncate(e.text, 500) + "\n";
    else if (e.kind == live::Entry::Tool) out += "(tool " + truncate(e.text, 80) + (e.status == 1 ? " done" : e.status == 2 ? " failed" : e.status == 3 ? " declined" : "") + ")\n";
  }
  if (out.size() > 4000) out = "\xE2\x80\xA6" + out.substr(out.size() - 4000);
  return out;
}

void App::liveSaveToChat() {
  // The voice conversation is kept with the Assistant's chat (and so with the project): user and assistant entries as
  // turns, tool calls as a short line under the assistant's reply.
  if (!P) return;
  const auto& E = live.log.entries;
  if (live.savedEntries >= E.size()) return;
  bool first = true;
  string tools;
  for (size_t i = live.savedEntries; i < E.size(); i++) {
    const live::Entry& e = E[i];
    if (e.kind == live::Entry::Tool) { tools += (tools.empty() ? "" : " \xC2\xB7 ") + e.text + (e.status == 2 ? " (failed)" : e.status == 3 ? " (declined)" : ""); continue; }
    if (e.kind != live::Entry::User && e.kind != live::Entry::Model) continue;
    if (trim(e.text).empty()) continue;
    if (e.kind == live::Entry::User) {
      if (!tools.empty()) { aiTurns.push_back({"assistant", "*Did: " + tools + "*", "live", nowStamp()}); tools.clear(); }
      aiTurns.push_back({"user", (first ? string("\xF0\x9F\x8E\x99 **Live session**  \n") : string()) + e.text, "live", nowStamp()});
    } else {
      string txt = e.text;
      if (!tools.empty()) { txt += "\n\n*Did: " + tools + "*"; tools.clear(); }
      if (first) txt = "\xF0\x9F\x8E\x99 **Live session**  \n" + txt;
      aiTurns.push_back({"assistant", txt, "live", nowStamp()});
    }
    first = false;
  }
  if (!tools.empty()) aiTurns.push_back({"assistant", "*Did: " + tools + "*", "live", nowStamp()});
  live.savedEntries = E.size();
  aiLayout_.clear();
  aiSaveTurns();
}

void App::liveSavePositions() {
  auto frac = [&](float x, float y, const char* key) {
    if (x < 0 || y < 0 || g.W <= 0 || g.H <= 0) { settings.j.set(key, Json()); return; }
    Json a = Json::array();
    a.push(double(x) / g.W);
    a.push(double(y) / g.H);
    settings.j.set(key, a);
  };
  frac(live.panelX, live.panelY, "livePanelPos");
  frac(live.orbX, live.orbY, "liveOrbPos");
  settings.save();
}

void App::liveRequestPicture(bool canvasOnly, bool fromUser) {
  live.shotUser = fromUser;
  shotRequest("live-vision", canvasOnly);
  needFrame = true;
}

void App::liveSendPicture(const vector<uint8_t>& bgra, int w, int h, bool canvasOnly) {
  bool user = live.shotUser;
  live.shotUser = false;
  auto finish = [&](bool ok, const string& msg) {
    if (agent.active && agent.live && agent.waitShot) { agent.waitShot = false; agentFinishTool(agent.jobTool, ok, msg); }
    else if (!ok) live.log.note(msg);
  };
  if (!live.s || live.phase != 2 || !live.s->ws) { finish(false, "The live session is not connected, so the picture was not sent."); return; }
  if (w <= 0 || h <= 0 || bgra.size() < size_t(w) * size_t(h) * 4) { finish(false, "The picture could not be taken."); return; }
  // shrink to at most 1280 px on the long side (box filter): plenty for the model, small on the wire
  int f = 1;
  while (std::max(w, h) / f > 1280) f++;
  int ow = std::max(1, w / f), oh = std::max(1, h / f);
  vector<uint8_t> px(size_t(ow) * size_t(oh) * 4);
  for (int y = 0; y < oh; y++)
    for (int x = 0; x < ow; x++) {
      unsigned acc[4] = {0, 0, 0, 0};
      for (int dy = 0; dy < f; dy++)
        for (int dx = 0; dx < f; dx++) {
          const uint8_t* q = &bgra[(size_t(y * f + dy) * size_t(w) + size_t(x * f + dx)) * 4];
          for (int c = 0; c < 4; c++) acc[c] += q[c];
        }
      uint8_t* d = &px[(size_t(y) * size_t(ow) + size_t(x)) * 4];
      for (int c = 0; c < 4; c++) d[c] = uint8_t(acc[c] / unsigned(f * f));
      d[3] = 255;
    }
  string jpeg;
  if (!g.encodeJpegWic(ow, oh, px.data(), 0.82f, jpeg)) { finish(false, "The picture could not be encoded."); return; }
  live.s->ws->send(live::imageMessage(jpeg, "image/jpeg"));
  string what = canvasOnly ? "map canvas" : "window";
  live.log.note(string("\xF0\x9F\x93\xB7 Sent a picture of the ") + what + " (" + std::to_string(ow) + "\xC3\x97" + std::to_string(oh) + ", " + fmtInt(long(jpeg.size() / 1024)) + " KB).");
  if (user) {
    liveSendUserText("Here is a picture of what I see on my screen right now (the " + what + "). Look at it.");
    live.awaiting = true;
  }
  finish(true, "A picture of the " + what + " (" + std::to_string(ow) + "x" + std::to_string(oh) + " px) was just sent to you as an image; it is in your context now. Answer from what you see" + (canvasOnly && hasMap() ? ", together with what you know from the data tools." : "."));
  needFrame = true;
}

void App::liveConnect(bool audio) {
  string key = liveKey();
  if (key.empty()) {
    liveDisconnect(true);
    live.phase = 0;
    live.error = "Add a Google Gemini API key in Settings \xE2\x86\x92 Live AI.";
    return;
  }
  bool reconnect = !live.resumeHandle.empty();
  string carry;
  if (reconnect && audio != live.audioMode) {
    // A Live session has exactly one response modality. Resuming a text session as a voice one (or back) is refused by the
    // server ("combination of response modalities (AUDIO, TEXT) is not supported"), so the switch starts a fresh session
    // and hands the conversation so far to the model as context.
    carry = liveCarryContext();
    live.resumeHandle.clear();
    live.reconnects = 0;
    reconnect = false;
    live.log.note(string("Switching to ") + (audio ? "voice" : "text") + " \xC2\xB7 the conversation continues in a new session.");
  }
  liveDisconnect(true);
  live.audioMode = audio;
  live.phase = 1;
  live.error.clear();
  live.awaiting = false;
  live.interaction.clear();
  live.reconnectAt = -1;
  live.model = liveModel();
  if (live.log.entries.empty()) live.allowAll = liveAuto();
  auto s = std::make_shared<LiveSession>();
  s->ws = std::make_shared<WebSocket>(hwnd);
  s->audio = std::make_shared<Audio>();
  s->audio->setBargeIn(settings.j["liveBargeIn"].boolean(false));
  live.s = s;
  live::Options o;
  o.model = live.model;
  o.audio = audio;
  o.voice = settings.j["liveVoice"].str("Puck");
  o.thinking = settings.j["liveThinking"].str("low");
  o.system = liveSystemPrompt();
  if (!carry.empty()) o.system += "\n\nConversation so far in this session (it continues now" + string(audio ? " by voice" : " in text") + "; do not repeat it, do not greet again):\n" + carry;
  o.resumeHandle = live.resumeHandle;
  o.tools = ai::agentTools();
  o.search = settings.j["liveSearch"].boolean(true);  // free-tier keys have no quota for grounding on Live sessions: see livePump
  o.clientVad = audio && settings.j["liveClientVad"].boolean(true) && !live.vadRetried;
  o.vadEndMs = std::max(300, settings.j["liveVadEndMs"].integer(1100));
  o.inputTranscripts = settings.j["liveInputTranscripts"].boolean(false);  // see live::Options::inputTranscripts
  live.clientVad = o.clientVad;
  live.hearing = live.waitingInput = live.turnContent = live.turnNudged = live.turnLife = live.turnAck = live.turnResent = false;
  live.turnT0 = 0;
  live.turnHeard.clear();
  live.searchOn = o.search;
  if (!o.search) live.searchRetried = false;  // the fallback may run again if search is switched back on later
  if (reconnect) live.reconnects++;
  s->ws->connect(live::endpoint(key), live::setupMessage(o));
  if (audio) {
    string err;
    if (!s->audio->startPlayback(&err)) live.log.note("Speaker: " + err + ". Replies are shown as text.");
  }
  needFrame = true;
}

void App::liveDisconnect(bool keepHandle) {
  if (live.s) {
    auto s = live.s;
    live.s.reset();
    if (s->ws && s->ws->connected() && s->audio && s->audio->capturing()) {
      if (live.clientVad && s->gate && s->gate->active) s->ws->send(live::activityEndMessage());
      if (!live.clientVad) s->ws->send(live::audioStreamEndMessage());  // only meaningful with the server's detection
    }
    std::thread([s]() {  // closing waits for the close handshake and the audio threads: never on the UI thread
      if (s->audio) s->audio->stop();
      if (s->ws) s->ws->close();
    }).detach();
  }
  live.phase = 0;
  live.speaking = false;
  live.awaiting = false;
  live.hearing = live.waitingInput = false;
  live.turnT0 = 0;
  live.interaction.clear();
  live.reconnectAt = -1;
  if (keepHandle) return;  // reconnecting: a running tool continues and reports on the next connection
  live.resumeHandle.clear();
  live.pendingResults.clear();
  live.queue.clear();
  if (agent.live && agent.active) {  // a tool was still running for this session
    agent.waitApproval = false;
    agent.waitJob = false;
    agent.active = false;
    live.toolRunning = false;
    live.log.toolStatus(live.toolId, 5, "");
  }
  if (!live.toolRunning && agent.live) agent = AgentRun();
}

// A typed (or synthetic) user message in the form this session needs: realtime text where the server's activity
// detection is on (text counts as activity there); a complete client turn where the application marks the activity
// itself, since realtime text would then wait for activity markers that never come.
void App::liveSendUserText(const string& t) {
  if (!live.s || !live.s->ws) return;
  if (live.clientVad && live.audioMode) live.s->ws->send(live::clientTurnMessage(t));
  else live.s->ws->send(live::textMessage(t));
}

void App::liveSend(const string& textIn) {
  string t = trim(textIn);
  if (t.empty()) return;
  live.log.userFinal(t);
  live.log.turnComplete();
  live.stick = true;
  live.turnCalls = live.nudges = 0;
  if (!live.s || live.phase == 0) { live.pendingText = t; liveConnect(live.talk); return; }
  if (live.phase == 1) { live.pendingText += (live.pendingText.empty() ? "" : "\n") + t; return; }
  liveSendUserText(t);
  live.awaiting = true;
  live.dropAudio = false;
  needFrame = true;
}

bool App::liveStartMic() {
  if (!live.s || !live.s->audio) return false;
  if (live.s->audio->capturing()) return true;
  std::weak_ptr<WebSocket> w = live.s->ws;
  auto gate = std::make_shared<MicGate>(hwnd);
  gate->det.minSpeechDb = float(settings.j["liveVadDb"].num(-56));       // quietest level taken as speech (dBFS)
  gate->det.endMs = std::max(300, settings.j["liveVadEndMs"].integer(1100));  // silence that ends an utterance
  gate->agc.enabled = settings.j["liveAgc"].boolean(true);              // quiet voices are brought up before sending
  live.s->audio->setVoiceProcessing(settings.j["liveMicVoice"].boolean(true));  // the device's noise suppression / echo cancellation
  live.s->gate = gate;
  bool marks = live.clientVad;  // send activityStart / activityEnd (server detection off) or only observe
  string err;
  bool ok = live.s->audio->startCapture([w, gate, marks](const int16_t* pcm, size_t n, bool muted) {
    auto ws = w.lock();
    if (!ws) return;
    gate->det.strict = muted;  // while the assistant speaks (barge-in off) only clear, sustained speech counts — a deliberate interruption
    live::SpeechDetector::Event ev = gate->feed(pcm, n);
    vector<int16_t> buf(pcm, pcm + n);
    gate->agc.process(buf.data(), n, gate->det.active());
    if (!marks) {  // the server's detection: a continuous stream; silence while the assistant speaks, so that it does not hear itself
      if (muted) std::fill(buf.begin(), buf.end(), int16_t(0));
      ws->send(live::audioMessage(buf.data(), n));
      return;
    }
    if (ev == live::SpeechDetector::Start) {
      if (muted) gate->bargeIn();  // the UI thread stops the playback
      ws->send(live::activityStartMessage());
      gate->beginUtterance();
      for (auto& c : gate->pre) {  // the pre-roll, this chunk included
        gate->agc.apply(c.data(), c.size());
        ws->send(live::audioMessage(c.data(), c.size()));
        gate->record(c.data(), c.size());
      }
      gate->pre.clear();
    } else if (gate->det.active()) {
      ws->send(live::audioMessage(buf.data(), n));
      gate->record(buf.data(), n);
    }
    if (ev == live::SpeechDetector::End) ws->send(live::activityEndMessage());
  }, &err);
  if (!ok) {
    live.talk = false;
    live.s->gate.reset();
    live.log.note("Microphone: " + err + ".");
  }
  return ok;
}

void App::liveToggleMic() {
  if (live.s && live.phase > 0 && live.s->audio && live.s->audio->capturing()) {
    bool wasActive = live.s->gate && live.s->gate->active;
    live.s->audio->stopCapture();
    live.talk = false;
    live.hearing = false;
    if (live.s->ws) {
      if (wasActive && live.clientVad) { live.s->ws->send(live::activityEndMessage()); live.awaiting = true; live.turnT0 = nowSeconds(); live.turnContent = live.turnNudged = live.turnLife = live.turnAck = live.turnResent = false; }
      if (!live.clientVad) live.s->ws->send(live::audioStreamEndMessage());
    }
    live.s->gate.reset();
    return;
  }
  live.talk = true;
  live.open = true;
  if (!live.s || live.phase == 0 || !live.audioMode) liveConnect(true);
  else if (live.phase == 2) liveStartMic();
}

void App::liveInterrupt() {
  if (live.s && live.s->audio) live.s->audio->flush();
  live.dropAudio = true;  // the server keeps streaming this turn; play nothing more until it ends
  live.log.interrupted();
}

void App::liveApprove(bool yes, bool all) {
  if (!(agent.live && agent.waitApproval)) return;
  if (all) live.allowAll = true;
  live.log.toolStatus(live.toolId, yes ? 0 : 3, yes ? "Running\xE2\x80\xA6" : "Declined");
  agentApprove(yes, all);
}

void App::liveDiagnose() {
  if (live.probe && !live.probe->done) return;
  string key = liveKey(), model = liveModel();
  if (key.empty()) { live.log.note("No Gemini API key is set (Settings \xE2\x86\x92 Live AI)."); return; }
  auto pb = std::make_shared<AiProbe>();
  live.probe = pb;
  live.log.note("Checking the key, the model and the project's quota with two small requests\xE2\x80\xA6");
  HWND hw = hwnd;
  std::thread([pb, key, model, hw]() {
    auto msgOf = [](const string& body, int st, const string& err) {
      string m = Json::parse(body)["error"]["message"].str();
      if (m.empty()) m = err.empty() ? "HTTP " + std::to_string(st) : err;
      return trim(m);
    };
    const string base = "https://generativelanguage.googleapis.com/v1beta/models/";
    string out = "Diagnosis\n";
    int st = 0;
    string err;
    string body = httpRequest("GET", base + model + "?key=" + key, {}, "", &st, &err, nullptr, 20000);
    if (st == 200) {
      const Json j = Json::parse(body);
      bool bidi = false;
      const Json& m = j["supportedGenerationMethods"];
      for (size_t i = 0; i < m.size(); i++) if (m[i].str() == "bidiGenerateContent") bidi = true;
      out += "\xE2\x80\xA2 " + model + " is visible to this key" + (bidi ? " and supports live sessions.\n" : " but does not list bidiGenerateContent, so it is not a Live model.\n");
    } else out += "\xE2\x80\xA2 " + model + " could not be looked up: " + msgOf(body, st, err) + ". Check the model name in Settings \xE2\x86\x92 Live AI.\n";
    Json req = Json::object();
    {
      Json part = Json::object(); part.set("text", "Reply with the single word OK");
      Json parts = Json::array(); parts.push(part);
      Json c = Json::object(); c.set("parts", parts);
      Json cs = Json::array(); cs.push(c);
      req.set("contents", cs);
      Json gc = Json::object(); gc.set("maxOutputTokens", 8);
      req.set("generationConfig", gc);
    }
    st = 0; err.clear();
    body = httpRequest("POST", base + "gemini-3.8-flash:generateContent?key=" + key, {{"Content-Type", "application/json"}}, req.dump(), &st, &err, nullptr, 30000);
    if (st == 200)
      out += "\xE2\x80\xA2 A plain text request to gemini-3.8-flash works, so the key and the project are fine. A Live session is refused for a quota that is "
             "specific to the session: with Google Search grounding switched on, a free-tier key is refused at setup with exactly this message (VOSStudio switches "
             "it off and reconnects by itself; check Settings \xE2\x86\x92 Live AI). If it still happens with search off, open AI Studio \xE2\x86\x92 Rate limits "
             "(aistudio.google.com/rate-limit) and look for " + model + ": if it shows 0 or is missing, link a billing account (Tier 1) to the project or try the other "
             "Live model in Settings.\n";
    else if (st == 429)
      out += "\xE2\x80\xA2 Even a plain text request is refused: " + msgOf(body, st, err) + "\n  So the project itself has no usable quota: a brand-new project that is not "
             "provisioned yet, a free tier that is not available for the project's region or account, or a billing problem. Create a project in the Google Cloud console "
             "with the Generative Language API enabled (link billing if you have it), import it in AI Studio and make a key for that project.\n";
    else out += "\xE2\x80\xA2 A plain text request to gemini-3.8-flash failed: " + msgOf(body, st, err) + "\n";
    std::lock_guard<std::mutex> lk(pb->m);
    pb->msg = out;
    pb->ok = true;
    pb->done = true;
    PostMessageW(hw, WM_NULL, 0, 0);
  }).detach();
}

bool App::liveBusy() const {
  return live.phase == 1 || live.toolRunning || live.awaiting || live.speaking || live.interaction == "IN_PROGRESS" || !live.pendingText.empty() || !live.queue.empty();
}

// ================================================================= per-frame work
// The user started / stopped talking — from the app's own detector (client markers) or from the server's detection
// (voiceActivity echoes). Start opens a fresh transcript entry; End arms the watchdog (see livePump).
void App::liveUtteranceStart() {
  live.hearing = true;
  live.waitingInput = false;
  live.awaiting = false;
  live.turnT0 = 0;
  live.turnHeard.clear();
  live.log.userBreak();  // each utterance gets its own entry
  live.dirty = true;
  needFrame = true;
}

void App::liveUtteranceEnd() {
  live.hearing = false;
  if (live.phase == 2) {
    live.awaiting = true;
    live.turnT0 = nowSeconds();
    live.turnContent = live.turnNudged = live.turnLife = live.turnAck = live.turnResent = false;
    live.turnCalls = live.nudges = 0;
  }
  live.dirty = true;
  needFrame = true;
}

void App::livePump() {
  if (agent.live && !agent.active && !live.toolRunning) agent = AgentRun();  // the last live tool is finished: free the agent
  if (live.toolRunning && !(agent.live && agent.active)) {  // the agent was reset underneath us (another project was opened)
    liveToolDone(live.toolName, false, "The tool was interrupted because the project changed.");
  }
  if (live.probe && live.probe->done) {
    string m;
    { std::lock_guard<std::mutex> lk(live.probe->m); m = live.probe->msg; }
    live.probe.reset();
    live.log.note(m);
    live.stick = true;
    needFrame = true;
  }
  auto s = live.s;
  if (s) {
    vector<WsEvent> ev;
    s->ws->poll(ev);
    for (auto& e : ev) {
      if (live.s != s) break;  // a handler replaced the connection
      switch (e.kind) {
        case WsEvent::Open: break;
        case WsEvent::Message: {
          live::ServerMessage m;
          if (live::parseServerMessage(e.text, m)) liveHandle(m);
          break;
        }
        case WsEvent::Closed:
        case WsEvent::Error: {
          bool wasConnected = live.phase == 2;
          string why = e.kind == WsEvent::Error ? e.text : live::closeReason(e.code, e.text);
          liveDisconnect(true);
          bool canResume = wasConnected && !live.resumeHandle.empty() && live.reconnects < 3 && e.code != 1008 && e.code != 1007;
          if (!wasConnected && e.kind == WsEvent::Closed) live.resumeHandle.clear();  // the server rejected the setup: do not insist on the handle
          bool quota = lower(why).find("quota") != string::npos;
          if (!wasConnected && e.kind == WsEvent::Closed && quota && live.searchOn && !live.searchRetried) {
            // Google refuses the whole session with "You exceeded your current quota" when the setup asks for Google Search grounding and the
            // key's project has no grounding quota (the free tier). Nothing else in the setup triggers it, so drop the tool and connect again.
            live.searchRetried = true;
            settings.j.set("liveSearch", false);
            settings.save();
            setLiveSearch = false;
            live.log.note("Google refused the session because this key has no quota for Google Search grounding (it is not part of the free tier). "
                          "Web search is now off in Settings \xE2\x86\x92 Live AI; connecting again without it\xE2\x80\xA6");
            if (live.open) liveConnect(live.audioMode);
            break;
          }
          if (!wasConnected && e.kind == WsEvent::Closed && live.clientVad && !live.vadRetried && (lower(why).find("activity") != string::npos || lower(why).find("realtime") != string::npos)) {
            // the server refused the setup with the client-side activity markers: this session uses its own detection
            live.vadRetried = true;
            live.log.note("The server refused the app's speech markers (" + why + "). Using the server's own speech detection for this session.");
            if (live.open) liveConnect(live.audioMode);
            break;
          }
          if (canResume && (live.talk || live.awaiting || live.interaction == "IN_PROGRESS" || live.toolRunning || !live.pendingResults.empty())) {
            live.log.note(why + " Reconnecting\xE2\x80\xA6");
            live.reconnectAt = nowSeconds() + 0.8;
          } else {
            live.error = why;
            string hint = lower(why).find("quota") != string::npos ? " Click Diagnose in the status line to see which quota and why." : "";
            live.log.note(why + hint);
            live.talk = false;
          }
          break;
        }
      }
    }
    s = live.s;
    if (s && s->audio) live.speaking = live.audioMode && s->audio->playing();
    else live.speaking = false;
    if (s && s->gate) {  // speech detector events from the capture thread
      for (int ev : s->gate->take()) {
        if (ev == live::SpeechDetector::Start) liveUtteranceStart();
        else if (ev == live::SpeechDetector::End) liveUtteranceEnd();
        else if (ev == MicGate::BargeIn) liveInterrupt();  // the user spoke up over the assistant
        live.dirty = true;
        needFrame = true;
      }
    }
  }
  // Watchdog: the user stopped talking and nothing came back — no reply, no "thinking", no tool call. The 3.8 Live models
  // sometimes transcribe an utterance and then never answer it (the turn stays open for good; measured with
  // tests/live_probe.py), while a text turn is always answered. So after liveWatchdogS seconds the utterance is relayed
  // as text — the server's own transcript of it, word for word — and if even that brings nothing, the app says so.
  // Step one: the utterance never arrived. Normally the server echoes ACTIVITY_END and sends the transcript within half a
  // second of activityEnd; when neither has come after liveResendS seconds the recording of the utterance is sent once
  // more, as a fresh activity (the probe measured this failure and the recovery).
  if (live.phase == 2 && live.s && live.s->ws && live.s->gate && live.clientVad && live.awaiting && live.turnT0 > 0 && !live.turnAck && !live.turnResent &&
      !live.turnContent && !live.hearing && live.interaction != "IN_PROGRESS" && nowSeconds() - live.turnT0 > std::max(1.5, settings.j["liveResendS"].num(4))) {
    live.turnResent = true;
    vector<int16_t> pcm = live.s->gate->utterance();
    if (pcm.size() >= size_t(live::kInputRate) / 5) {  // at least 200 ms of it
      live.turnT0 = nowSeconds();
      live.log.note("The server did not take that in \xE2\x80\x94 sending it again\xE2\x80\xA6");
      live.s->ws->send(live::activityStartMessage());
      const size_t step = size_t(live::kInputRate) / 5;  // 200 ms per message
      for (size_t i = 0; i < pcm.size(); i += step) live.s->ws->send(live::audioMessage(pcm.data() + i, std::min(step, pcm.size() - i)));
      live.s->ws->send(live::activityEndMessage());
      live.stick = true;
      needFrame = true;
    }
  }
  if (live.phase == 2 && live.s && live.s->ws && live.awaiting && live.turnT0 > 0 && !live.turnContent && live.interaction != "IN_PROGRESS" && !live.toolRunning &&
      live.queue.empty() && !live.speaking && !live.hearing && nowSeconds() - live.turnT0 > std::max(3.0, settings.j["liveWatchdogS"].num(6))) {
    if (!live.turnNudged) {
      live.turnNudged = true;
      live.turnT0 = nowSeconds();
      string heard = trim(live.turnHeard);
      if (!heard.empty()) {
        live.log.note("The model did not answer that \xE2\x80\x94 sending your words as text\xE2\x80\xA6");
        live.s->ws->send(live::clientTurnMessage("(The user just said this by voice and the audio turn produced no answer. Answer it now, as if you had heard it: \"" + heard + "\")"));
      } else {
        live.log.note("No reply came for that \xE2\x80\x94 asking the model to answer\xE2\x80\xA6");
        live.s->ws->send(live::clientTurnMessage("(The user has finished speaking and is waiting, but their words may not have reached you. If you heard them, answer now; if not, say out loud, in one short sentence, that you did not catch it and ask them to say it again.)"));
      }
    } else {
      live.awaiting = false;
      live.turnT0 = 0;
      live.log.note(string("Still nothing from the model. Try again; if it keeps happening, ") + (live.clientVad ? "switch the speech detection in Settings \xE2\x86\x92 Live AI to the server's, or type your question." : "switch on the app's own speech detection in Settings \xE2\x86\x92 Live AI."));
    }
    live.stick = true;
    needFrame = true;
  }
  if (live.phase == 2 && live.reconnects > 0 && nowSeconds() - live.connectedAt > 120) live.reconnects = 0;  // a stable connection
  if (!live.toolRunning && !live.queue.empty()) liveRunNextTool();
  if (live.reconnectAt > 0 && nowSeconds() >= live.reconnectAt && !live.toolRunning && live.interaction != "IN_PROGRESS") {
    live.reconnectAt = -1;
    if (live.open) liveConnect(live.audioMode);
  }
}

void App::liveHandle(const live::ServerMessage& m) {
  auto s = live.s;
  if (!s) return;
  live.lastEventT = nowSeconds();
  if (m.setupComplete) {
    live.phase = 2;
    live.error.clear();
    live.connectedAt = nowSeconds();
    live.log.note((live.reconnects > 0 && !live.log.entries.empty() ? string("Reconnected") : "Connected to " + live::shortModelName(live.model)) + (live.audioMode ? " \xC2\xB7 voice." : " \xC2\xB7 text."));
    live.dropAudio = false;
    for (auto& r : live.pendingResults) s->ws->send(r);
    live.pendingResults.clear();
    if (live.talk) liveStartMic();
    if (!live.pendingText.empty()) {
      liveSendUserText(live.pendingText);
      live.pendingText.clear();
      live.awaiting = true;
    }
  }
  if (!m.error.empty()) { live.error = m.error; live.log.note(m.error); }
  if (!m.interactionStatus.empty()) live.interaction = m.interactionStatus;
  // The server's speech detection (or ours, echoed back). With the server's detection these are the only word on when
  // the user started and stopped, so they drive the "Hearing you…" state and the watchdog exactly like the app's detector.
  if (m.activity == 1 && !live.clientVad && live.audioMode) liveUtteranceStart();
  if (m.activity == 2 && !live.clientVad && live.audioMode) liveUtteranceEnd();
  if (m.activity == 2 || !m.inputTranscript.empty()) live.turnAck = true;  // the server took the utterance in (see the watchdog)
  bool content = false;
  for (auto& t : m.text) { live.log.model(t); content = true; }
  if (!m.outputTranscript.empty()) { live.log.model(m.outputTranscript); content = true; }
  if (!m.inputTranscript.empty()) {
    live.log.userFinal(m.inputTranscript);
    live.turnCalls = live.nudges = 0;
    if (!live.turnContent) live.turnHeard += m.inputTranscript;  // what the watchdog relays if no answer follows
    // no detector events at all (an older model): the transcript itself marks the end of the utterance
    if (!live.clientVad && live.audioMode && !live.hearing && !live.awaiting && live.phase == 2) liveUtteranceEnd();
  } else if (!m.interimInputTranscript.empty()) live.log.userInterim(m.interimInputTranscript);
  if (!m.calls.empty()) { live.turnCalls += int(m.calls.size()); live.turnContent = true; live.waitingInput = false; }
  if (!m.audio.empty() && s->audio) { if (!live.dropAudio) for (auto& a : m.audio) s->audio->play(a); content = true; }
  if (content) { live.awaiting = false; live.stick = true; live.turnContent = true; live.waitingInput = false; live.turnT0 = 0; live.turnHeard.clear(); }
  if (m.interactionStatus == "IN_PROGRESS") live.turnContent = true;  // the model is on it
  // An empty serverContent arrives shortly before the first audio of an answer: a sign of life worth one more wait
  // (but not an answer — in the failing sessions it sometimes came and nothing followed).
  if (m.serverContent && !content && live.awaiting && live.turnT0 > 0 && !live.turnContent && !live.turnLife && m.inputTranscript.empty() && m.interimInputTranscript.empty()) {
    live.turnLife = true;
    live.turnT0 = nowSeconds();
  }
  if (m.waitingForInput) { live.waitingInput = true; live.dirty = true; }
  if (m.interrupted) { if (s->audio) s->audio->flush(); live.log.interrupted(); live.dropAudio = false; }
  if (m.turnComplete) {
    live.log.turnComplete();
    live.awaiting = false;
    live.dropAudio = false;
    live.waitingInput = false;
    if (live.turnContent) live.turnHeard.clear();
    if (!live.turnContent && !m.turnCompleteReason.empty() && live.audioMode) {
      // an empty turn with a reason: the model decided not to answer ("NEED_MORE_INPUT": it thought the user was not
      // finished; "RESPONSE_REJECTED": it produced nothing). Ask once in plain words rather than sit in silence.
      bool more = m.turnCompleteReason.find("MORE_INPUT") != string::npos;
      if (!live.turnNudged && live.phase == 2) {
        live.turnNudged = true;
        live.turnT0 = nowSeconds();
        live.awaiting = true;
        live.log.note(more ? "The model waited for more instead of answering \xE2\x80\x94 telling it you have finished\xE2\x80\xA6" : "The model closed the turn without an answer (" + m.turnCompleteReason + ") \xE2\x80\x94 asking again\xE2\x80\xA6");
        s->ws->send(live::clientTurnMessage(more ? "(The user has finished speaking; that was the whole question. Answer it now.)"
                                                  : "(Your last turn ended without any answer reaching the user. Answer their last request now.)"));
      } else {
        live.log.note("The model ended the turn without answering (" + m.turnCompleteReason + ").");
      }
    }
    // The model sometimes apologises for "a system error" when its own function call failed inside the service (nothing
    // reached the application: no tool row appears). One retry per user turn, asked for in plain words.
    if (live.turnCalls == 0 && live.nudges == 0 && live.phase == 2) {
      string last;
      for (size_t i = live.log.entries.size(); i-- > 0;) { const auto& e = live.log.entries[i]; if (e.kind == live::Entry::User) break; if (e.kind == live::Entry::Model) { last = lower(e.text); break; } }
      bool apology = !last.empty() && (last.find("system error") != string::npos || last.find("an error occurred") != string::npos || last.find("something went wrong") != string::npos ||
                                       last.find("encountered an error") != string::npos || last.find("internal error") != string::npos || last.find("unable to complete") != string::npos);
      if (apology) {
        live.nudges++;
        live.log.note("The model reported an internal error before any tool call reached the application \xE2\x80\x94 asking it to try again.");
        liveSendUserText("That error happened on your side before any tool was called; nothing failed in the application. Carry out my last request now by calling the right tool (load_data, build_map, run_commands, ...).");
        live.awaiting = true;
      }
    }
  }
  if (m.generationComplete) live.awaiting = false;
  if (!m.searchQueries.empty()) live.log.note("Searched the web: " + join(m.searchQueries, "; "));
  for (auto& c : m.calls) live.queue.push_back(c);
  for (auto& id : m.cancelIds) {
    for (auto it = live.queue.begin(); it != live.queue.end();) it = it->id == id ? live.queue.erase(it) : std::next(it);
    if (live.toolRunning && id == live.toolId) live.toolCancelled = true;
  }
  if (m.goAway) live.reconnectAt = nowSeconds() + std::max(0.0, m.goAwaySeconds - 3);
  if (m.resumptionUpdate && m.resumable && !m.resumeHandle.empty()) live.resumeHandle = m.resumeHandle;
  if (m.totalTokens >= 0) live.tokens = m.totalTokens;
  // words and sound only move the overlay; tool calls and session changes may change anything
  if (m.setupComplete || !m.calls.empty() || !m.cancelIds.empty() || !m.error.empty()) needFrame = true;
  else live.dirty = true;
}

// ================================================================= tools
void App::liveRunNextTool() {
  if (live.toolRunning || live.queue.empty()) return;
  if (agent.live && !agent.active) agent = AgentRun();
  live::FunctionCall fc = live.queue.front();
  live.queue.pop_front();
  if (agent.active) {  // the Assistant's own agent is working in this project
    liveSendResult(fc.id, fc.name, false, "The Assistant's agent is busy in this project. Try again in a moment.");
    return;
  }
  const ai::ToolSpec* spec = ai::findTool(fc.name);
  if (!spec) { liveSendResult(fc.id, fc.name, false, "There is no tool with this name."); return; }
  if (!P) { liveSendResult(fc.id, fc.name, false, "No project is open."); return; }
  ai::AgentAction a;
  a.tool = fc.name;
  a.args = fc.args;
  agent = AgentRun();
  agent.active = true;
  agent.live = true;
  agent.turn = -1;
  agent.allowAll = live.allowAll;
  agent.steps = 1;
  live.toolRunning = true;
  live.toolCancelled = false;
  live.toolId = fc.id;
  live.toolName = fc.name;
  string title = agentDescribe(a);
  live.log.tool(fc.id, title);
  live.stick = true;
  if (actionKind(a) == ai::ToolKind::Change && !live.allowAll) {
    agent.pending = a;
    agent.waitApproval = true;
    agent.log.push_back({title, "", "", 4});
    live.log.toolStatus(fc.id, 4, "Waiting for your approval");
    if (!live.open) live.open = true;
    if (live.mini) liveSetMini(false);  // the approval card needs the panel
    return;
  }
  agent.log.push_back({title, "", "", 0});
  needFrame = true;  // whatever the tool changes is drawn by a full frame
  agentExecute(a);  // may finish synchronously (→ agentFinishTool → liveToolDone) or wait for a job (agentPump)
}

void App::liveToolDone(const string& tool, bool ok, const string& result) {
  if (!live.toolRunning) return;
  string id = live.toolId;
  bool cancelled = live.toolCancelled;
  int status = ok ? 1 : 2;
  string detail;
  if (!agent.log.empty()) {
    if (agent.log.back().status == 3) status = 3;
    detail = agent.log.back().detail;
  }
  if (detail.empty()) detail = firstLineOf(result);
  live.changes += agent.changes;
  agent.changes = 0;
  // the agent state stays allocated until the next frame: agentExecute(agent.pending) may still be on the stack
  agent.active = false;
  agent.waitApproval = false;
  agent.waitJob = false;
  live.toolRunning = false;
  live.log.toolStatus(id, cancelled ? 5 : status, detail);
  if (!cancelled) liveSendResult(id, tool, ok, result);
  post([this] { liveRunNextTool(); });
  needFrame = true;
}

void App::liveSendResult(const string& id, const string& name, bool ok, const string& result) {
  live::FunctionResult r;
  r.id = id;
  r.name = name;
  r.ok = ok;
  r.result = live::truncateResult(result);
  string msg = live::toolResponseMessage({r});
  if (live.s && live.phase == 2) live.s->ws->send(msg);
  else live.pendingResults.push_back(msg);
}

// ================================================================= pop-up
string App::liveStatus() const {
  if (live.phase == 1) return live.reconnects ? "Reconnecting\xE2\x80\xA6" : "Connecting\xE2\x80\xA6";
  if (live.phase == 0) return !live.error.empty() ? live.error : liveKey().empty() ? "Add a Google Gemini API key in Settings \xE2\x86\x92 AI Assistant." : "Not connected";
  if (agent.live && agent.waitApproval) return "Waiting for your approval";
  if (live.toolRunning) {
    if (agent.waitJob) return agent.jobTool == "search_openalex" ? "Searching OpenAlex\xE2\x80\xA6" : "Building the map\xE2\x80\xA6";
    return "Working\xE2\x80\xA6";
  }
  if (live.speaking) return "Speaking\xE2\x80\xA6";
  if (live.hearing) return "Hearing you\xE2\x80\xA6";
  if (live.waitingInput) return "Waiting for you to continue\xE2\x80\xA6";
  if (live.interaction == "IN_PROGRESS" || live.awaiting) return "Thinking\xE2\x80\xA6";
  bool mic = live.s && live.s->audio && live.s->audio->capturing();
  if (mic) return "Listening";
  return live.audioMode ? "Ready \xE2\x80\x94 talk or type" : "Ready \xE2\x80\x94 type a message";
}

Rect App::liveRect() const {
  float s = ui.s;
  float w = std::min(350 * s, float(g.W) - 24 * s);
  float top = topR.h > 0 ? topR.b() : 52 * s;
  float bottom = statusR.h > 0 ? statusR.y : float(g.H) - 26 * s;
  float maxH = bottom - top - 24 * s;
  float h = clampv(std::round(float(g.H) * 0.42f), 260 * s, 460 * s);
  h = std::min(h, maxH);
  if (live.panelX >= 0 && live.panelY >= 0) {  // dragged somewhere: keep it inside the window
    float x = clampv(live.panelX, 0.f, std::max(0.f, float(g.W) - w)), y = clampv(live.panelY, 0.f, std::max(0.f, float(g.H) - h));
    return {std::round(x), std::round(y), w, h};
  }
  return {std::round(float(g.W) - w - 12 * s), std::round(bottom - 12 * s - h), w, h};
}

void App::liveSetMini(bool m) {
  if (m != live.mini) {  // animate between the panel and the orb (drawLiveMorph)
    live.miniFrom = liveRect();
    live.miniTo = m;
    live.miniT0 = ui.time;
  }
  live.mini = m;
  if (!m) { live.stick = true; live.focusWanted = !live.talk; }
  needFrame = true;
}

Rect App::liveOrbRect() const {
  float s = ui.s, d = 62 * s;
  if (live.orbX >= 0 && live.orbY >= 0) {
    float x = clampv(live.orbX, 4 * s, std::max(4 * s, float(g.W) - d - 4 * s)), y = clampv(live.orbY, 4 * s, std::max(4 * s, float(g.H) - d - 4 * s));
    return {std::round(x), std::round(y), d, d};
  }
  float bottom = statusR.h > 0 ? statusR.y : float(g.H) - 26 * s;
  return {std::round(float(g.W) - d - 22 * s), std::round(bottom - 22 * s - d), d, d};
}

// the orb owns a little more than its circle: the two small buttons above it and the space its aura moves in
Rect App::liveHitRect() const {
  if (!live.mini) return liveRect();
  Rect o = liveOrbRect();
  float s = ui.s;
  bool below = o.y < 50 * s;  // dragged to the top edge: the buttons sit under the orb
  return {o.x - 22 * s, o.y - (below ? 18 : 46) * s, o.w + 44 * s, o.h + 64 * s};
}

// ================================================================= the orb
// A small luminous sphere that lives while the session lives: an aura of soft, slowly turning shapes that swell with
// the sound (yours while it listens, its own while it speaks), a ring of voice bars, ripples on the strong syllables,
// electrons in orbit while it thinks, a red pulse on errors. Everything eases, nothing snaps.
namespace {

float easeOutCubic(float u) { u = clampv(u, 0.f, 1.f); return 1 - (1 - u) * (1 - u) * (1 - u); }
float easeInOutCubic(float u) { u = clampv(u, 0.f, 1.f); return u < 0.5f ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3.f) / 2; }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
Rect lerpRect(const Rect& a, const Rect& b, float t) { return {lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.w, b.w, t), lerpf(a.h, b.h, t)}; }
// smooth pseudo-noise in [0, 1] from a seed and time: three incommensurable sines
float noise1(float seed, double t) { return 0.5f + 0.5f * float(std::sin(t * 1.7 + seed * 12.9898) * 0.5 + std::sin(t * 2.9 + seed * 78.233) * 0.3 + std::sin(t * 0.7 + seed * 37.719) * 0.2); }

struct OrbPaint {
  ID2D1DeviceContext* dc;
  ID2D1Factory1* f;
  Ui* ui;
  ID2D1SolidColorBrush* solid(const Color& c) { return ui->g->br(c); }
  // radial gradient inside an ellipse: stops as (position, colour)
  void radial(float cx, float cy, float rx, float ry, float offx, float offy, const vector<std::pair<float, Color>>& stops) {
    vector<D2D1_GRADIENT_STOP> st;
    for (auto& p : stops) st.push_back(D2D1::GradientStop(p.first, D2D1::ColorF(p.second.r, p.second.g, p.second.b, p.second.a)));
    Com<ID2D1GradientStopCollection> gsc;
    if (FAILED(dc->CreateGradientStopCollection(st.data(), UINT32(st.size()), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, gsc.put())) || !gsc) return;
    Com<ID2D1RadialGradientBrush> rb;
    if (FAILED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(offx, offy), rx, ry), gsc.get(), rb.put())) || !rb) return;
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), rb.get());
  }
  // an organic closed shape: radius = base * (1 + sum of low harmonics), filled with one colour
  void blob(float cx, float cy, float base, const float* amp, const float* ph, const int* freq, int harmonics, float rot, ID2D1Brush* brush) {
    const int n = 96;
    D2D1_POINT_2F pts[n];
    for (int i = 0; i < n; i++) {
      float th = float(i) * 6.2831853f / n;
      float r = 1;
      for (int k = 0; k < harmonics; k++) r += amp[k] * std::sin(freq[k] * th + ph[k]);
      float a = th + rot;
      pts[i] = D2D1::Point2F(cx + std::cos(a) * base * r, cy + std::sin(a) * base * r);
    }
    Com<ID2D1PathGeometry> geo;
    if (FAILED(f->CreatePathGeometry(geo.put())) || !geo) return;
    Com<ID2D1GeometrySink> sk;
    if (FAILED(geo->Open(sk.put())) || !sk) return;
    sk->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    sk->AddLines(pts + 1, n - 1);
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    sk->Close();
    dc->FillGeometry(geo.get(), brush);
  }
  ID2D1StrokeStyle* roundCaps() {
    static Com<ID2D1StrokeStyle> ss;
    if (!ss) f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, ss.put());
    return ss.get();
  }
  // a rounded bar along the radius at angle `ang`, from r0 outwards
  void bar(float cx, float cy, float ang, float r0, float len, float w, const Color& col) {
    float c = std::cos(ang), sn = std::sin(ang);
    dc->DrawLine(D2D1::Point2F(cx + c * r0, cy + sn * r0), D2D1::Point2F(cx + c * (r0 + len), cy + sn * (r0 + len)), solid(col), w, roundCaps());
  }
};

Color liveStateColor(const UiColors& c, bool err, bool off, bool speaking, bool listening, bool thinking) {
  if (err) return c.danger;
  if (off) return Color(0.56f, 0.60f, 0.68f);
  if (speaking) return Color(0.99f, 0.52f, 0.34f);      // coral: its own voice
  if (listening) return Color(0.14f, 0.78f, 0.60f);     // teal: yours
  if (thinking) return Color(0.58f, 0.44f, 0.98f);      // violet: reasoning / working
  return c.accent;
}

}  // namespace

void App::drawLiveOrb() {
  float s = ui.s;
  Rect o = liveOrbRect();
  float cx = o.x + o.w / 2, cy = o.y + o.h / 2, R0 = o.w / 2;
  auto& A = live.orb;
  bool mic = live.s && live.s->audio && live.s->audio->capturing();
  bool paused = mic && live.s->audio->micPaused();
  bool err = live.phase == 0 && !live.error.empty();
  bool off = live.phase == 0;
  bool thinking = live.phase == 1 || live.interaction == "IN_PROGRESS" || live.awaiting || live.toolRunning;
  bool listening = mic && !paused && !live.speaking;
  float inL = mic && !paused ? live.s->audio->inLevel() : 0;
  float outL = live.speaking && live.s && live.s->audio ? live.s->audio->outLevel() : 0;
  double t = ui.time;
  // ---- motion state: smoothed levels, phases, colour
  float dt = A.lastT > 0 ? clampv(float(t - A.lastT), 0.f, 0.05f) : 0.016f;
  A.lastT = t;
  auto ease = [&](float& v, float target, float attack, float release) { float k = target > v ? attack : release; v += (target - v) * (1 - std::exp(-dt * k)); };
  ease(A.in, clampv(inL * 1.6f, 0.f, 1.f), 22.f, 7.f);
  ease(A.out, clampv(outL * 1.25f, 0.f, 1.f), 26.f, 8.f);
  float level = std::max(A.in, A.out);
  ease(A.energy, level, 6.f, 2.5f);
  float speed = 0.35f + 1.6f * A.energy + (thinking ? 0.9f : 0.f);
  A.spin += dt * speed;
  const float phSpeed[4] = {0.9f, -1.3f, 0.6f, -0.45f};
  for (int k = 0; k < 4; k++) A.ph[k] += dt * phSpeed[k] * (0.6f + 1.8f * A.energy + (thinking ? 0.8f : 0.f));
  A.breathe += dt * (off ? 0.9f : 1.6f);
  Color want = liveStateColor(ui.c, err, off, live.speaking, listening, thinking);
  if (A.hue < 0) { A.col[0] = want.r; A.col[1] = want.g; A.col[2] = want.b; A.hue = 1; }
  float ck = 1 - std::exp(-dt * 5.f);
  A.col[0] += (want.r - A.col[0]) * ck; A.col[1] += (want.g - A.col[1]) * ck; A.col[2] += (want.b - A.col[2]) * ck;
  Color base(A.col[0], A.col[1], A.col[2]);
  Color light = base.mix(Color(1, 1, 1), 0.55f), deep = base.mix(Color(0.05f, 0.04f, 0.12f), 0.45f);
  // ---- interaction (buttons first: they lie outside the sphere but inside its rectangle's neighbourhood)
  bool nearby = liveHitRect().has(ui.in.mx, ui.in.my);
  ease(A.hover, nearby ? 1.f : 0.f, 14.f, 8.f);
  float bw = 24 * s;
  bool below = o.y < 50 * s;
  float by = below ? o.b() + 10 * s : o.y - 34 * s;
  Rect bExpand{cx - bw - 4 * s, by, bw, bw}, bClose{cx + 4 * s, by, bw, bw};
  uint64_t idExpand = ui.id("live:orb:expand"), idClose = ui.id("live:orb:close"), oid = ui.id("live:orb");
  bool hExpand = false, hClose = false, hov = false;
  bool doExpand = false, doClose = false, doMic = false;
  if (A.hover > 0.05f && !live.dragging) {
    if (ui.behave(idExpand, bExpand, &hExpand)) doExpand = true;
    if (ui.behave(idClose, bClose, &hClose)) doClose = true;
    ui.tipFor(idExpand, "Open the panel (or double-click / right-click the orb)");
    ui.tipFor(idClose, "Close and end the session");
  }
  bool overSphere = std::hypot(ui.in.mx - cx, ui.in.my - cy) <= R0 * 1.15f;
  bool clicked = ui.behave(oid, o, &hov) && overSphere;
  hov = hov && overSphere;
  bool held = ui.active == oid && ui.in.down[0];
  // drag the orb anywhere: a press that moves more than a few pixels; a still press-and-release is a click
  if (ui.in.pressed[0] && overSphere) { live.dragging = true; live.dragMoved = false; live.dragDx = ui.in.mx - o.x; live.dragDy = ui.in.my - o.y; }
  if (live.dragging && held) {
    float nx = ui.in.mx - live.dragDx, ny = ui.in.my - live.dragDy;
    if (live.dragMoved || std::fabs(nx - o.x) > 4 * s || std::fabs(ny - o.y) > 4 * s) { live.dragMoved = true; live.orbX = nx; live.orbY = ny; needFrame = true; }
    ui.cursor = "move";
  } else if (live.dragging && !ui.in.down[0]) {
    live.dragging = false;
    if (live.dragMoved) { liveSavePositions(); clicked = false; A.clickT = 0; }
  }
  bool dragMove = live.dragging && live.dragMoved;
  held = held && overSphere && !dragMove;
  ease(A.press, held ? 1.f : 0.f, 30.f, 10.f);
  if (overSphere && ui.in.dbl && !dragMove) { doExpand = true; A.clickT = 0; live.dragging = false; }
  else if (clicked && !dragMove) A.clickT = t;  // a single click acts after the double-click window
  if (A.clickT > 0 && (t - A.clickT > 0.3 || live.phase == 0)) { A.clickT = 0; doMic = true; }
  if (A.clickT > 0) liveAnim(30);
  if (ui.in.pressed[1] && overSphere) doExpand = true;
  if (ui.in.pressed[0] && overSphere) A.ripples.push_back({t, R0 * 0.9f, 0.5f});
  string tip = liveStatus() + "\nClick: " + (mic ? "stop talking" : "talk") + " \xC2\xB7 double-click or right-click: open the panel \xC2\xB7 drag to move";
  ui.tipFor(oid, tip);
  // ---- geometry that follows the state
  float scale = 1 + 0.04f * A.hover - 0.07f * A.press + 0.10f * A.energy + (off ? 0.f : 0.012f * std::sin(A.breathe));
  float R = R0 * 0.86f * scale;  // the sphere; the aura lives in the rest of the rectangle and a little beyond
  OrbPaint P_{ui.dc(), g.d2f.get(), &ui};
  bool alive = !off;
  // frame budget: the orb is redrawn alone over a cached picture of the window (App::frame), so it may move often while
  // something happens and slowly while it merely breathes
  bool busyLook = live.speaking || listening || thinking || A.energy > 0.02f || A.hover > 0.02f || A.press > 0.02f || !A.ripples.empty() || live.dragging || err;
  if (alive || err) liveAnim(busyLook ? 30 : 12);
  // ---- 1. ambient glow (wide, soft), stronger with sound and while thinking
  {
    float ga = (off ? 0.10f : 0.22f) + 0.30f * A.energy + (thinking ? 0.06f + 0.05f * float(std::sin(t * 3.1)) : 0.f);
    if (err) ga = 0.22f + 0.16f * float(std::sin(t * 2.4));
    float gr = R0 * (1.9f + 0.9f * A.energy);
    P_.radial(cx, cy, gr, gr, cx, cy, {{0.f, base.withA(ga)}, {0.45f, base.withA(ga * 0.45f)}, {1.f, base.withA(0.f)}});
  }
  // ---- 2. the aura: three slowly turning organic layers that swell with the voice
  if (alive || err) {
    const int freq[3] = {3, 5, 2};
    float e = A.energy;
    for (int layer = 0; layer < 3; layer++) {
      float amp[3] = {0.035f + 0.16f * e + 0.02f * layer, 0.025f + 0.10f * e, 0.02f + 0.06f * e};
      float ph[3] = {A.ph[layer % 4] + layer * 1.9f, A.ph[(layer + 1) % 4] * 1.3f, A.ph[(layer + 2) % 4] * 0.7f + layer};
      float baseR = R * (1.14f + 0.10f * layer + 0.26f * e + (thinking ? 0.05f : 0.f));
      float rot = A.spin * (layer % 2 ? -0.7f : 0.5f) + layer * 2.1f;
      Color lc = (layer == 1 ? base.mix(Color(0.45f, 0.55f, 1.f), 0.35f) : layer == 2 ? light : base).withA((0.16f - 0.03f * layer) * (0.7f + 0.6f * e));
      P_.blob(cx, cy, baseR, amp, ph, freq, 3, rot, P_.solid(lc));
    }
  }
  // ---- 3. ripples: born on strong syllables and on clicks, drift outwards and fade
  if (live.speaking && A.out > A.energy + 0.10f && t - A.lastRipple > 0.26) { A.lastRipple = t; A.ripples.push_back({t, R * 1.05f, 0.42f}); }
  if (listening && A.in > 0.55f && t - A.lastRipple > 0.5) { A.lastRipple = t; A.ripples.push_back({t, R * 1.05f, 0.25f}); }
  for (size_t i = 0; i < A.ripples.size();) {
    float u = float((t - A.ripples[i].t0) / 0.95);
    if (u >= 1) { A.ripples.erase(A.ripples.begin() + long(i)); continue; }
    float rr = A.ripples[i].r0 + easeOutCubic(u) * R0 * 1.5f;
    ui.circle(cx, cy, rr, light.withA(A.ripples[i].a * (1 - u) * (1 - u)), false, (1.8f * (1 - u) + 0.4f) * s);
    i++;
  }
  // ---- 4. the ring of voice bars (an equaliser around the sphere; a calm breathing ring when nothing is said)
  {
    const int nb = 36;
    float src = live.speaking ? A.out : listening ? A.in : 0.f;
    for (int i = 0; i < nb; i++) {
      float target = src > 0.02f ? src * (0.35f + 0.65f * noise1(float(i), t * 1.6)) : 0.f;
      float k = target > A.bars[i] ? 1 - std::exp(-dt * 24) : 1 - std::exp(-dt * 9);
      A.bars[i] += (target - A.bars[i]) * k;
    }
    if (alive) {
      float r0 = R + 3.5f * s;
      for (int i = 0; i < nb; i++) {
        float ang = float(i) * 6.2831853f / nb - 1.5707963f + A.spin * 0.25f;
        float calm = (1.2f + 1.0f * float(std::sin(A.breathe * 1.4 + i * 0.55))) * s;
        float len = calm + (2.f + 15.f * A.bars[i]) * s;
        float a = 0.18f + 0.75f * A.bars[i] + (thinking ? 0.12f * noise1(float(i) + 7, t * 3) : 0.f);
        Color bc = (live.speaking ? light : base.mix(Color(1, 1, 1), 0.3f)).withA(std::min(1.f, a));
        P_.bar(cx, cy, ang, r0, len, (1.6f + 1.2f * A.bars[i]) * s, bc);
      }
    }
  }
  // ---- 5. electrons in orbit while it thinks or works (the far half passes behind the sphere)
  struct Orbiter { float x, y, depth, size; float alpha; };
  vector<Orbiter> orbs;
  if (thinking) {
    for (int k = 0; k < 3; k++) {
      float tilt = 0.5f + k * 1.05f;
      for (int trail = 0; trail < 5; trail++) {
        float a = float(t * (2.4 + 0.5 * k)) + k * 2.094f - trail * 0.16f;
        float ex = std::cos(a) * R * 1.55f, ey = std::sin(a) * R * 0.42f;
        float x = cx + ex * std::cos(tilt) - ey * std::sin(tilt), y = cy + ex * std::sin(tilt) + ey * std::cos(tilt);
        orbs.push_back({x, y, std::sin(a), (2.6f - trail * 0.4f) * s, trail == 0 ? 0.95f : 0.55f - trail * 0.1f});
      }
    }
    for (auto& ob : orbs) if (ob.depth < 0) ui.circle(ob.x, ob.y, ob.size * 0.85f, light.withA(ob.alpha * 0.45f));
  }
  // ---- 6. the sphere: shaded, with a highlight that keeps its place while the colour changes
  ui.shadow({cx - R, cy - R, 2 * R, 2 * R}, R, 10 * s);
  P_.radial(cx, cy, R, R, cx - R * 0.32f, cy - R * 0.36f, {{0.f, light.mix(Color(1, 1, 1), 0.35f)}, {0.35f, base.mix(light, 0.25f)}, {0.8f, base}, {1.f, deep}});
  ui.circle(cx, cy, R, deep.withA(0.35f), false, 1.f * s);
  P_.radial(cx - R * 0.3f, cy - R * 0.38f, R * 0.42f, R * 0.30f, cx - R * 0.34f, cy - R * 0.42f, {{0.f, Color(1, 1, 1, 0.42f)}, {1.f, Color(1, 1, 1, 0.f)}});
  if (thinking) for (auto& ob : orbs) if (ob.depth >= 0) ui.circle(ob.x, ob.y, ob.size, Color(1, 1, 1, ob.alpha));
  // ---- 7. what it is doing, drawn on the sphere
  Color fg(1, 1, 1, 0.95f);
  if (live.speaking) {  // a compact waveform inside
    for (int i = 0; i < 5; i++) {
      float h = (4 + 13 * clampv(A.out * (0.6f + 0.4f * noise1(float(i) + 3, t * 2.2)), 0.f, 1.f)) * s;
      ui.fill({cx - 12 * s + i * 6 * s, cy - h / 2, 3 * s, h}, fg, 1.5f * s);
    }
  } else if (mic) {
    ui.icon("mic", cx, cy, (19 + 7 * A.in) * s, paused ? fg.withA(0.45f) : fg, 2.f);
    if (paused) ui.text({cx - 30 * s, cy + R + 4 * s, 60 * s, 12 * s}, "paused", 9.5f * s, ui.c.textFaint, AL_CENTER);
  } else if (thinking && !live.toolRunning) {
    ui.icon("live", cx, cy, 20 * s, fg.withA(0.75f + 0.25f * float(std::sin(t * 6))), 1.9f);
  } else if (live.toolRunning) {  // a working mark: a small rotating arc
    float a0 = float(std::fmod(t * 4.0, 6.2831853));
    for (int i = 0; i < 14; i++) { float a = a0 + i * 0.22f; ui.circle(cx + std::cos(a) * 8 * s, cy + std::sin(a) * 8 * s, 1.6f * s, fg.withA(1 - i / 14.f)); }
  } else ui.icon("live", cx, cy, 22 * s, fg, 1.9f);
  // ---- 8. the two small buttons above (fade in when the mouse is near)
  if (A.hover > 0.02f) {
    auto tiny = [&](const Rect& b, const char* icon, bool bh) {
      float a = A.hover;
      Rect bb{b.x, b.y + (1 - a) * 8 * s, b.w, b.h};
      ui.shadow(bb, bb.w / 2, 6 * s);
      ui.circle(bb.x + bb.w / 2, bb.y + bb.h / 2, bb.w / 2, (bh ? ui.c.hover : ui.c.panel2).withA(a));
      ui.circle(bb.x + bb.w / 2, bb.y + bb.h / 2, bb.w / 2, ui.c.border.withA(a), false, 1.f);
      ui.icon(icon, bb.x + bb.w / 2, bb.y + bb.h / 2, 11.5f * s, (bh ? ui.c.text : ui.c.textDim).withA(a), 1.8f);
    };
    tiny(bExpand, "expand", hExpand);
    tiny(bClose, "x", hClose);
    liveAnim(30);
  }
  // ---- 9. caption: the error, or what is being said (stays a few seconds after the last words)
  string cap;
  Color capCol = ui.c.text;
  if (err) { cap = live.error; capCol = ui.c.danger; }
  else if (off) cap = liveKey().empty() ? "Add a Gemini key in Settings \xE2\x86\x92 Live AI" : "Click to talk";
  else {
    for (auto it = live.log.entries.rbegin(); it != live.log.entries.rend(); ++it)
      if (it->kind == live::Entry::Model && !it->text.empty()) { if (live.speaking || it->open || nowSeconds() - live.lastEventT < 6) cap = it->text; break; }
    if (cap.empty() && live.toolRunning) cap = live.log.entries.empty() ? "Working\xE2\x80\xA6" : "Working: " + live.log.entries.back().text;
  }
  if (!cap.empty() && !dragMove) {
    string txt = truncate(cap, 170);
    bool right = o.x - 40 * s < 120 * s && float(g.W) - o.r() > o.x;  // orb near the left edge: the bubble goes to its right
    float maxW = std::min(300 * s, right ? float(g.W) - o.r() - 40 * s : o.x - 40 * s);
    if (maxW > 80 * s) {
      float tw = std::min(maxW - 20 * s, ui.textW(txt, 12 * s) + 2 * s);
      float th = ui.textWrap({0, 0, tw, 300 * s}, txt, 12 * s, capCol, 400, false);
      float bh = th + 14 * s, bwid = tw + 20 * s;
      Rect cb{right ? o.r() + 16 * s : o.x - 16 * s - bwid, clampv(std::round(cy - bh / 2), 4 * s, std::max(4 * s, float(g.H) - bh - 4 * s)), bwid, bh};
      ui.shadow(cb, 10 * s, 8 * s);
      ui.fill(cb, ui.c.panel2, 10 * s);
      ui.stroke(cb, ui.c.border, 10 * s);
      // a small pointer towards the orb
      float px = right ? cb.x - 5 * s : cb.r() + 5 * s;
      ui.circle(px, cy, 3 * s, ui.c.panel2);
      ui.circle(px, cy, 3 * s, ui.c.border, false, 1.f);
      ui.textWrap({cb.x + 10 * s, cb.y + 7 * s, tw, th + 2 * s}, txt, 12 * s, capCol);
    }
  }
  // ---- actions last (they may replace this widget)
  if (doClose) { liveClose(); return; }
  if (doExpand) { liveSetMini(false); return; }
  if (doMic) liveToggleMic();
}

// The panel shrinking into the orb, or the orb growing into the panel (about 0.45 s).
void App::drawLiveMorph(float u) {
  float s = ui.s;
  Rect panel = live.miniFrom.w > 0 ? live.miniFrom : liveRect();
  Rect o = liveOrbRect();
  bool toOrb = live.miniTo;
  float k = toOrb ? easeInOutCubic(u) : easeOutCubic(u);
  Rect cur = toOrb ? lerpRect(panel, o, k) : lerpRect(o, panel, k);
  float rad = toOrb ? lerpf(12 * s, o.w / 2, k) : lerpf(o.w / 2, 12 * s, k);
  float orbness = toOrb ? k : 1 - k;  // 1 = looks like the orb
  auto& A = live.orb;
  Color base = A.hue < 0 ? ui.c.accent : Color(A.col[0], A.col[1], A.col[2]);
  float cx = cur.x + cur.w / 2, cy = cur.y + cur.h / 2;
  OrbPaint P_{ui.dc(), g.d2f.get(), &ui};
  // glow grows with orbness
  float gr = std::max(cur.w, cur.h) * (0.7f + 0.5f * orbness);
  P_.radial(cx, cy, gr, gr, cx, cy, {{0.f, base.withA(0.28f * orbness)}, {1.f, base.withA(0.f)}});
  ui.shadow(cur, rad, 14 * s);
  Color fillc = ui.c.panel2.mix(base, orbness * orbness);
  ui.fill(cur, fillc, rad);
  ui.stroke(cur, ui.c.border.withA(1 - orbness), rad);
  // the panel's header ghost fades out / in
  float ha = (1 - orbness) * (1 - orbness);
  if (ha > 0.02f && cur.w > 120 * s) {
    ui.circle(cur.x + 18 * s, cur.y + 18 * s, 4.2f * s, (live.phase == 2 ? ui.c.ok : ui.c.textFaint).withA(ha));
    ui.text({cur.x + 30 * s, cur.y, 60 * s, 36 * s}, "Live", 13 * s, ui.c.text.withA(ha), AL_LEFT, 650);
  }
  // the sphere's icon appears as it becomes the orb
  if (orbness > 0.5f) ui.icon("live", cx, cy, 22 * s * (orbness - 0.5f) * 2, Color(1, 1, 1, (orbness - 0.5f) * 2), 1.9f);
  liveAnim(60);
}

void App::drawLive() {
  if (!live.open) return;
  float s = ui.s;
  Rect r = liveRect();
  if (!paletteOpen) ui.unblock();  // the pop-up's own widgets take the mouse (not under the command palette)
  bool mic = live.s && live.s->audio && live.s->audio->capturing();
  bool active = live.phase == 1 || live.speaking || live.interaction == "IN_PROGRESS" || live.awaiting || live.toolRunning || mic;
  if (active) liveAnim(mic || live.speaking ? 20 : 10);  // level meter and pulses; redrawn alone over the cached window (App::frame)
  if (live.miniT0 >= 0) {  // the panel <-> orb transition
    float u = float((ui.time - live.miniT0) / 0.45);
    if (u < 1) { drawLiveMorph(u); return; }
    live.miniT0 = -1;
    if (live.mini) { live.orb.ripples.push_back({ui.time, liveOrbRect().w * 0.45f, 0.5f}); live.orb.ripples.push_back({ui.time - 0.15, liveOrbRect().w * 0.45f, 0.35f}); }
  }
  if (live.mini) { drawLiveOrb(); return; }
  ui.shadow(r, 12 * s, 18 * s);
  ui.fill(r, ui.c.panel2, 12 * s);
  ui.stroke(r, ui.c.border, 12 * s);
  // ---- header: state dot, title, approval mode, close
  Rect hd{r.x, r.y, r.w, 36 * s};
  {
    Color dot = live.phase == 2 ? (live.error.empty() ? ui.c.ok : ui.c.warn) : live.phase == 1 ? ui.c.warn : ui.c.textFaint;
    float pulse = 1;
    if (live.phase == 1 || live.speaking || live.interaction == "IN_PROGRESS" || live.awaiting) pulse = 0.55f + 0.45f * float(std::sin(ui.time * 4.5));
    else if (mic) pulse = 0.6f + 0.4f * std::min(1.f, live.s->audio->inLevel() * 2.f);
    ui.circle(hd.x + 18 * s, hd.y + hd.h / 2, 4.2f * s, dot.withA(pulse));
    if (mic) ui.circle(hd.x + 18 * s, hd.y + hd.h / 2, (6.5f + 6 * live.s->audio->inLevel()) * s, dot.withA(0.25f), false, 1.2f * s);
    ui.text({hd.x + 30 * s, hd.y, 60 * s, hd.h}, "Live", 13 * s, ui.c.text, AL_LEFT, 650);
    string sub = live::shortModelName(live.model.empty() ? liveModel() : live.model);
    ui.text({hd.x + 62 * s, hd.y + 1 * s, hd.w - 62 * s - 124 * s, hd.h}, sub, 11 * s, ui.c.textFaint, AL_LEFT);
    // drag the panel by its header (the part without buttons) anywhere in the window
    {
      Rect grip{hd.x, hd.y, hd.w - 4 * 28 * s - 34 * s, hd.h};
      uint64_t gid = ui.id("live:drag");
      bool gh = false, gheld = false;
      ui.behave(gid, grip, &gh, &gheld);
      if (ui.in.pressed[0] && grip.has(ui.in.mx, ui.in.my) && ui.active == gid) { live.dragging = true; live.dragMoved = false; live.dragDx = ui.in.mx - r.x; live.dragDy = ui.in.my - r.y; }
      if (live.dragging && gheld) {
        float nx = ui.in.mx - live.dragDx, ny = ui.in.my - live.dragDy;
        if (live.dragMoved || std::fabs(nx - r.x) > 3 * s || std::fabs(ny - r.y) > 3 * s) { live.dragMoved = true; live.panelX = nx; live.panelY = ny; needFrame = true; }
        ui.cursor = "move";
      } else if (live.dragging && !ui.in.down[0]) {
        live.dragging = false;
        if (live.dragMoved) liveSavePositions();
      }
      if (gh && !live.dragging) ui.cursor = "move";
      if (gh) ui.tipFor(gid, "Drag to move the panel");
    }
    float bx = hd.r() - 30 * s;
    if (ui.iconButton({bx, hd.y + 5 * s, 26 * s, 26 * s}, "x", "Close and end the session")) { liveClose(); return; }
    bx -= 28 * s;
    if (ui.iconButton({bx, hd.y + 5 * s, 26 * s, 26 * s}, "shield", live.allowAll ? "Changes run without asking (click to ask first)" : "Asks before changing the project (click to allow all changes)", !live.allowAll)) {
      live.allowAll = !live.allowAll;
      if (live.allowAll && agent.live && agent.waitApproval) liveApprove(true, true);
    }
    bx -= 28 * s;
    if (ui.iconButton({bx, hd.y + 5 * s, 26 * s, 26 * s}, "orb", "Shrink to a small circle that listens and talks (right-click it to come back)")) liveSetMini(true);
    bx -= 28 * s;
    if (ui.iconButton({bx, hd.y + 5 * s, 26 * s, 26 * s}, "settings", "Live AI settings (key, model, voice, thinking)")) { setTab = 3; settingsWanted = true; }
  }
  // ---- status line
  Rect st{r.x + 16 * s, hd.b() - 2 * s, r.w - 32 * s, 16 * s};
  {
    bool err = live.phase == 0 && !live.error.empty();
    bool off = live.phase == 0;
    string full = liveStatus(), txt = truncate(full, mic || (live.phase == 0 && !live.error.empty()) ? 42 : 60);
    float tw = ui.textW(txt, 11 * s);
    ui.text(st, txt, 11 * s, err ? ui.c.danger : ui.c.textDim);
    if (off) {
      uint64_t sid = ui.id("live:status");
      bool sh = false;
      if (ui.behave(sid, {st.x, st.y, std::min(tw, st.w), st.h}, &sh)) {
        if (liveKey().empty()) { setTab = 3; settingsWanted = true; }
        else liveConnect(live.audioMode || live.talk);
      }
      if (sh) ui.line(st.x, st.b() - 2 * s, st.x + std::min(tw, st.w), st.b() - 2 * s, err ? ui.c.danger : ui.c.textDim);
      ui.tipFor(sid, (txt != full ? full + "\n" : string()) + (liveKey().empty() ? "Open Settings" : "Click to connect"));
    }
    if (err) {  // "Diagnose": two small REST requests explain a refused connection
      bool running = live.probe && !live.probe->done;
      string dl = running ? "Checking\xE2\x80\xA6" : "Diagnose";
      float dw = ui.textW(dl, 11 * s, 600);
      Rect dr{st.r() - dw, st.y, dw, st.h};
      uint64_t did = ui.id("live:diag");
      bool dh = false;
      if (ui.behave(did, dr, &dh) && !running) liveDiagnose();
      ui.text(dr, dl, 11 * s, running ? ui.c.textFaint : dh ? ui.c.accent.withA(0.8f) : ui.c.accent, AL_RIGHT, 600);
      ui.tipFor(did, "Checks whether the key sees the model and whether the project has any quota at all (one tiny text request), and shows the full error text");
      if (running) liveAnim(10);
    }
    // level bars while the microphone is on
    if (mic) {
      float lv = live.s->audio->micPaused() ? 0 : live.s->audio->inLevel();
      for (int i = 0; i < 5; i++) {
        float bh = (3 + 9 * std::max(0.f, std::min(1.f, lv * 1.6f - i * 0.18f))) * s;
        Rect b{st.r() - (5 - i) * 5 * s, st.y + st.h / 2 - bh / 2, 3 * s, bh};
        ui.fill(b, lv > i * 0.18f ? (live.hearing ? ui.c.accent : ui.c.ok) : ui.c.textFaint.withA(0.5f), 1.5f * s);  // accent: the detector hears speech
      }
      if (live.s->audio->micPaused()) ui.text({st.r() - 120 * s, st.y, 90 * s, st.h}, "mic paused", 10 * s, ui.c.textFaint, AL_RIGHT);
    } else if (live.tokens > 0 && !err) ui.text({st.r() - 90 * s, st.y, 90 * s, st.h}, fmtInt(live.tokens) + " tokens", 10 * s, ui.c.textFaint, AL_RIGHT);
  }
  // ---- approval card (above the composer)
  bool approval = agent.live && agent.waitApproval;
  float cardH = 0;
  string what, why;
  if (approval) {
    what = agentDescribe(agent.pending);
    cardH = 12 * s + ui.textWrap({0, 0, r.w - 56 * s, 200 * s}, what, 12 * s, ui.c.text, 550, false) + 38 * s;
  }
  // ---- composer
  Rect cp{r.x + 10 * s, r.b() - 46 * s, r.w - 20 * s, 34 * s};
  // ---- transcript
  Rect tr{r.x + 6 * s, st.b() + 6 * s, r.w - 12 * s, cp.y - 8 * s - cardH - (st.b() + 6 * s)};
  {
    if (live.log.version != live.seenVersion) { live.seenVersion = live.log.version; live.stick = true; live.dirty = true; }
    if (tr.has(ui.in.mx, ui.in.my) && ui.in.wheel > 0) live.stick = false;  // wheel up leaves the auto-follow; new content re-enables it
    if (live.stick) ui.scrollSet("live:log", std::max(0.f, live.logH - tr.h));
    ui.beginScroll("live:log", tr);
    float y = tr.y - ui.scrollY() + 4 * s, x = tr.x + 10 * s, w = tr.w - 24 * s;
    size_t n = live.log.entries.size();
    size_t start = n > 80 ? n - 80 : 0;
    if (n == 0) {
      string hint = live.phase == 0 && liveKey().empty() ? "The live assistant needs a Google Gemini API key." :
                    "Talk or type. The assistant sees your data and can build maps, switch views, name clusters, search OpenAlex, read papers and write reports.";
      y += ui.textWrap({x, y + 8 * s, w, 200 * s}, hint, 12 * s, ui.c.textFaint) + 16 * s;
    }
    for (size_t i = start; i < n; i++) {
      const live::Entry& e = live.log.entries[i];
      if (e.kind == live::Entry::User) {
        float th = ui.textWrap({x, y, w, 4000}, e.text, 12.5f * s, ui.c.text, 400, false);
        float tw = std::min(w, ui.textW(e.text, 12.5f * s) + 2 * s);
        Rect bub{x + w - tw - 20 * s, y, tw + 20 * s, th + 12 * s};
        ui.fill(bub, ui.c.accent.withA(e.open ? 0.10f : 0.16f), 9 * s);
        ui.textWrap({bub.x + 10 * s, bub.y + 6 * s, tw, th + 2 * s}, e.text, 12.5f * s, ui.c.text);
        y += bub.h + 6 * s;
      } else if (e.kind == live::Entry::Model) {
        float th = ui.richText({x, y + 4 * s, w, 4000}, e.text, 12.5f * s, ui.c.text, 400, true, 1.3f);
        y += th + 12 * s;
      } else if (e.kind == live::Entry::Tool) {
        Rect row{x - 4 * s, y, w + 8 * s, 0};
        float titleH = ui.textWrap({x + 20 * s, y + 5 * s, w - 24 * s, 200 * s}, e.text, 12 * s, ui.c.text, 500, false);
        float detH = e.detail.empty() ? 0 : ui.textWrap({x + 20 * s, y, w - 24 * s, 200 * s}, e.detail, 11 * s, ui.c.textDim, 400, false) + 2 * s;
        row.h = titleH + detH + 10 * s;
        ui.fill(row, ui.dark ? Color(1, 1, 1, 0.04f) : Color(0, 0, 0, 0.03f), 7 * s);
        Color ic = e.status == 1 ? ui.c.ok : e.status == 2 ? ui.c.danger : e.status == 3 || e.status == 5 ? ui.c.textFaint : e.status == 4 ? ui.c.warn : ui.c.accent;
        const char* icon = e.status == 1 ? "check" : e.status == 2 ? "warn" : e.status == 3 ? "x" : e.status == 5 ? "x" : e.status == 4 ? "shield" : "refresh";
        if (e.status == 0) {
          float a = 0.4f + 0.6f * float(0.5 + 0.5 * std::sin(ui.time * 5));
          ui.circle(x + 6 * s, y + 5 * s + 8 * s, 3.5f * s, ic.withA(a));
        } else ui.icon(icon, x + 6 * s, y + 5 * s + 8 * s, 12 * s, ic, 1.8f);
        ui.textWrap({x + 20 * s, y + 5 * s, w - 24 * s, titleH + 2 * s}, e.text, 12 * s, e.status == 3 || e.status == 5 ? ui.c.textDim : ui.c.text, 500);
        if (detH > 0) ui.textWrap({x + 20 * s, y + 5 * s + titleH + 1 * s, w - 24 * s, detH + 2 * s}, e.detail, 11 * s, ui.c.textDim);
        y += row.h + 6 * s;
      } else {
        float th = ui.textWrap({x + 4 * s, y + 2 * s, w - 8 * s, 300 * s}, e.text, 11 * s, ui.c.textFaint);
        y += th + 8 * s;
      }
    }
    if (!agentUndos.empty() && agentUndos.back().live && !live.toolRunning) {
      string lbl = "Undo: " + truncate(agentUndos.back().label, 34);
      Rect ub{x, y + 2 * s, std::min(w, ui.textW(lbl, 12.5f * s) + 24 * s), 22 * s};
      if (ui.button(ub, lbl, BTN_GHOST, "", !busy())) agentUndoChanges();
      ui.tip("Takes back the newest change only; click again for the one before it. " + plural(long(agentUndos.size()), "change") + " can be undone.");
      y += 30 * s;
    }
    float contentH = y - (tr.y - ui.scrollY()) + 6 * s;
    ui.endScroll(contentH);
    live.logH = contentH;
    if (live.stick) ui.scrollSet("live:log", std::max(0.f, contentH - tr.h));
  }
  // ---- approval card
  if (approval) {
    Rect card{r.x + 10 * s, cp.y - 8 * s - cardH, r.w - 20 * s, cardH};
    ui.fill(card, ui.c.warn.withA(ui.dark ? 0.12f : 0.09f), 9 * s);
    ui.stroke(card, ui.c.warn.withA(0.35f), 9 * s);
    ui.icon("shield", card.x + 16 * s, card.y + 16 * s, 14 * s, ui.c.warn, 1.8f);
    float th = ui.textWrap({card.x + 30 * s, card.y + 7 * s, card.w - 40 * s, 200 * s}, what, 12 * s, ui.c.text, 550);
    float by = card.y + 12 * s + th + 4 * s;
    float bw = 74 * s;
    if (ui.button({card.x + 10 * s, by, bw, 26 * s}, "Allow", BTN_PRIMARY)) liveApprove(true, false);
    else if (ui.button({card.x + 16 * s + bw, by, bw + 20 * s, 26 * s}, "Allow all", BTN_NORMAL)) liveApprove(true, true);
    else if (ui.button({card.r() - 10 * s - bw, by, bw, 26 * s}, "Decline", BTN_GHOST)) liveApprove(false, false);
  }
  // ---- composer: mic, input, send / stop speaking
  {
    Rect mb{cp.x, cp.y + 2 * s, 30 * s, 30 * s};
    bool hov = false;
    uint64_t mid = ui.id("live:mic");
    bool clicked = ui.behave(mid, mb, &hov);
    Color mc = mic ? ui.c.danger : hov ? ui.c.text : ui.c.textDim;
    if (mic) ui.fill(mb, ui.c.danger.withA(0.12f), 8 * s);
    else if (hov) ui.fill(mb, ui.c.hover, 8 * s);
    ui.icon("mic", mb.x + mb.w / 2, mb.y + mb.h / 2, 16 * s, mc, 1.7f);
    ui.tipFor(mid, mic ? "Stop talking (the session stays open)" : live.audioMode ? "Talk to the assistant" : "Start a live talking session (spoken replies)");
    if (clicked) liveToggleMic();
    float sendW = 30 * s;
    bool camera = live.phase == 2 && (hasMap() || hasCorpus());
    float camW = camera ? 30 * s : 0;
    if (camera) {  // show the assistant what is on the screen: one still picture, only when asked
      Rect cb{mb.r() + 4 * s, cp.y + 2 * s, 30 * s, 30 * s};
      if (ui.iconButton(cb, "camera", hasMap() ? "Show the assistant the map canvas (one picture, sent now)" : "Show the assistant the window (one picture, sent now)")) liveRequestPicture(hasMap(), true);
      camW += 4 * s;
    }
    Rect in{mb.r() + 6 * s + camW, cp.y + 2 * s, cp.w - mb.w - camW - sendW - 12 * s, 30 * s};
    bool submitted = false;
    live.inputId = ui.id("ti:live:input");
    if (live.focusWanted) { live.focusWanted = false; ui.focusText(live.inputId, live.input); }
    ui.textInput(in, "live:input", live.input, mic ? "Or type\xE2\x80\xA6" : "Message\xE2\x80\xA6", &submitted);
    Rect sb{in.r() + 6 * s, cp.y + 2 * s, sendW, 30 * s};
    if (live.speaking) {
      if (ui.iconButton(sb, "stop", "Stop speaking")) liveInterrupt();
    } else {
      bool can = !trim(live.input).empty();
      if (ui.iconButton(sb, "send", "Send  (Enter)", false, can)) submitted = true;
    }
    if (submitted && !trim(live.input).empty()) {
      string t = live.input;
      live.input.clear();
      liveSend(t);
      ui.focusText(live.inputId, live.input);
    }
  }
}

}  // namespace win
}  // namespace vs
