// Live AI: Gemini Live API (BidiGenerateContent over a WebSocket) — message building and parsing.
// Transport-free and audio-device-free, so everything here is testable on any host. The Windows layer
// (src/win/live.cpp) owns the socket, the microphone and the speaker and drives the application's tools.
//
// Protocol summary (https://ai.google.dev/api/live):
//   client → server: {"setup": …} once, then {"realtimeInput": {text|audio|audioStreamEnd}}, {"toolResponse": …}
//   server → client: {"setupComplete": {}}, {"serverContent": {modelTurn, inputTranscription, outputTranscription,
//                    turnComplete, interrupted, generationComplete, interactionStatus}}, {"toolCall": {functionCalls}},
//                    {"toolCallCancellation": {ids}}, {"goAway": {timeLeft}}, {"sessionResumptionUpdate": {…}},
//                    {"voiceActivity": {type: ACTIVITY_START|ACTIVITY_END, audioOffset}} (undocumented; the server's own
//                    speech detection, or our activityStart/activityEnd acknowledged), {} (keep-alive while it generates)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ai.h"
#include "json.h"

namespace vs {
namespace live {
using std::string;
using std::vector;

constexpr const char* kDefaultModel = "gemini-3.8-live-extended-thinking";
constexpr int kInputRate = 16000;   // microphone PCM sent to the model (16-bit mono)
constexpr int kOutputRate = 24000;  // PCM received from the model (16-bit mono)

const vector<string>& voices();          // prebuilt voice names
const vector<string>& thinkingLevels();  // "low", "medium", "high"
bool isThinkingModel(const string& model);
// Older Live models only run tools synchronously; 3.8 Live (and Extended Thinking, which requires it) run them in the background.
bool nonBlockingTools(const string& model);

struct Options {
  string model = kDefaultModel;
  bool audio = true;             // spoken replies (AUDIO, with transcripts) or TEXT
  string voice = "Puck";
  string thinking = "low";       // thinking level for Extended Thinking models ("" = default)
  string system;                 // system instruction
  string resumeHandle;           // handle of a previous session to continue
  bool transcripts = true;       // transcripts of the model's speech (outputAudioTranscription)
  // Whether to ask for inputAudioTranscription explicitly. The 3.8 Live models transcribe the user's speech without being
  // asked; asking for it made the server transcribe spoken turns and then never answer them in the end-to-end tests
  // (tests/live_probe.py, September 2026), so the field is only sent on request (setting liveInputTranscripts).
  bool inputTranscripts = false;
  bool search = true;            // Grounding with Google Search
  bool compression = true;       // sliding-window context compression (long sessions)
  // The application marks the start and the end of the user's speech itself (activityStart / activityEnd from its own
  // detector) instead of leaving it to the server's voice-activity detection, which on the 3.x Live models regularly
  // swallows an utterance and leaves the turn open: the user stops talking and nothing ever comes back.
  bool clientVad = true;
  int vadEndMs = 1100;           // silence that ends an utterance (the detector's endMs; silenceDurationMs for the server's detection)
  vector<ai::ToolSpec> tools;    // application tools offered as function declarations
};

string endpoint(const string& apiKey);
// Gemini needs a schema for function parameters; the application describes tool arguments by example
// ({"n": 15, "by": "weight|links"}), so the schema is derived: numbers → INTEGER/NUMBER, "a|b" → enum, …
Json schemaFromExample(const Json& example, const string& key = "", const string& tool = "");
Json functionDeclaration(const ai::ToolSpec& t, bool nonBlocking);
Json setupJson(const Options& o);
string setupMessage(const Options& o);

string textMessage(const string& text);                     // realtime text input
string audioMessage(const int16_t* pcm, size_t samples);    // 16 kHz PCM chunk
string audioStreamEndMessage();                             // microphone turned off
string activityStartMessage();                              // client-side speech detection: the user started talking
string activityEndMessage();                                // ... and stopped: the model answers now
string imageMessage(const string& bytes, const string& mime);  // one still picture (JPEG/PNG) as a realtime video frame
string clientTurnMessage(const string& text);               // a user turn appended to the history (interrupts the model)

struct FunctionResult {
  string id, name;
  bool ok = true;
  string result;       // tool output or error text
  string scheduling;   // "", "INTERRUPT", "WHEN_IDLE", "SILENT" (non-blocking tools)
};
string toolResponseMessage(const vector<FunctionResult>& results);

struct FunctionCall { string id, name; Json args; };
struct ServerMessage {
  bool setupComplete = false;
  vector<string> text;             // model text parts (TEXT sessions)
  vector<string> audio;            // raw PCM (16-bit little-endian, 24 kHz) decoded from inlineData
  string inputTranscript, interimInputTranscript, outputTranscript;
  bool turnComplete = false, interrupted = false, generationComplete = false, waitingForInput = false;
  string turnCompleteReason;       // undocumented, seen on empty turns: "NEED_MORE_INPUT", "RESPONSE_REJECTED"
  string interactionStatus;        // "", "IN_PROGRESS", "IDLE" (Extended Thinking)
  vector<FunctionCall> calls;
  vector<string> cancelIds;
  bool goAway = false;
  double goAwaySeconds = 0;
  bool resumptionUpdate = false, resumable = false;
  string resumeHandle;
  vector<string> searchQueries;    // Google Search queries the model ran (grounding metadata)
  vector<string> sources;          // titles of the grounding sources
  long long totalTokens = -1;
  string error;                    // an error object delivered as a message
  bool serverContent = false;      // the message carried a serverContent object (even an empty one: the model is at work)
  int activity = 0;                // voiceActivity echo: 1 = ACTIVITY_START, 2 = ACTIVITY_END (the server's detection, or ours acknowledged)
  double activityOffset = -1;      // ... at this offset into the audio stream (seconds)
  bool empty() const;
};
bool parseServerMessage(const string& json, ServerMessage& out, string* err = nullptr);

// ---- helpers
double durationSeconds(const string& proto);   // "12.500s" → 12.5
string closeReason(int code, const string& reason);  // WebSocket close code → readable text
string describeUpgradeFailure(int status, const string& body);  // HTTP status of a refused upgrade
float rmsLevel(const int16_t* pcm, size_t n);  // 0..1 loudness of a chunk (meters)

// Automatic gain: brings quiet speech up to a steady level before it is sent (the model hears every voice at about
// the same loudness). The gain follows speech only — rises slowly (about 6 dB/s), falls fast — with a soft limiter.
class Agc {
 public:
  bool enabled = true;
  float targetDb = -20;      // RMS aimed at during speech (dBFS)
  float maxGainDb = 24;
  void process(int16_t* pcm, size_t n, bool speech);  // adapt (when speech) and apply
  void apply(int16_t* pcm, size_t n) const;           // apply the current gain only (pre-roll chunks)
  float gainDb() const { return gainDb_; }
 private:
  float gainDb_ = 0;
};

// Speech detector run by the application on the microphone chunks (energy against an adaptive noise floor, with
// start and end hangovers). Start opens the user's activity, End closes it; the caller sends activityStart, the
// buffered pre-roll and the audio in between, then activityEnd.
class SpeechDetector {
 public:
  enum Event { None = 0, Start = 1, End = 2 };
  explicit SpeechDetector(int chunkMs = 40) : chunkMs_(chunkMs) {}
  Event feed(const int16_t* pcm, size_t n);  // one chunk of 16-bit PCM
  bool active() const { return active_; }
  float levelDb() const { return level_; }   // dBFS of the last chunk
  float floorDb() const { return floor_; }   // current noise-floor estimate
  int activeMs() const { return activeMs_; }
  void reset() { active_ = false; above_ = below_ = activeMs_ = 0; level_ = -100; floor_ = -60; }
  float minSpeechDb = -56;   // nothing quieter than this is speech (quiet voices on laptop microphones sit around -50 dBFS)
  float marginDb = 10;       // speech is this much louder than the noise floor
  int startMs = 160;         // speech needed before Start
  int endMs = 1100;          // non-speech needed before End (a short fragment waits 300 ms longer: it is usually the start of a sentence)
  int maxMs = 90000;         // longest activity (a guard against steady noise)
  bool strict = false;       // while the assistant speaks: a Start needs clearer, longer speech (8 dB more, 320 ms) — a deliberate interruption
 private:
  int chunkMs_;
  bool active_ = false;
  float level_ = -100, floor_ = -60;
  int above_ = 0, below_ = 0, activeMs_ = 0;
};
string shortModelName(const string& model);    // "gemini-3.8-live-extended-thinking" → "3.8 Live Thinking"
string truncateResult(const string& s, size_t maxChars = 12000);

// ---- transcript shown in the pop-up
struct Entry {
  enum Kind { User = 0, Model = 1, Tool = 2, Note = 3 } kind = Note;
  string text, detail;   // tool: title + result summary; note: text
  int status = 0;        // tool: 0 running, 1 done, 2 failed, 3 declined, 4 waiting for approval, 5 cancelled
  bool open = false;     // user/model: still receiving text
  string id;             // tool call id
};
class Transcript {
 public:
  vector<Entry> entries;
  size_t version = 0;    // bumps on every change (layout caches)
  void userFinal(const string& t);     // committed piece of the user's speech or a typed message
  void userInterim(const string& t);   // low-latency partial transcription (replaces the previous interim)
  void userBreak();                    // the user starts another utterance: the next piece opens a new entry
  void model(const string& t);         // model speech transcript / text, appended to the open model entry
  void turnComplete();                 // seals the open entries
  void interrupted();                  // the user barged in: seals the model entry with an ellipsis
  int tool(const string& id, const string& title);  // returns the entry index
  void toolStatus(const string& id, int status, const string& detail);
  void note(const string& t);
  void clear() { entries.clear(); interim_.clear(); version++; }
  string plain() const;                // the whole transcript as text (script logs, copy)
 private:
  Entry* openEntry(Entry::Kind k);
  void closeUser();
  string interim_, committed_;
};

}  // namespace live
}  // namespace vs
