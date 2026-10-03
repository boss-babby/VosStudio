// AI core: request building, SSE parsing (split at every byte), error texts, structured replies, context, persistence.
#include <cassert>
#include <cstdio>
#include <string>

#include "ai.h"
#include "project.h"

using namespace vs;
using namespace vs::ai;

static int fails = 0;
#define CHECK(c)                                                   \
  do {                                                             \
    if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } \
  } while (0)

static string feedSplit(Provider p, const string& s, size_t step, string* err = nullptr) {
  StreamParser sp(p);
  for (size_t i = 0; i < s.size(); i += step) sp.feed(s.data() + i, std::min(step, s.size() - i));
  sp.finish();
  if (err) *err = sp.error;
  return sp.text;
}

int main() {
  vector<Msg> msgs = {{"user", "Hello \"map\""}, {"assistant", "Hi"}, {"user", "Name the clusters"}};
  // ---- OpenAI
  {
    Config c; c.provider = Provider::OpenAI; c.key = "sk-test";
    HttpReq r = buildChat(c, "SYS", msgs, true);
    CHECK(r.url == "https://api.openai.com/v1/chat/completions");
    Json b = Json::parse(r.body);
    CHECK(b["model"].str() == "gpt-5-mini");
    CHECK(b["messages"].size() == 4);
    CHECK(b["messages"][size_t(0)]["role"].str() == "developer");
    CHECK(b["messages"][size_t(1)]["content"].str() == "Hello \"map\"");
    CHECK(b["stream"].boolean());
    CHECK(!b.has("temperature"));
    bool auth = false;
    for (auto& h : r.headers) if (h.first == "Authorization" && h.second == "Bearer sk-test") auth = true;
    CHECK(auth);
    string sse = "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
                 "data: {\"choices\":[{\"delta\":{\"content\":\"Clusters \"}}]}\n\n"
                 "data: {\"choices\":[{\"delta\":{\"content\":\"\\u00e9 ok\"},\"finish_reason\":\"stop\"}]}\n\n"
                 "data: [DONE]\n\n";
    for (size_t step : {size_t(1), size_t(7), size_t(1000)}) CHECK(feedSplit(Provider::OpenAI, sse, step) == "Clusters \xC3\xA9 ok");
    CHECK(parseChat(c, "{\"choices\":[{\"message\":{\"content\":\"hey\"}}]}", nullptr) == "hey");
    string e;
    feedSplit(Provider::OpenAI, "{\"error\":{\"message\":\"Incorrect API key\",\"type\":\"invalid_request_error\"}}", 5, &e);
    CHECK(e == "Incorrect API key");
    string d = describeError(c, 401, "{\"error\":{\"message\":\"Incorrect API key provided\"}}", "");
    CHECK(d.find("rejected the API key") != string::npos && d.find("Incorrect") != string::npos);
    CHECK(describeError(c, 404, "", "").find("gpt-5-mini") != string::npos);
    CHECK(describeError(c, 0, "", "timeout").find("Could not reach OpenAI") != string::npos);
    vector<string> ml = parseModelList(c, "{\"data\":[{\"id\":\"gpt-5\"},{\"id\":\"text-embedding-3-small\"},{\"id\":\"whisper-1\"},{\"id\":\"o4-mini\"},{\"id\":\"gpt-5-mini\"}]}");
    CHECK(ml.size() == 3 && ml[0] == "gpt-5");
  }
  // ---- Gemini
  {
    Config c; c.provider = Provider::Gemini; c.key = "AIza"; c.model = "gemini-2.5-flash";
    HttpReq r = buildChat(c, "SYS", msgs, true);
    CHECK(r.url == "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    Json b = Json::parse(r.body);
    CHECK(b["systemInstruction"]["parts"][size_t(0)]["text"].str() == "SYS");
    CHECK(b["contents"].size() == 3);
    CHECK(b["contents"][size_t(1)]["role"].str() == "model");
    CHECK(!b["generationConfig"].has("temperature"));
    string sse = "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"thinking\", \"thought\": true}, {\"text\": \"Alpha \"}],\"role\": \"model\"}}]}\r\n\r\n"
                 "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"Beta\"}],\"role\": \"model\"},\"finishReason\": \"STOP\"}]}\r\n\r\n";
    for (size_t step : {size_t(1), size_t(13), size_t(1000)}) CHECK(feedSplit(Provider::Gemini, sse, step) == "Alpha Beta");
    CHECK(buildChat(c, "", msgs, false).url.find(":generateContent") != string::npos);
    vector<string> ml = parseModelList(c, "{\"models\":[{\"name\":\"models/gemini-2.5-flash\",\"supportedGenerationMethods\":[\"generateContent\"]},"
                                          "{\"name\":\"models/text-embedding-004\",\"supportedGenerationMethods\":[\"embedContent\"]}]}");
    CHECK(ml.size() == 1 && ml[0] == "gemini-2.5-flash");
    string e;
    feedSplit(Provider::Gemini, "[{\n  \"error\": {\n    \"code\": 400,\n    \"message\": \"API key not valid.\"\n  }\n}\n]", 3, &e);
    CHECK(describeError(c, 400, "[{\"error\":{\"code\":400,\"message\":\"API key not valid.\"}}]", "").find("API key not valid") != string::npos);
  }
  // ---- Anthropic
  {
    Config c; c.provider = Provider::Anthropic; c.key = "sk-ant";
    HttpReq r = buildChat(c, "SYS", msgs, true);
    CHECK(r.url == "https://api.anthropic.com/v1/messages");
    Json b = Json::parse(r.body);
    CHECK(b["system"].str() == "SYS" && b["max_tokens"].integer() == 4096 && b["messages"].size() == 3);
    bool ver = false, key = false;
    for (auto& h : r.headers) { if (h.first == "anthropic-version") ver = true; if (h.first == "x-api-key" && h.second == "sk-ant") key = true; }
    CHECK(ver && key);
    string sse = "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m\"}}\n\n"
                 "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                 "event: ping\ndata: {\"type\": \"ping\"}\n\n"
                 "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}\n\n"
                 "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\" world\"}}\n\n"
                 "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
                 "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    for (size_t step : {size_t(1), size_t(9), size_t(5000)}) CHECK(feedSplit(Provider::Anthropic, sse, step) == "Hello world");
    string e;
    feedSplit(Provider::Anthropic, "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n", 4, &e);
    CHECK(e == "Overloaded");
    CHECK(parseChat(c, "{\"content\":[{\"type\":\"text\",\"text\":\"A\"},{\"type\":\"text\",\"text\":\"B\"}]}", nullptr) == "AB");
  }
  // ---- OpenAI-compatible
  {
    Config c; c.provider = Provider::Compatible; c.baseUrl = "http://localhost:8765/v1/"; c.model = "llama3.1";
    CHECK(c.ready());
    HttpReq r = buildChat(c, "SYS", msgs, true);
    CHECK(r.url == "http://localhost:8765/v1/chat/completions");
    Json b = Json::parse(r.body);
    CHECK(b["messages"][size_t(0)]["role"].str() == "system" && b["max_tokens"].integer() == 4096);
    for (auto& h : r.headers) CHECK(h.first != "Authorization");
    CHECK(buildModelList(c).url == "http://localhost:8765/v1/models");
    Config none; none.provider = Provider::Compatible;
    CHECK(!none.ready());
    Config nokey; nokey.provider = Provider::OpenAI;
    CHECK(!nokey.ready());
  }
  // ---- structured replies
  {
    auto names = parseClusterNames("Here you go:\n```json\n[{\"cluster\": 1, \"name\": \"AI Feedback\", \"summary\": \"s1\"},\n {\"cluster\": 2, \"name\": \"Learning Analytics\"}, {\"cluster\": 0, \"name\": \"bad\"}]\n```");
    CHECK(names.size() == 2 && names[0].cluster == 0 && names[0].name == "AI Feedback" && names[1].cluster == 1 && names[0].summary == "s1");
    CHECK(parseClusterNames("no json").empty());
    SearchPlan sp;
    CHECK(parseSearchPlan("```json\n{\"keywords\": \"llm; \\\"learning analytics\\\"\", \"semantic\": [\"a\", \"b\", \"\"], \"fromYear\": 2018, \"toYear\": 0}\n```", sp));
    CHECK(sp.keywords == "llm; \"learning analytics\"" && sp.semantic.size() == 2 && sp.fromYear == 2018 && sp.toYear == 0);
    CHECK(!parseSearchPlan("sorry", sp));
    CHECK(taskPrompt(Task::NameClusters, "").find("JSON array") != string::npos);
    CHECK(taskPrompt(Task::Search, "LLM tutoring").find("LLM tutoring") != string::npos);
    CHECK(tasks().size() + 1 == size_t(Task::Count));  // every task except Agent (not a one-click task) has a card
    CHECK(taskInfo(Task::Agent).task == Task::Agent && string(taskInfo(Task::Clean).id) == "clean");
  }
  // ---- context from the sample project, persistence
  {
    Project P;
    P.addSample(false);
    string cc = corpusContext(P);
    CHECK(cc.find("records") != string::npos && cc.find("Most frequent author keywords") != string::npos);
    P.build();
    string mc = mapContext(P, 8);
    CHECK(mc.find("Cluster 1") != string::npos && mc.find("modularity") != string::npos);
    CHECK(!topDocsContext(P, 5).empty());
    CHECK(itemContext(P, 0).find("Strongest links") != string::npos);
    CHECK(clusterContext(P, 0).find("Items by weight") != string::npos);
    CHECK(documentContext(P, 0).find("Title:") != string::npos);
    ContextOpts o; o.selectedItem = 1; o.document = 2;
    string all = buildContext(P, o);
    CHECK(all.find("## Data set") != string::npos && all.find("## Current map") != string::npos && all.find("## Selected item") != string::npos && all.find("## Document") != string::npos);
    std::printf("context: %zu chars (corpus %zu, map %zu, trends %zu)\n", all.size(), cc.size(), mc.size(), trendContext(P).size());
    vector<Turn> t = {{"user", "Q \xE2\x80\x9Cquoted\xE2\x80\x9D", "chat", "2026-09-27 10:00"}, {"assistant", "## A\n- b", "", ""}};
    P.assistant = turnsToJson(t);
    string path = "/tmp/ai_test.vosproj", err;
    CHECK(P.save(path, &err));
    Project Q;
    CHECK(Q.open(path, &err));
    auto t2 = turnsFromJson(Q.assistant);
    CHECK(t2.size() == 2 && t2[0].text == t[0].text && t2[0].task == "chat" && t2[1].role == "assistant" && t2[1].text == "## A\n- b");
    Project E;
    E.addSample(false);
    CHECK(E.save(path, &err));
    Project F;
    F.assistant = turnsToJson(t);
    CHECK(F.open(path, &err) && F.assistant.size() == 0);  // opening a project without a conversation clears the old one
  }
  std::printf(fails ? "ai_test: %d FAILED\n" : "ai_test: all passed\n", fails);
  return fails ? 1 : 0;
}
