// Live AI core: setup message, tool schemas from the agent's example arguments, client messages, server message
// parsing (audio, transcripts, tool calls, Extended Thinking interaction status, goAway, resumption), transcript model.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "ai.h"
#include "live.h"

using namespace vs;
using namespace vs::live;

static int fails = 0;
#define CHECK(c)                                                   \
  do {                                                             \
    if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
  } while (0)

static const Json& find(const Json& arr, const string& name) {
  static Json nul;
  for (size_t i = 0; i < arr.size(); i++) if (arr[i]["name"].str() == name) return arr[i];
  return nul;
}

int main() {
  // ---- endpoint and model helpers
  CHECK(endpoint("KEY") == "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=KEY");
  CHECK(isThinkingModel("gemini-3.8-live-extended-thinking"));
  CHECK(!isThinkingModel("gemini-3.8-live"));
  CHECK(nonBlockingTools("gemini-3.8-live-extended-thinking"));
  CHECK(nonBlockingTools("gemini-3.8-live"));
  CHECK(!nonBlockingTools("gemini-3.1-flash-live-preview"));
  CHECK(shortModelName("models/gemini-3.8-live-extended-thinking") == "Gemini 3.8 Live Thinking");
  CHECK(shortModelName("gemini-3.8-live") == "Gemini 3.8 Live");
  CHECK(std::string(kDefaultModel) == "gemini-3.8-live-extended-thinking");

  // ---- schemas from example arguments
  {
    Json s = schemaFromExample(Json::parse("{\"by\": \"weight|links|citations|year\", \"n\": 15, \"cluster\": 0}"), "", "top_items");
    CHECK(s["type"].str() == "OBJECT");
    CHECK(s["properties"]["by"]["type"].str() == "STRING");
    CHECK(s["properties"]["by"]["enum"].size() == 4);
    CHECK(s["properties"]["by"]["enum"][size_t(3)].str() == "year");
    CHECK(s["properties"]["n"]["type"].str() == "INTEGER");
    CHECK(s["properties"]["cluster"]["description"].str().find("0 = all") != string::npos);
    Json r = schemaFromExample(Json::parse("{\"resolution\": 1.0, \"min_cluster_size\": 1, \"method\": \"vosviewer|modularity\"}"), "", "recluster");
    CHECK(r["properties"]["resolution"]["type"].str() == "NUMBER");
    CHECK(r["properties"]["min_cluster_size"]["type"].str() == "INTEGER");
    Json n = schemaFromExample(Json::parse("{\"names\": [{\"cluster\": 1, \"name\": \"...\"}]}"), "", "name_clusters");
    CHECK(n["properties"]["names"]["type"].str() == "ARRAY");
    CHECK(n["properties"]["names"]["items"]["type"].str() == "OBJECT");
    CHECK(n["properties"]["names"]["items"]["properties"]["name"]["type"].str() == "STRING");
    CHECK(!n["properties"]["names"]["items"]["properties"]["name"].has("enum"));
    Json m = schemaFromExample(Json::parse("{\"unit\": \"keywords\", \"merges\": [{\"target\": \"...\", \"members\": [\"...\"]}], \"ignore\": [\"...\"]}"), "", "merge_terms");
    CHECK(m["properties"]["merges"]["items"]["properties"]["members"]["items"]["type"].str() == "STRING");
    CHECK(m["properties"]["ignore"]["items"]["type"].str() == "STRING");
    Json b = schemaFromExample(Json::parse("{\"semantic\": false}"), "", "search_openalex");
    CHECK(b["properties"]["semantic"]["type"].str() == "BOOLEAN");
  }
  // ---- every agent tool becomes a valid declaration
  {
    int withParams = 0;
    for (auto& t : ai::agentTools()) {
      Json f = functionDeclaration(t, true);
      CHECK(f["name"].str() == t.name);
      CHECK(!f["description"].str().empty());
      CHECK(f["behavior"].str() == "NON_BLOCKING");
      if (f.has("parameters")) {
        withParams++;
        CHECK(f["parameters"]["type"].str() == "OBJECT");
        CHECK(f["parameters"]["properties"].size() > 0);
      }
    }
    CHECK(withParams >= 15);
    Json gi = functionDeclaration(*ai::findTool("get_item"), false);
    CHECK(!gi.has("behavior"));
    CHECK(gi["parameters"]["required"].size() == 1 && gi["parameters"]["required"][size_t(0)].str() == "label");
    Json ov = functionDeclaration(*ai::findTool("get_overview"), true);
    CHECK(!ov.has("parameters"));  // "{}" → no parameters at all
    Json bm = functionDeclaration(*ai::findTool("build_map"), true);
    CHECK(bm["description"].str().find("approval") != string::npos);
    CHECK(!bm["parameters"].has("required"));
  }
  // ---- setup message: audio session with thinking, tools and search
  {
    Options o;
    o.system = "You are the live assistant.";
    o.tools = ai::agentTools();
    string s = setupMessage(o);
    Json j = Json::parse(s);
    const Json& su = j["setup"];
    CHECK(su["model"].str() == "models/gemini-3.8-live-extended-thinking");
    CHECK(su["generationConfig"]["responseModalities"][size_t(0)].str() == "AUDIO");
    CHECK(su["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"].str() == "Puck");
    CHECK(su["generationConfig"]["thinkingConfig"]["thinkingLevel"].str() == "LOW");
    CHECK(su["systemInstruction"]["parts"][size_t(0)]["text"].str() == "You are the live assistant.");
    CHECK(su["tools"].size() == 2);
    CHECK(su["tools"][size_t(0)]["functionDeclarations"].size() == ai::agentTools().size());
    CHECK(su["tools"][size_t(1)].has("googleSearch"));
    CHECK(find(su["tools"][size_t(0)]["functionDeclarations"], "build_map")["behavior"].str() == "NON_BLOCKING");
    {  // 1.9.9: project safety and the display tools are declared with the switches the guards look at
      const Json& decls = su["tools"][size_t(0)]["functionDeclarations"];
      CHECK(find(decls, "new_project")["parameters"]["properties"]["discard_unsaved"]["type"].str() == "BOOLEAN");
      CHECK(find(decls, "search_openalex")["parameters"]["properties"]["replace"]["type"].str() == "BOOLEAN");
      CHECK(find(decls, "load_data")["parameters"]["properties"]["replace"]["type"].str() == "BOOLEAN");
      CHECK(find(decls, "show_papers")["parameters"]["properties"]["sort"]["enum"].size() == 5);
      CHECK(find(decls, "show_chart")["parameters"]["required"][size_t(0)].str() == "chart");
      CHECK(ai::findTool("show_chart")->kind == ai::ToolKind::View && ai::findTool("new_project")->kind == ai::ToolKind::Change);
      // 1.10: compare mode and the Word export are declared with their enumerations
      CHECK(find(decls, "show_compare")["parameters"]["properties"]["kind"]["enum"].size() == 3);
      CHECK(find(decls, "show_compare")["parameters"]["properties"]["layout"]["enum"].size() == 3);
      CHECK(find(decls, "show_compare")["parameters"]["properties"]["min_a"]["type"].str() == "NUMBER");
      CHECK(find(decls, "export_report")["parameters"]["properties"]["format"]["enum"].size() == 5);  // 1.11: pdf, docx, html, both, all
      CHECK(ai::findTool("show_compare")->kind == ai::ToolKind::View);
    }
    CHECK(!su.has("inputAudioTranscription") && su.has("outputAudioTranscription"));  // 1.9.6: input transcription only on request
    CHECK(su["contextWindowCompression"].has("slidingWindow"));
    CHECK(su.has("sessionResumption") && !su["sessionResumption"].has("handle"));
    // exactly one top-level field
    CHECK(j.size() == 1);
  }
  // ---- setup message: text session on the plain live model, resumed
  {
    Options o;
    o.model = "models/gemini-3.8-live";
    o.audio = false;
    o.thinking = "high";
    o.search = false;
    o.compression = false;
    o.resumeHandle = "H123";
    Json su = setupJson(o)["setup"];
    CHECK(su["model"].str() == "models/gemini-3.8-live");
    CHECK(su["generationConfig"]["responseModalities"][size_t(0)].str() == "TEXT");
    CHECK(!su["generationConfig"].has("speechConfig"));
    CHECK(!su["generationConfig"].has("thinkingConfig"));  // not a thinking model
    CHECK(!su.has("tools"));
    CHECK(!su.has("inputAudioTranscription") && !su.has("outputAudioTranscription"));
    CHECK(!su.has("realtimeInputConfig"));  // text session: no speech detection at all
    o.inputTranscripts = true;
    CHECK(setupJson(o)["setup"].has("inputAudioTranscription"));
    CHECK(setupMessage(o).find("googleSearch") == string::npos);  // search off: the free tier refuses a setup that asks for grounding
    CHECK(!su.has("contextWindowCompression"));
    CHECK(su["sessionResumption"]["handle"].str() == "H123");
    CHECK(!su.has("systemInstruction"));
    Options t; t.model = "gemini-3.1-flash-live-preview"; t.tools = ai::agentTools();
    CHECK(!find(setupJson(t)["setup"]["tools"][size_t(0)]["functionDeclarations"], "get_item").has("behavior"));
  }
  // ---- client messages
  {
    Json t = Json::parse(textMessage("Build a \"keyword\" map\nplease"));
    CHECK(t["realtimeInput"]["text"].str() == "Build a \"keyword\" map\nplease");
    int16_t pcm[4] = {0, 1, -1, 0x1234};
    Json a = Json::parse(audioMessage(pcm, 4));
    CHECK(a["realtimeInput"]["audio"]["mimeType"].str() == "audio/pcm;rate=16000");
    string raw = base64Decode(a["realtimeInput"]["audio"]["data"].str());
    CHECK(raw.size() == 8);
    CHECK((unsigned char)raw[2] == 1 && (unsigned char)raw[3] == 0);
    CHECK((unsigned char)raw[4] == 0xff && (unsigned char)raw[5] == 0xff);
    CHECK((unsigned char)raw[6] == 0x34 && (unsigned char)raw[7] == 0x12);
    CHECK(Json::parse(audioStreamEndMessage())["realtimeInput"]["audioStreamEnd"].boolean());
    Json c = Json::parse(clientTurnMessage("note"));
    CHECK(c["clientContent"]["turnComplete"].boolean() && c["clientContent"]["turns"][size_t(0)]["role"].str() == "user");
    vector<FunctionResult> res = {{"call_1", "get_overview", true, "140 records", ""}, {"call_2", "build_map", false, "No records", "WHEN_IDLE"}};
    Json r = Json::parse(toolResponseMessage(res));
    const Json& fr = r["toolResponse"]["functionResponses"];
    CHECK(fr.size() == 2);
    CHECK(fr[size_t(0)]["id"].str() == "call_1" && fr[size_t(0)]["name"].str() == "get_overview");
    CHECK(fr[size_t(0)]["response"]["output"].str() == "140 records");
    CHECK(!fr[size_t(0)]["response"].has("scheduling"));
    CHECK(fr[size_t(1)]["response"]["error"].str() == "No records");
    CHECK(fr[size_t(1)]["response"]["scheduling"].str() == "WHEN_IDLE");
  }
  // ---- server messages
  {
    ServerMessage m;
    CHECK(parseServerMessage("{\"setupComplete\": {}}", m) && m.setupComplete && !m.turnComplete);
    string pcm = base64(string("\x01\x00\x02\x00", 4));
    string audioMsg = "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"" + pcm +
                      "\"}},{\"thought\":true,\"text\":\"hidden\"},{\"text\":\"Hello\"}]},\"outputTranscription\":{\"text\":\"Hel\"}},\"usageMetadata\":{\"totalTokenCount\":321}}";
    CHECK(parseServerMessage(audioMsg, m));
    CHECK(m.audio.size() == 1 && m.audio[0].size() == 4 && m.audio[0][2] == 2);
    CHECK(m.text.size() == 1 && m.text[0] == "Hello");
    CHECK(m.outputTranscript == "Hel");
    CHECK(m.totalTokens == 321);
    CHECK(!m.setupComplete && !m.empty());
    CHECK(parseServerMessage("{\"serverContent\":{\"inputTranscription\":{\"text\":\"build a map\"},\"interimInputTranscription\":{\"text\":\"build a m\"}}}", m));
    CHECK(m.inputTranscript == "build a map" && m.interimInputTranscript == "build a m");
    // Extended Thinking: a spoken filler ends its utterance but the interaction goes on
    CHECK(parseServerMessage("{\"serverContent\":{\"turnComplete\":true,\"interactionStatus\":\"IN_PROGRESS\"}}", m));
    CHECK(m.turnComplete && m.interactionStatus == "IN_PROGRESS");
    CHECK(parseServerMessage("{\"toolCall\":{\"functionCalls\":[{\"id\":\"call_123\",\"name\":\"build_map\",\"args\":{\"unit\":\"all_keywords\",\"min\":5}},{\"id\":\"c2\",\"name\":\"get_overview\"}]},\"interactionStatus\":\"IN_PROGRESS\"}", m));
    CHECK(m.calls.size() == 2);
    CHECK(m.calls[0].id == "call_123" && m.calls[0].name == "build_map" && m.calls[0].args["min"].integer() == 5 && m.calls[0].args["unit"].str() == "all_keywords");
    CHECK(m.calls[1].args.t == Json::Obj && m.calls[1].args.size() == 0);
    CHECK(m.interactionStatus == "IN_PROGRESS");
    CHECK(parseServerMessage("{\"serverContent\":{\"turnComplete\":true,\"interactionStatus\":\"IDLE\",\"generationComplete\":true}}", m));
    CHECK(m.turnComplete && m.generationComplete && m.interactionStatus == "IDLE");
    CHECK(parseServerMessage("{\"serverContent\":{\"interrupted\":true}}", m) && m.interrupted);
    CHECK(parseServerMessage("{\"toolCallCancellation\":{\"ids\":[\"a\",\"b\"]}}", m) && m.cancelIds.size() == 2 && m.cancelIds[1] == "b");
    CHECK(parseServerMessage("{\"goAway\":{\"timeLeft\":\"9.500s\"}}", m) && m.goAway && std::fabs(m.goAwaySeconds - 9.5) < 1e-9);
    CHECK(parseServerMessage("{\"goAway\":{\"timeLeft\":{\"seconds\":4,\"nanos\":500000000}}}", m) && std::fabs(m.goAwaySeconds - 4.5) < 1e-9);
    CHECK(parseServerMessage("{\"sessionResumptionUpdate\":{\"newHandle\":\"H9\",\"resumable\":true}}", m) && m.resumptionUpdate && m.resumable && m.resumeHandle == "H9");
    CHECK(parseServerMessage("{\"sessionResumptionUpdate\":{\"resumable\":false}}", m) && m.resumptionUpdate && !m.resumable && m.resumeHandle.empty());
    CHECK(parseServerMessage("{\"serverContent\":{\"groundingMetadata\":{\"webSearchQueries\":[\"vosviewer clustering\"],\"groundingChunks\":[{\"web\":{\"uri\":\"u\",\"title\":\"Site\"}}]}}}", m));
    CHECK(m.searchQueries.size() == 1 && m.sources.size() == 1 && m.sources[0] == "Site");
    CHECK(parseServerMessage("{\"error\":{\"code\":400,\"message\":\"Invalid argument\",\"status\":\"INVALID_ARGUMENT\"}}", m) && m.error == "Invalid argument");
    string err;
    CHECK(!parseServerMessage("nonsense", m, &err) && !err.empty());
    CHECK(parseServerMessage("{}", m) && m.empty());
  }
  // ---- helpers
  {
    CHECK(closeReason(1000, "").find("ended") != string::npos);
    CHECK(closeReason(1008, "API key not valid").find("API key not valid") != string::npos);
    CHECK(closeReason(1006, "") == "The connection was lost.");
    CHECK(describeUpgradeFailure(404, "{\"error\":{\"message\":\"models/x is not found\"}}").find("is not found") != string::npos);
    CHECK(describeUpgradeFailure(403, "").find("API key") != string::npos);
    CHECK(std::fabs(durationSeconds("12.5s") - 12.5) < 1e-9 && durationSeconds("") == 0);
    int16_t silent[160] = {0};
    CHECK(rmsLevel(silent, 160) == 0);
    int16_t loud[160];
    for (int i = 0; i < 160; i++) loud[i] = int16_t(i % 2 ? 20000 : -20000);
    CHECK(rmsLevel(loud, 160) > 0.8f);
    CHECK(truncateResult(string(20, 'a'), 10).find("10 more") != string::npos);
    CHECK(truncateResult("short", 10) == "short");
    CHECK(voices().size() >= 6 && thinkingLevels().size() == 3);
  }
  // ---- transcript
  {
    Transcript t;
    t.userInterim("build a");
    CHECK(t.entries.size() == 1 && t.entries[0].kind == Entry::User && t.entries[0].text == "build a" && t.entries[0].open);
    t.userInterim("build a keyword");
    CHECK(t.entries.size() == 1 && t.entries[0].text == "build a keyword");
    t.userFinal("build a keyword map");
    CHECK(t.entries.size() == 1 && t.entries[0].text == "build a keyword map");
    t.userFinal(" please");
    CHECK(t.entries[0].text == "build a keyword map please");
    t.model("Sure, ");
    CHECK(t.entries.size() == 2 && !t.entries[0].open && t.entries[1].kind == Entry::Model && t.entries[1].open);
    int k = t.tool("c1", "Build map");
    CHECK(k == 2 && t.entries[2].status == 0);
    t.model("building it now.");
    CHECK(t.entries.size() == 4);  // the model entry after the tool row is a new one
    t.toolStatus("c1", 1, "120 items");
    CHECK(t.entries[2].status == 1 && t.entries[2].detail == "120 items");
    t.turnComplete();
    CHECK(!t.entries[3].open && t.entries[3].text == "building it now.");
    t.model("The map has 5 clusters");
    t.interrupted();
    CHECK(t.entries.back().text.find("\xE2\x80\xA6") != string::npos && !t.entries.back().open);
    t.userFinal("");
    t.turnComplete();  // empty user entry disappears
    CHECK(t.entries.back().kind == Entry::Model);
    t.note("Reconnected");
    string p = t.plain();
    CHECK(p.find("You: build a keyword map please") != string::npos && p.find("[tool 1] Build map : 120 items") != string::npos && p.find("(Reconnected)") != string::npos);
    size_t v = t.version;
    t.clear();
    CHECK(t.entries.empty() && t.version > v);
  }
  {  // 1.9.4: the application's own speech detection and the activity messages
    Options o;
    o.audio = true;
    Json j = setupJson(o);
    CHECK(j["setup"]["realtimeInputConfig"]["automaticActivityDetection"]["disabled"].boolean(false));
    o.clientVad = false;
    o.vadEndMs = 1200;
    j = setupJson(o);
    {  // 1.9.6: the server's detection is configured explicitly (with the defaults the 3.8 models did not react at all)
      const Json& aad = j["setup"]["realtimeInputConfig"]["automaticActivityDetection"];
      CHECK(!aad["disabled"].boolean(true) && aad["startOfSpeechSensitivity"].str() == "START_SENSITIVITY_HIGH");
      CHECK(aad["endOfSpeechSensitivity"].str() == "END_SENSITIVITY_LOW" && aad["silenceDurationMs"].integer(0) == 1200 && aad["prefixPaddingMs"].integer(0) == 200);
      o.vadEndMs = 50;
      CHECK(setupJson(o)["setup"]["realtimeInputConfig"]["automaticActivityDetection"]["silenceDurationMs"].integer(0) == 300);
    }
    {  // 1.9.6: voiceActivity echoes, empty serverContent (a sign of life), keep-alive {}
      ServerMessage v;
      CHECK(parseServerMessage("{\"voiceActivity\":{\"type\":\"ACTIVITY_END\",\"audioOffset\":\"3.080s\"}}", v));
      CHECK(v.activity == 2 && std::fabs(v.activityOffset - 3.08) < 1e-6 && !v.empty() && !v.serverContent);
      CHECK(parseServerMessage("{\"voiceActivity\":{\"type\":\"ACTIVITY_START\",\"audioOffset\":\"0s\"}}", v));
      CHECK(v.activity == 1 && v.activityOffset == 0);
      CHECK(parseServerMessage("{\"serverContent\":{}}", v));
      CHECK(v.serverContent && v.activity == 0 && !v.empty() && v.text.empty() && v.audio.empty() && !v.turnComplete);
      CHECK(parseServerMessage("{}", v));
      CHECK(v.empty() && !v.serverContent);
      CHECK(parseServerMessage("{\"serverContent\":{\"inputTranscription\":{\"text\":\"Hello.\"}}}", v));
      CHECK(v.serverContent && v.inputTranscript == "Hello.");
    }
    Json a = Json::parse(activityStartMessage()), b = Json::parse(activityEndMessage());
    CHECK(a["realtimeInput"].has("activityStart") && b["realtimeInput"].has("activityEnd") && !a["realtimeInput"].has("activityEnd"));
    ServerMessage m;
    CHECK(parseServerMessage("{\"serverContent\":{\"turnComplete\":true,\"turnCompleteReason\":\"NEED_MORE_INPUT\",\"waitingForInput\":true}}", m));
    CHECK(m.turnComplete && m.turnCompleteReason == "NEED_MORE_INPUT" && m.waitingForInput);
    // detector: 40 ms chunks of 16 kHz PCM; quiet noise, a 1.5 s "utterance", quiet again
    SpeechDetector d(40);
    std::vector<int16_t> chunk(640);
    unsigned seed = 12345;
    auto fill = [&](double amp) {  // white noise at the given peak amplitude (0..1)
      for (auto& v : chunk) { seed = seed * 1103515245u + 12345u; double r = (double((seed >> 8) & 0xffff) / 32768.0 - 1.0); v = int16_t(r * amp * 32767); }
    };
    int startAt = -1, endAt = -1, events = 0;
    for (int i = 0; i < 150; i++) {  // 6 s
      bool talk = i >= 25 && i < 70;   // 1.0 s .. 2.8 s
      fill(talk ? 0.08 : 0.002);       // about -25 dBFS against -57 dBFS
      SpeechDetector::Event e = d.feed(chunk.data(), chunk.size());
      if (e == SpeechDetector::Start) { startAt = i; events++; }
      if (e == SpeechDetector::End) { endAt = i; events++; }
    }
    CHECK(events == 2);
    CHECK(startAt >= 27 && startAt <= 30);            // 160 ms of speech
    CHECK(endAt >= 70 + 27 && endAt <= 70 + 30);      // 1100 ms of silence after the utterance
    CHECK(!d.active() && d.floorDb() < -45 && d.floorDb() > -80);
    {  // a short fragment (under 1.5 s) waits 300 ms longer: it is usually the start of a sentence
      SpeechDetector d2(40);
      int s2 = -1, e2 = -1;
      for (int i = 0; i < 150; i++) {
        fill(i >= 25 && i < 45 ? 0.08 : 0.002);  // 0.8 s of speech
        SpeechDetector::Event e = d2.feed(chunk.data(), chunk.size());
        if (e == SpeechDetector::Start) s2 = i;
        if (e == SpeechDetector::End) e2 = i;
      }
      CHECK(s2 >= 27 && s2 <= 30);
      CHECK(e2 >= 45 + 34 && e2 <= 45 + 37);          // 1400 ms
    }
    {  // strict (the assistant is speaking): the same speech needs 320 ms and 8 dB more; a quiet voice does not start it
      SpeechDetector d3(40);
      d3.strict = true;
      int s3 = -1, n3 = 0;
      for (int i = 0; i < 100; i++) {
        fill(i >= 25 && i < 70 ? 0.08 : 0.002);  // loud and clear: about -25 dBFS
        SpeechDetector::Event e = d3.feed(chunk.data(), chunk.size());
        if (e == SpeechDetector::Start) { s3 = i; n3++; }
      }
      CHECK(n3 == 1 && s3 >= 31 && s3 <= 34);          // 320 ms
      SpeechDetector d4(40);
      d4.strict = true;
      int n4 = 0;
      for (int i = 0; i < 100; i++) { fill(i >= 25 && i < 70 ? 0.004 : 0.002); if (d4.feed(chunk.data(), chunk.size()) == SpeechDetector::Start) n4++; }  // only 6 dB over the floor
      CHECK(n4 == 0);
    }
    {  // automatic gain: a quiet voice (-40 dBFS) is brought up towards -20 dBFS over a few seconds; loud audio is left alone
      Agc a;
      std::vector<int16_t> c2(640);
      auto rmsDb = [&](const std::vector<int16_t>& v) { double acc = 0; for (auto x : v) acc += double(x) / 32768 * double(x) / 32768; return 20 * std::log10(std::sqrt(acc / double(v.size()))); };
      double before = 0, after = 0;
      for (int i = 0; i < 200; i++) {  // 8 s of quiet speech
        for (size_t k = 0; k < c2.size(); k++) c2[k] = int16_t(0.01 * 32767 * std::sin(0.3 * double(k)));  // about -43 dBFS
        if (i == 0) before = rmsDb(c2);
        a.process(c2.data(), c2.size(), true);
        after = rmsDb(c2);
      }
      CHECK(a.gainDb() > 15 && a.gainDb() <= 24);
      CHECK(after > before + 15);
      Agc b;
      for (int i = 0; i < 50; i++) { for (size_t k = 0; k < c2.size(); k++) c2[k] = int16_t(0.3 * 32767 * std::sin(0.3 * double(k))); b.process(c2.data(), c2.size(), true); }
      CHECK(b.gainDb() < 0.01f);
      Agc c;
      c.enabled = false;
      for (size_t k = 0; k < c2.size(); k++) c2[k] = 100;
      c.process(c2.data(), c2.size(), true);
      CHECK(c2[0] == 100);
    }
    // a click of 80 ms is not speech; a very quiet murmur below the minimum level neither
    int ev2 = 0;
    for (int i = 0; i < 60; i++) { fill(i >= 10 && i < 12 ? 0.3 : 0.002); if (d.feed(chunk.data(), chunk.size()) != SpeechDetector::None) ev2++; }
    for (int i = 0; i < 60; i++) { fill(i >= 10 && i < 40 ? 0.003 : 0.002); if (d.feed(chunk.data(), chunk.size()) != SpeechDetector::None) ev2++; }
    CHECK(ev2 == 0);
    // digital silence (muted microphone) does not open an activity and does not wreck the floor
    std::fill(chunk.begin(), chunk.end(), int16_t(0));
    for (int i = 0; i < 100; i++) CHECK(d.feed(chunk.data(), chunk.size()) == SpeechDetector::None);
    CHECK(d.floorDb() >= -80.01f);
    // transcript: a second utterance in the same open turn gets a space; userBreak starts a new entry
    Transcript t;
    t.userFinal("Why are you not responding?");
    t.userFinal("Why are you not responding?");
    CHECK(t.entries.size() == 1 && t.entries[0].text == "Why are you not responding? Why are you not responding?");
    t.userFinal(", please");
    CHECK(t.entries[0].text == "Why are you not responding? Why are you not responding?, please");
    t.userBreak();
    t.userFinal("Hello");
    CHECK(t.entries.size() == 2 && !t.entries[0].open && t.entries[1].text == "Hello" && t.entries[1].open);
  }
  if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
  std::printf("live_test: all checks passed\n");
  return 0;
}
