// Live AI: Gemini Live API messages (see live.h)
#include "live.h"

#include <algorithm>

#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

namespace vs {
namespace live {

namespace {

const std::map<string, string>& paramDocs() {
  static const std::map<string, string> d = {
      {"n", "Number of entries to return"},
      {"top", "Top items to list per cluster"},
      {"by", "Sort order"},
      {"label", "Item label (case-insensitive; part of a label matches the heaviest item containing it)"},
      {"runs", "Number of clustering runs"},
      {"a_from", "First year of period A (0 = automatic)"},
      {"a_to", "Last year of period A (0 = automatic)"},
      {"b_from", "First year of period B (0 = automatic)"},
      {"b_to", "Last year of period B (0 = automatic)"},
      {"unit", "Unit of analysis"},
      {"view", "Map view"},
      {"page", "Page of the left panel"},
      {"type", "Analysis type"},
      {"counting", "Counting method"},
      {"min", "Minimum number of occurrences of an item"},
      {"max_items", "Maximum number of items on the map"},
      {"from_year", "First publication year (0 = no limit)"},
      {"to_year", "Last publication year (0 = no limit)"},
      {"resolution", "Clustering resolution (higher = more, smaller clusters)"},
      {"min_cluster_size", "Minimum cluster size"},
      {"method", "Clustering method"},
      {"names", "Cluster names to write"},
      {"name", "New cluster name"},
      {"merges", "Terms to merge"},
      {"target", "Preferred term"},
      {"members", "Variants merged into the target"},
      {"ignore", "Terms to leave out of the maps"},
      {"semantic", "Search by meaning (a one or two sentence description) instead of keywords"},
      {"max", "Maximum number of works (50 to 2000)"},
      {"append", "Add to the current records instead of replacing them"},
      {"sort", "Order of the papers"},
      {"skip", "Papers to skip (paging)"},
      {"chart", "Chart id (see the tool description)"},
      {"title", "Title"},
      {"caption", "Caption: what the figure shows"},
      {"subtitle", "Subtitle"},
      {"text", "Section text in Markdown"},
  };
  return d;
}

string paramDoc(const string& key, const string& tool) {
  if (key == "cluster") return tool == "top_items" ? "Cluster number as shown on the map (0 = all clusters)" : "Cluster number as shown on the map (from 1)";
  if (key == "query") {
    if (tool == "read_papers") return "Words that must all occur in the title, abstract or keywords (empty = all papers)";
    if (tool == "search_items") return "Text contained in the item label";
    if (tool == "search_openalex") return "2 to 6 precise search terms; alternative searches separated by ;. With semantic true: a one or two sentence description";
    return "Search text";
  }
  auto it = paramDocs().find(key);
  return it == paramDocs().end() ? string() : it->second;
}

const std::map<string, vector<string>>& requiredArgs() {
  static const std::map<string, vector<string>> r = {
      {"get_item", {"label"}},        {"search_items", {"query"}},  {"show_view", {"view"}},       {"focus_item", {"label"}},
      {"focus_cluster", {"cluster"}}, {"open_page", {"page"}},      {"name_clusters", {"names"}},  {"merge_terms", {"unit"}},
      {"search_openalex", {"query"}}, {"add_chart", {"chart"}},     {"add_section", {"text"}},     {"scan_variants", {"unit"}},
      {"show_chart", {"chart"}},       {"open_pdf", {"paper"}},       {"set_reading", {"paper"}},    {"paper_notes", {"paper"}},
  };
  return r;
}

string upperAscii(const string& s) {
  string o = s;
  for (auto& ch : o) if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
  return o;
}

}  // namespace

const vector<string>& voices() {
  static const vector<string> v = {"Puck", "Charon", "Kore", "Fenrir", "Aoede", "Leda", "Orus", "Zephyr"};
  return v;
}

const vector<string>& thinkingLevels() {
  static const vector<string> v = {"low", "medium", "high"};
  return v;
}

bool isThinkingModel(const string& model) { return lower(model).find("thinking") != string::npos; }

bool nonBlockingTools(const string& model) {
  string m = lower(model);
  if (isThinkingModel(m)) return true;
  return m.find("3.1") == string::npos && m.find("2.0") == string::npos;
}

string endpoint(const string& apiKey) {
  return "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" + apiKey;
}

Json schemaFromExample(const Json& ex, const string& key, const string& tool) {
  Json s = Json::object();
  switch (ex.t) {
    case Json::Bool: s.set("type", "BOOLEAN"); break;
    case Json::Num: {
      bool integral = std::fabs(ex.n - std::round(ex.n)) < 1e-9 && key != "resolution" && key != "min_a" && key != "min_b";
      s.set("type", integral ? "INTEGER" : "NUMBER");
      break;
    }
    case Json::Str: {
      s.set("type", "STRING");
      if (ex.s.find('|') != string::npos) {
        Json en = Json::array();
        for (auto& p : split(ex.s, '|')) { string v = trim(p); if (!v.empty()) en.push(v); }
        if (en.size() > 1) s.set("enum", en);
      }
      break;
    }
    case Json::Arr: {
      s.set("type", "ARRAY");
      s.set("items", ex.a.empty() ? schemaFromExample(Json("..."), key, tool) : schemaFromExample(ex.a[0], key, tool));
      break;
    }
    case Json::Obj: {
      s.set("type", "OBJECT");
      Json props = Json::object();
      for (auto& kv : ex.o) props.set(kv.first, schemaFromExample(kv.second, kv.first, tool));
      s.set("properties", props);
      break;
    }
    case Json::Null: s.set("type", "STRING"); break;
  }
  if (ex.t != Json::Obj) {
    string d = paramDoc(key, tool);
    if (!d.empty()) s.set("description", d);
  }
  return s;
}

Json functionDeclaration(const ai::ToolSpec& t, bool nonBlocking) {
  Json f = Json::object();
  f.set("name", t.name);
  string desc = t.desc;
  if (t.kind == ai::ToolKind::Change) desc += " (Changes the project; may need the user's approval.)";
  f.set("description", desc);
  if (nonBlocking) f.set("behavior", "NON_BLOCKING");
  Json ex = Json::parse(t.args);
  if (ex.t == Json::Obj && !ex.o.empty()) {
    Json params = schemaFromExample(ex, "", t.name);
    auto it = requiredArgs().find(t.name);
    if (it != requiredArgs().end()) {
      Json req = Json::array();
      for (auto& k : it->second) if (ex.has(k)) req.push(k);
      if (req.size()) params.set("required", req);
    }
    f.set("parameters", params);
  }
  return f;
}

Json setupJson(const Options& o) {
  Json setup = Json::object();
  string m = o.model.empty() ? string(kDefaultModel) : o.model;
  setup.set("model", m.rfind("models/", 0) == 0 ? m : "models/" + m);
  Json gen = Json::object();
  { Json mods = Json::array(); mods.push(o.audio ? "AUDIO" : "TEXT"); gen.set("responseModalities", mods); }
  if (o.audio && !o.voice.empty()) {
    Json pv = Json::object(); pv.set("voiceName", o.voice);
    Json vc = Json::object(); vc.set("prebuiltVoiceConfig", pv);
    Json sc = Json::object(); sc.set("voiceConfig", vc);
    gen.set("speechConfig", sc);
  }
  if (isThinkingModel(m) && !o.thinking.empty()) {
    Json tc = Json::object(); tc.set("thinkingLevel", upperAscii(o.thinking));
    gen.set("thinkingConfig", tc);
  }
  setup.set("generationConfig", gen);
  if (!o.system.empty()) {
    Json part = Json::object(); part.set("text", o.system);
    Json parts = Json::array(); parts.push(part);
    Json si = Json::object(); si.set("parts", parts);
    setup.set("systemInstruction", si);
  }
  Json tools = Json::array();
  if (!o.tools.empty()) {
    Json decls = Json::array();
    bool nb = nonBlockingTools(m);
    for (auto& t : o.tools) decls.push(functionDeclaration(t, nb));
    Json ft = Json::object(); ft.set("functionDeclarations", decls);
    tools.push(ft);
  }
  if (o.search) { Json gs = Json::object(); gs.set("googleSearch", Json::object()); tools.push(gs); }
  if (tools.size()) setup.set("tools", tools);
  if (o.transcripts) {
    if (o.inputTranscripts) setup.set("inputAudioTranscription", Json::object());  // see Options::inputTranscripts
    if (o.audio) setup.set("outputAudioTranscription", Json::object());
  }
  if (o.compression) { Json cw = Json::object(); cw.set("slidingWindow", Json::object()); setup.set("contextWindowCompression", cw); }
  if (o.audio) {
    Json aad = Json::object();
    if (o.clientVad) {  // the client sends activityStart / activityEnd; the server's own detection is off
      aad.set("disabled", true);
    } else {
      // Spelled out rather than left to the defaults: with no realtimeInputConfig at all the 3.8 Live models did not
      // react to the test utterances (no activity, no transcript, no answer); with these settings they did.
      aad.set("disabled", false);
      aad.set("startOfSpeechSensitivity", "START_SENSITIVITY_HIGH");
      aad.set("endOfSpeechSensitivity", "END_SENSITIVITY_LOW");
      aad.set("prefixPaddingMs", 200);
      aad.set("silenceDurationMs", std::max(300, std::min(3000, o.vadEndMs)));
    }
    Json ric = Json::object(); ric.set("automaticActivityDetection", aad);
    setup.set("realtimeInputConfig", ric);
  }
  Json sr = Json::object();
  if (!o.resumeHandle.empty()) sr.set("handle", o.resumeHandle);
  setup.set("sessionResumption", sr);
  Json msg = Json::object();
  msg.set("setup", setup);
  return msg;
}

string setupMessage(const Options& o) { return setupJson(o).dump(); }

string textMessage(const string& text) {
  Json ri = Json::object(); ri.set("text", text);
  Json m = Json::object(); m.set("realtimeInput", ri);
  return m.dump();
}

string audioMessage(const int16_t* pcm, size_t samples) {
  string raw;
  raw.resize(samples * 2);
  for (size_t i = 0; i < samples; i++) {  // little-endian regardless of the host
    uint16_t v = uint16_t(pcm[i]);
    raw[2 * i] = char(v & 0xff);
    raw[2 * i + 1] = char(v >> 8);
  }
  Json blob = Json::object();
  blob.set("data", base64(raw));
  blob.set("mimeType", "audio/pcm;rate=" + std::to_string(kInputRate));
  Json ri = Json::object(); ri.set("audio", blob);
  Json m = Json::object(); m.set("realtimeInput", ri);
  return m.dump();
}

string imageMessage(const string& bytes, const string& mime) {
  Json blob = Json::object();
  blob.set("data", base64(bytes));
  blob.set("mimeType", mime);
  Json ri = Json::object(); ri.set("video", blob);
  Json m = Json::object(); m.set("realtimeInput", ri);
  return m.dump();
}

string audioStreamEndMessage() {
  Json ri = Json::object(); ri.set("audioStreamEnd", true);
  Json m = Json::object(); m.set("realtimeInput", ri);
  return m.dump();
}

string activityStartMessage() {
  Json ri = Json::object(); ri.set("activityStart", Json::object());
  Json m = Json::object(); m.set("realtimeInput", ri);
  return m.dump();
}

string activityEndMessage() {
  Json ri = Json::object(); ri.set("activityEnd", Json::object());
  Json m = Json::object(); m.set("realtimeInput", ri);
  return m.dump();
}

string clientTurnMessage(const string& text) {
  Json part = Json::object(); part.set("text", text);
  Json parts = Json::array(); parts.push(part);
  Json turn = Json::object(); turn.set("role", "user"); turn.set("parts", parts);
  Json turns = Json::array(); turns.push(turn);
  Json cc = Json::object(); cc.set("turns", turns); cc.set("turnComplete", true);
  Json m = Json::object(); m.set("clientContent", cc);
  return m.dump();
}

string toolResponseMessage(const vector<FunctionResult>& results) {
  Json arr = Json::array();
  for (auto& r : results) {
    Json fr = Json::object();
    fr.set("id", r.id);
    fr.set("name", r.name);
    Json resp = Json::object();
    resp.set(r.ok ? "output" : "error", r.result);
    if (!r.scheduling.empty()) resp.set("scheduling", r.scheduling);
    fr.set("response", resp);
    arr.push(fr);
  }
  Json tr = Json::object(); tr.set("functionResponses", arr);
  Json m = Json::object(); m.set("toolResponse", tr);
  return m.dump();
}

bool ServerMessage::empty() const {
  return !setupComplete && text.empty() && audio.empty() && inputTranscript.empty() && interimInputTranscript.empty() && outputTranscript.empty() &&
         !turnComplete && !interrupted && !generationComplete && !waitingForInput && interactionStatus.empty() && calls.empty() && cancelIds.empty() && !goAway &&
         !resumptionUpdate && searchQueries.empty() && totalTokens < 0 && error.empty() && !serverContent && activity == 0;
}

double durationSeconds(const string& proto) {
  string t = trim(proto);
  if (t.empty()) return 0;
  if (!t.empty() && t.back() == 's') t.pop_back();
  return toDouble(t, 0);
}

bool parseServerMessage(const string& json, ServerMessage& out, string* err) {
  out = ServerMessage();
  string perr;
  Json j = Json::parse(json, &perr);
  if (j.t != Json::Obj) { if (err) *err = perr.empty() ? "not a JSON object" : perr; return false; }
  if (j.has("setupComplete")) out.setupComplete = true;
  if (j.has("interactionStatus")) out.interactionStatus = j["interactionStatus"].str();
  if (j.has("voiceActivity")) {
    string t = j["voiceActivity"]["type"].str();
    out.activity = t == "ACTIVITY_START" ? 1 : t == "ACTIVITY_END" ? 2 : 0;
    const Json& off = j["voiceActivity"]["audioOffset"];
    if (off.t == Json::Str) out.activityOffset = durationSeconds(off.str());
    else if (off.t == Json::Obj) out.activityOffset = off["seconds"].num(0) + off["nanos"].num(0) / 1e9;
  }
  if (j.has("serverContent")) {
    out.serverContent = true;
    const Json& sc = j["serverContent"];
    out.turnComplete = sc["turnComplete"].boolean(false);
    out.interrupted = sc["interrupted"].boolean(false);
    out.generationComplete = sc["generationComplete"].boolean(false);
    out.waitingForInput = sc["waitingForInput"].boolean(false);
    out.turnCompleteReason = sc["turnCompleteReason"].str();
    if (sc.has("interactionStatus")) out.interactionStatus = sc["interactionStatus"].str();
    if (sc.has("inputTranscription")) out.inputTranscript = sc["inputTranscription"]["text"].str();
    if (sc.has("interimInputTranscription")) out.interimInputTranscript = sc["interimInputTranscription"]["text"].str();
    if (sc.has("outputTranscription")) out.outputTranscript = sc["outputTranscription"]["text"].str();
    const Json& parts = sc["modelTurn"]["parts"];
    for (size_t i = 0; i < parts.size(); i++) {
      const Json& p = parts[i];
      if (p["thought"].boolean(false)) continue;
      if (p.has("text") && !p["text"].str().empty()) out.text.push_back(p["text"].str());
      if (p.has("inlineData")) {
        string mime = lower(p["inlineData"]["mimeType"].str());
        if (mime.empty() || mime.rfind("audio/", 0) == 0) out.audio.push_back(base64Decode(p["inlineData"]["data"].str()));
      }
    }
    const Json& gm = sc["groundingMetadata"];
    for (size_t i = 0; i < gm["webSearchQueries"].size(); i++) out.searchQueries.push_back(gm["webSearchQueries"][i].str());
    for (size_t i = 0; i < gm["groundingChunks"].size(); i++) {
      string t = gm["groundingChunks"][i]["web"]["title"].str();
      if (!t.empty()) out.sources.push_back(t);
    }
  }
  if (j.has("toolCall")) {
    const Json& calls = j["toolCall"]["functionCalls"];
    for (size_t i = 0; i < calls.size(); i++) {
      FunctionCall fc;
      fc.id = calls[i]["id"].str();
      fc.name = calls[i]["name"].str();
      fc.args = calls[i]["args"];
      if (fc.args.t != Json::Obj) fc.args = Json::object();
      out.calls.push_back(fc);
    }
  }
  if (j.has("toolCallCancellation")) {
    const Json& ids = j["toolCallCancellation"]["ids"];
    for (size_t i = 0; i < ids.size(); i++) out.cancelIds.push_back(ids[i].str());
  }
  if (j.has("goAway")) {
    out.goAway = true;
    const Json& tl = j["goAway"]["timeLeft"];
    out.goAwaySeconds = tl.t == Json::Obj ? tl["seconds"].num(0) + tl["nanos"].num(0) / 1e9 : durationSeconds(tl.str());
  }
  if (j.has("sessionResumptionUpdate")) {
    out.resumptionUpdate = true;
    out.resumeHandle = j["sessionResumptionUpdate"]["newHandle"].str();
    out.resumable = j["sessionResumptionUpdate"]["resumable"].boolean(false);
  }
  if (j.has("usageMetadata")) out.totalTokens = (long long)j["usageMetadata"]["totalTokenCount"].num(-1);
  if (j.has("error")) {
    const Json& e = j["error"];
    out.error = e.t == Json::Str ? e.str() : e["message"].str(e["status"].str("error"));
  }
  return true;
}

string closeReason(int code, const string& reason) {
  string r = trim(reason);
  string base;
  switch (code) {
    case 1000: base = "The session ended."; break;
    case 1001: base = "The server ended the session."; break;
    case 1006: base = "The connection was lost."; break;
    case 1007: base = "The server rejected a message"; break;
    case 1008: base = "The request was refused (check the API key, the model name and your quota)"; break;
    case 1011: base = "The server reported an internal error"; break;
    case 1013: base = "The service is busy; try again in a moment"; break;
    default: base = code > 0 ? "The connection closed (code " + std::to_string(code) + ")" : "The connection closed"; break;
  }
  if (!r.empty()) return base + (base.back() == '.' ? " " : ": ") + r;
  return base.back() == '.' ? base : base + ".";
}

string describeUpgradeFailure(int status, const string& body) {
  string msg;
  Json j = Json::parse(body);
  if (j.t == Json::Obj) msg = j["error"]["message"].str();
  string base;
  switch (status) {
    case 400: base = "The request was invalid"; break;
    case 401: case 403: base = "The API key was not accepted"; break;
    case 404: base = "The model was not found (check the model name in Settings)"; break;
    case 429: base = "The quota or rate limit was exceeded"; break;
    default: base = status >= 500 ? "The service is unavailable" : status > 0 ? "The connection was refused (HTTP " + std::to_string(status) + ")" : "The connection failed"; break;
  }
  return msg.empty() ? base + "." : base + ": " + truncate(msg, 200);
}

SpeechDetector::Event SpeechDetector::feed(const int16_t* pcm, size_t n) {
  double acc = 0;
  for (size_t i = 0; i < n; i++) { double v = pcm[i] / 32768.0; acc += v * v; }
  double rms = n ? std::sqrt(acc / double(n)) : 0;
  level_ = float(20 * std::log10(std::max(rms, 1e-5)));  // -100 dB for digital silence
  // the noise floor follows drops at once and rises slowly: 1 dB/s between utterances, a quarter of that during one
  if (level_ < floor_) floor_ = std::max(-80.f, level_);
  else floor_ = std::min(-20.f, floor_ + (active_ ? 0.25f : 1.f) * float(chunkMs_) / 1000.f);
  bool hard = strict && !active_;
  bool speech = level_ > std::max(minSpeechDb + (hard ? 6.f : 0.f), floor_ + marginDb + (hard ? 8.f : 0.f));
  Event ev = None;
  if (!active_) {
    above_ = speech ? above_ + 1 : 0;
    if (above_ * chunkMs_ >= startMs + (hard ? 160 : 0)) { active_ = true; ev = Start; activeMs_ = 0; above_ = below_ = 0; }
  } else {
    activeMs_ += chunkMs_;
    below_ = speech ? 0 : below_ + 1;
    int spoken = activeMs_ - below_ * chunkMs_;         // speech before the current pause
    int need = spoken < 1500 ? endMs + 300 : endMs;      // a short fragment is usually the start of a sentence: wait a little longer
    if (below_ * chunkMs_ >= need || activeMs_ >= maxMs) { active_ = false; ev = End; above_ = below_ = 0; }
  }
  return ev;
}

void Agc::process(int16_t* pcm, size_t n, bool speech) {
  if (!enabled || !pcm || n == 0) return;
  if (speech) {
    double acc = 0;
    for (size_t i = 0; i < n; i++) { double v = pcm[i] / 32768.0; acc += v * v; }
    float lvl = float(20 * std::log10(std::max(std::sqrt(acc / double(n)), 1e-5)));
    if (lvl > -70) {
      float want = std::max(0.f, std::min(maxGainDb, targetDb - lvl));
      if (want < gainDb_) gainDb_ = std::max(want, gainDb_ - 3.f);     // down fast: 3 dB per 40 ms chunk
      else gainDb_ = std::min(want, gainDb_ + 0.25f);                  // up slowly: about 6 dB per second
    }
  }
  apply(pcm, n);
}

void Agc::apply(int16_t* pcm, size_t n) const {
  if (!enabled || gainDb_ < 0.05f || !pcm) return;
  float g = std::pow(10.f, gainDb_ / 20.f);
  for (size_t i = 0; i < n; i++) {
    float v = pcm[i] * g;
    float a = std::fabs(v);
    if (a > 26000.f) { a = 26000.f + (a - 26000.f) * 0.2f; v = v < 0 ? -a : a; }  // soft knee instead of clipping
    pcm[i] = int16_t(std::max(-32767.f, std::min(32767.f, v)));
  }
}

float rmsLevel(const int16_t* pcm, size_t n) {
  if (!pcm || n == 0) return 0;
  double acc = 0;
  for (size_t i = 0; i < n; i++) { double v = pcm[i] / 32768.0; acc += v * v; }
  double rms = std::sqrt(acc / double(n));
  // perceptual-ish scale: -50 dB → 0, 0 dB → 1
  double db = 20 * std::log10(std::max(rms, 1e-6));
  return float(std::max(0.0, std::min(1.0, (db + 50) / 50)));
}

string shortModelName(const string& model) {
  string m = model;
  if (m.rfind("models/", 0) == 0) m = m.substr(7);
  bool th = isThinkingModel(m);
  string ver;
  size_t p = m.find("gemini-");
  if (p != string::npos) {
    size_t a = p + 7, b = a;
    while (b < m.size() && (isdigit((unsigned char)m[b]) || m[b] == '.')) b++;
    ver = m.substr(a, b - a);
  }
  if (ver.empty()) return m;
  return "Gemini " + ver + " Live" + (th ? " Thinking" : "");
}

string truncateResult(const string& s, size_t maxChars) {
  if (s.size() <= maxChars) return s;
  return s.substr(0, maxChars) + "\n… (" + std::to_string(s.size() - maxChars) + " more characters were left out)";
}

// ---------------------------------------------------------------- transcript
Entry* Transcript::openEntry(Entry::Kind k) {
  if (!entries.empty() && entries.back().kind == k && entries.back().open) return &entries.back();
  return nullptr;
}

void Transcript::closeUser() {
  if (Entry* u = openEntry(Entry::User)) {
    u->text = trim(u->text);
    u->open = false;
    if (u->text.empty()) entries.pop_back();
  }
  interim_.clear();
  committed_.clear();
}

void Transcript::userBreak() {
  closeUser();
  version++;
}

void Transcript::userFinal(const string& t) {
  if (Entry* m = openEntry(Entry::Model)) { m->open = false; if (trim(m->text).empty()) entries.pop_back(); }
  Entry* u = openEntry(Entry::User);
  if (!u) { entries.push_back(Entry()); u = &entries.back(); u->kind = Entry::User; u->open = true; committed_.clear(); }
  // pieces arrive without a separator when one utterance follows another in the same turn ("talking?What is")
  if (!committed_.empty() && !t.empty() && strchr(".?!", committed_.back()) && !isspace(static_cast<unsigned char>(t[0])) && isupper(static_cast<unsigned char>(t[0]))) committed_ += ' ';
  committed_ += t;
  interim_.clear();
  u->text = committed_;
  version++;
}

void Transcript::userInterim(const string& t) {
  if (Entry* m = openEntry(Entry::Model)) { m->open = false; if (trim(m->text).empty()) entries.pop_back(); }
  Entry* u = openEntry(Entry::User);
  if (!u) { entries.push_back(Entry()); u = &entries.back(); u->kind = Entry::User; u->open = true; committed_.clear(); }
  interim_ = t;
  u->text = committed_ + interim_;
  version++;
}

void Transcript::model(const string& t) {
  if (t.empty()) return;
  Entry* m = openEntry(Entry::Model);
  if (!m) {
    closeUser();
    entries.push_back(Entry());
    m = &entries.back();
    m->kind = Entry::Model;
    m->open = true;
  }
  m->text += t;
  version++;
}

void Transcript::turnComplete() {
  closeUser();
  if (Entry* m = openEntry(Entry::Model)) { m->text = trim(m->text); m->open = false; if (m->text.empty()) entries.pop_back(); }
  version++;
}

void Transcript::interrupted() {
  if (Entry* m = openEntry(Entry::Model)) {
    m->text = trim(m->text);
    if (m->text.empty()) entries.pop_back();
    else { m->text += " \xE2\x80\xA6"; m->open = false; }
  }
  version++;
}

int Transcript::tool(const string& id, const string& title) {
  Entry e;
  e.kind = Entry::Tool;
  e.id = id;
  e.text = title;
  e.status = 0;
  entries.push_back(e);
  version++;
  return int(entries.size()) - 1;
}

void Transcript::toolStatus(const string& id, int status, const string& detail) {
  for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    if (it->kind == Entry::Tool && it->id == id) { it->status = status; if (!detail.empty()) it->detail = detail; version++; return; }
}

void Transcript::note(const string& t) {
  Entry e;
  e.kind = Entry::Note;
  e.text = t;
  entries.push_back(e);
  version++;
}

string Transcript::plain() const {
  string o;
  for (auto& e : entries) {
    switch (e.kind) {
      case Entry::User: o += "You: " + e.text + "\n"; break;
      case Entry::Model: o += "AI: " + e.text + "\n"; break;
      case Entry::Tool: o += "[tool " + std::to_string(e.status) + "] " + e.text + (e.detail.empty() ? string() : " : " + e.detail) + "\n"; break;
      case Entry::Note: o += "(" + e.text + ")\n"; break;
    }
  }
  return o;
}

}  // namespace live
}  // namespace vs
