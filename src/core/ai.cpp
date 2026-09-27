#include "ai.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "project.h"

namespace vs {
namespace ai {

// ================================================================= providers
const char* providerName(Provider p) {
  switch (p) {
    case Provider::OpenAI: return "OpenAI";
    case Provider::Gemini: return "Google Gemini";
    case Provider::Anthropic: return "Anthropic Claude";
    case Provider::Compatible: return "OpenAI-compatible";
  }
  return "";
}

const char* providerHint(Provider p) {
  switch (p) {
    case Provider::OpenAI: return "Create a key at platform.openai.com/api-keys";
    case Provider::Gemini: return "Create a key at aistudio.google.com/apikey";
    case Provider::Anthropic: return "Create a key at console.anthropic.com";
    case Provider::Compatible: return "OpenRouter, Groq, DeepSeek, Mistral, Ollama, LM Studio and other /v1/chat/completions servers";
  }
  return "";
}

string defaultModel(Provider p) {
  switch (p) {
    case Provider::OpenAI: return "gpt-5-mini";
    case Provider::Gemini: return "gemini-3.8-flash";
    case Provider::Anthropic: return "claude-haiku-4-5";
    case Provider::Compatible: return "";
  }
  return "";
}

vector<string> suggestedModels(Provider p) {
  switch (p) {
    case Provider::OpenAI: return {"gpt-5-mini", "gpt-5", "gpt-5.4-mini", "gpt-4.1-mini"};
    case Provider::Gemini: return {"gemini-3.8-flash", "gemini-2.5-flash", "gemini-2.5-pro"};
    case Provider::Anthropic: return {"claude-haiku-4-5", "claude-sonnet-4-5", "claude-opus-4-1"};
    case Provider::Compatible: return {"llama3.1", "qwen2.5", "deepseek-chat", "mistral-large-latest"};
  }
  return {};
}

string Config::effectiveModel() const { return trim(model).empty() ? defaultModel(provider) : trim(model); }

static string stripSlash(string s) {
  s = trim(s);
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

static string compatBase(const Config& c) {
  string b = stripSlash(c.baseUrl);
  if (b.empty()) b = "http://localhost:11434/v1";
  return b;
}

static string urlEncode(const string& s) {
  static const char* hx = "0123456789ABCDEF";
  string o;
  for (unsigned char ch : s) {
    if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += char(ch);
    else { o += '%'; o += hx[ch >> 4]; o += hx[ch & 15]; }
  }
  return o;
}

HttpReq buildChat(const Config& c, const string& system, const vector<Msg>& msgs, bool stream) {
  HttpReq r;
  const string model = c.effectiveModel();
  r.headers.push_back({"Content-Type", "application/json"});
  if (c.provider == Provider::Gemini) {
    r.url = "https://generativelanguage.googleapis.com/v1beta/models/" + urlEncode(model) + (stream ? ":streamGenerateContent?alt=sse" : ":generateContent");
    r.headers.push_back({"x-goog-api-key", c.key});
    Json b = Json::object();
    if (!system.empty()) {
      Json si = Json::object(), parts = Json::array(), pt = Json::object();
      pt.set("text", system);
      parts.push(pt);
      si.set("parts", parts);
      b.set("systemInstruction", si);
    }
    Json contents = Json::array();
    for (auto& m : msgs) {
      Json one = Json::object(), parts = Json::array(), pt = Json::object();
      one.set("role", m.role == "assistant" ? "model" : "user");
      pt.set("text", m.text);
      parts.push(pt);
      one.set("parts", parts);
      contents.push(one);
    }
    b.set("contents", contents);
    Json gc = Json::object();
    gc.set("maxOutputTokens", c.maxTokens);
    b.set("generationConfig", gc);
    r.body = b.dump();
    return r;
  }
  if (c.provider == Provider::Anthropic) {
    r.url = "https://api.anthropic.com/v1/messages";
    r.headers.push_back({"x-api-key", c.key});
    r.headers.push_back({"anthropic-version", "2023-06-01"});
    Json b = Json::object();
    b.set("model", model);
    b.set("max_tokens", c.maxTokens);
    if (!system.empty()) b.set("system", system);
    Json arr = Json::array();
    for (auto& m : msgs) {
      Json one = Json::object();
      one.set("role", m.role == "assistant" ? "assistant" : "user");
      one.set("content", m.text);
      arr.push(one);
    }
    b.set("messages", arr);
    if (stream) b.set("stream", true);
    r.body = b.dump();
    return r;
  }
  // OpenAI and compatible servers: /chat/completions
  r.url = (c.provider == Provider::OpenAI ? string("https://api.openai.com/v1") : compatBase(c)) + "/chat/completions";
  if (!c.key.empty()) r.headers.push_back({"Authorization", "Bearer " + c.key});
  if (c.provider == Provider::Compatible) { r.headers.push_back({"HTTP-Referer", "https://vosstudio.app"}); r.headers.push_back({"X-Title", "VOSStudio"}); }
  Json b = Json::object();
  b.set("model", model);
  Json arr = Json::array();
  if (!system.empty()) {
    Json one = Json::object();
    one.set("role", c.provider == Provider::OpenAI ? "developer" : "system");
    one.set("content", system);
    arr.push(one);
  }
  for (auto& m : msgs) {
    Json one = Json::object();
    one.set("role", m.role == "assistant" ? "assistant" : "user");
    one.set("content", m.text);
    arr.push(one);
  }
  b.set("messages", arr);
  if (c.provider == Provider::OpenAI) b.set("max_completion_tokens", c.maxTokens * 4);  // reasoning models count hidden tokens too
  else b.set("max_tokens", c.maxTokens);
  if (stream) b.set("stream", true);
  r.body = b.dump();
  return r;
}

HttpReq buildModelList(const Config& c) {
  HttpReq r;
  r.method = "GET";
  if (c.provider == Provider::Gemini) {
    r.url = "https://generativelanguage.googleapis.com/v1beta/models?pageSize=200";
    r.headers.push_back({"x-goog-api-key", c.key});
  } else if (c.provider == Provider::Anthropic) {
    r.url = "https://api.anthropic.com/v1/models?limit=100";
    r.headers.push_back({"x-api-key", c.key});
    r.headers.push_back({"anthropic-version", "2023-06-01"});
  } else {
    r.url = (c.provider == Provider::OpenAI ? string("https://api.openai.com/v1") : compatBase(c)) + "/models";
    if (!c.key.empty()) r.headers.push_back({"Authorization", "Bearer " + c.key});
  }
  return r;
}

vector<string> parseModelList(const Config& c, const string& body) {
  vector<string> out;
  Json j = Json::parse(body);
  if (c.provider == Provider::Gemini) {
    for (auto& m : j["models"].a) {
      bool gen = false;
      for (auto& g : m["supportedGenerationMethods"].a) if (g.str() == "generateContent") gen = true;
      string name = m["name"].str();
      if (name.rfind("models/", 0) == 0) name = name.substr(7);
      if (gen && !name.empty() && name.find("embedding") == string::npos && name.find("aqa") == string::npos) out.push_back(name);
    }
  } else {
    for (auto& m : j["data"].a) {
      string id = m["id"].str();
      if (id.empty()) continue;
      if (c.provider == Provider::OpenAI) {  // chat-capable families only
        static const char* skip[] = {"embedding", "whisper", "tts", "dall-e", "moderation", "davinci", "babbage", "image", "audio", "realtime", "transcribe", "search", "sora"};
        bool bad = false;
        for (auto* s : skip) if (id.find(s) != string::npos) bad = true;
        if (bad || !(id.rfind("gpt", 0) == 0 || id.rfind("o", 0) == 0 || id.rfind("chatgpt", 0) == 0)) continue;
      }
      out.push_back(id);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

static string textOfGeminiCandidate(const Json& j) {
  string s;
  for (auto& cand : j["candidates"].a) {
    for (auto& p : cand["content"]["parts"].a)
      if (!p["thought"].boolean()) s += p["text"].str();
    break;
  }
  return s;
}

string parseChat(const Config& c, const string& body, string* err) {
  Json j = Json::parse(body);
  if (j.has("error")) { if (err) *err = j["error"]["message"].str(j["error"].str("Unknown error")); return ""; }
  if (c.provider == Provider::Gemini) return textOfGeminiCandidate(j);
  if (c.provider == Provider::Anthropic) {
    string s;
    for (auto& b : j["content"].a) if (b["type"].str() == "text") s += b["text"].str();
    return s;
  }
  return j["choices"][size_t(0)]["message"]["content"].str();
}

string describeError(const Config& c, int status, const string& body, const string& transportErr) {
  string msg;
  Json j = Json::parse(body);
  if (j.has("error")) msg = j["error"].t == Json::Obj ? j["error"]["message"].str() : j["error"].str();
  if (msg.empty() && j.t == Json::Arr && j.size()) msg = j[size_t(0)]["error"]["message"].str();
  if (msg.size() > 300) msg = truncate(msg, 300);
  string who = providerName(c.provider);
  if (status == 0) return "Could not reach " + who + (transportErr.empty() ? "." : ": " + transportErr + ".") + " Check the internet connection" + (c.provider == Provider::Compatible ? " and the server address." : ".");
  if (status == 401 || status == 403) return who + " rejected the API key (" + std::to_string(status) + "). Check it in Settings." + (msg.empty() ? "" : "\n" + msg);
  if (status == 404) return "Model \"" + c.effectiveModel() + "\" was not found. Choose another model in Settings." + (msg.empty() ? "" : "\n" + msg);
  if (status == 429) return who + " rate limit or quota reached. Wait a moment or check the billing of this key." + (msg.empty() ? "" : "\n" + msg);
  if (status >= 500) return who + " is temporarily unavailable (" + std::to_string(status) + "). Try again shortly.";
  return who + " error " + std::to_string(status) + (msg.empty() ? "." : ": " + msg);
}

// ================================================================= streaming
bool StreamParser::feed(const char* data, size_t n) {
  buf_.append(data, n);
  size_t pos;
  while ((pos = buf_.find('\n')) != string::npos) {
    string l = buf_.substr(0, pos);
    buf_.erase(0, pos + 1);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    line(l);
  }
  return error.empty();
}

void StreamParser::finish() {
  if (!buf_.empty()) { string l = buf_; buf_.clear(); if (!l.empty() && l.back() == '\r') l.pop_back(); line(l); }
  // a non-SSE body (e.g. an error object sent without streaming) ends up in text: detect it
  if (text.empty() && error.empty() && !raw_.empty()) {
    Json j = Json::parse(raw_);
    if (j.has("error")) error = j["error"].t == Json::Obj ? j["error"]["message"].str("Unknown error") : j["error"].str("Unknown error");
  }
  done = true;
}

void StreamParser::line(const string& l) {
  if (l.empty()) { event_.clear(); return; }
  if (l.rfind("event:", 0) == 0) { event_ = trim(l.substr(6)); return; }
  if (l[0] == ':') return;  // SSE comment / keep-alive
  if (l.rfind("data:", 0) != 0) { if (raw_.size() < 100000) raw_ += l + "\n"; return; }
  string d = trim(l.substr(5));
  if (d == "[DONE]") { done = true; return; }
  Json j = Json::parse(d);
  if (j.has("error")) {
    error = j["error"].t == Json::Obj ? j["error"]["message"].str("Unknown error") : j["error"].str("Unknown error");
    return;
  }
  switch (p_) {
    case Provider::Gemini: {
      text += textOfGeminiCandidate(j);
      string fr = j["candidates"][size_t(0)]["finishReason"].str();
      if (!fr.empty()) stopReason = fr;
      break;
    }
    case Provider::Anthropic: {
      string type = j["type"].str();
      if (type == "content_block_delta" && j["delta"]["type"].str() == "text_delta") text += j["delta"]["text"].str();
      else if (type == "message_delta") stopReason = j["delta"]["stop_reason"].str();
      else if (type == "message_stop") done = true;
      break;
    }
    default: {
      const Json& ch = j["choices"][size_t(0)];
      text += ch["delta"]["content"].str();
      string fr = ch["finish_reason"].str();
      if (!fr.empty()) stopReason = fr;
      break;
    }
  }
}

// ================================================================= research context
namespace {
struct Counter {
  std::map<string, int> m;
  std::map<string, string> disp;
  void add(const string& s) {
    string k = lower(trim(s));
    if (k.empty()) return;
    m[k]++;
    if (!disp.count(k)) disp[k] = trim(s);
  }
  vector<std::pair<string, int>> top(size_t n) const {
    vector<std::pair<string, int>> v;
    for (auto& e : m) v.push_back({disp.at(e.first), e.second});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    if (v.size() > n) v.resize(n);
    return v;
  }
};
string listTop(const vector<std::pair<string, int>>& v) {
  string s;
  for (size_t i = 0; i < v.size(); i++) s += (i ? "; " : "") + v[i].first + " (" + std::to_string(v[i].second) + ")";
  return s;
}
string firstAuthor(const Record& r) {
  if (r.authors.empty()) return "Anonymous";
  string a = r.authors[0];
  size_t c = a.find(',');
  if (c != string::npos) a = a.substr(0, c);
  return trim(a);
}
string cite(const Record& r) { return firstAuthor(r) + (r.authors.size() > 2 ? " et al." : r.authors.size() == 2 ? " & co." : "") + " (" + (r.year ? std::to_string(r.year) : string("n.d.")) + ")"; }
double nodeScore(const Network& N, int i, const string& name) {
  int k = N.scoreIndex(name);
  if (k < 0 || size_t(k) >= N.nodes[size_t(i)].sc.size()) return NAN;
  return N.nodes[size_t(i)].sc[size_t(k)];
}
}  // namespace

string corpusContext(const Project& P) {
  const Corpus& C = P.corpus;
  if (C.recs.empty()) return "";
  int y0 = 0, y1 = 0;
  long long cites = 0;
  Counter src, kw, ctry, types;
  std::map<int, int> perYear;
  for (auto& r : C.recs) {
    if (r.year) { y0 = y0 ? std::min(y0, r.year) : r.year; y1 = std::max(y1, r.year); perYear[r.year]++; }
    cites += r.cites;
    src.add(r.source);
    for (auto& k : r.keywords) kw.add(k);
    std::set<string> cs;
    for (auto& c : r.countries) cs.insert(c);
    for (auto& c : cs) ctry.add(c);
    if (!r.docType.empty()) types.add(r.docType);
  }
  string s = "## Data set\n";
  s += "- " + fmtInt(long(C.recs.size())) + " records (" + formatLabel(C.format) + ")";
  if (y0) s += ", published " + std::to_string(y0) + "-" + std::to_string(y1);
  s += ", " + fmtInt(cites) + " citations in total (" + fmtNum(double(cites) / double(C.recs.size()), 1) + " per record)\n";
  if (!perYear.empty()) {
    s += "- Records per year: ";
    int k = 0;
    for (auto& e : perYear) s += string(k++ ? ", " : "") + std::to_string(e.first) + ": " + std::to_string(e.second);
    s += "\n";
  }
  auto ts = src.top(10); if (!ts.empty()) s += "- Leading sources: " + listTop(ts) + "\n";
  auto tk = kw.top(25); if (!tk.empty()) s += "- Most frequent author keywords: " + listTop(tk) + "\n";
  auto tc = ctry.top(10); if (!tc.empty()) s += "- Leading countries: " + listTop(tc) + "\n";
  auto tt = types.top(5); if (!tt.empty()) s += "- Document types: " + listTop(tt) + "\n";
  return s;
}

string mapContext(const Project& P, int maxItems) {
  const Network& N = P.net;
  if (N.n() == 0) return "";
  string s = "## Current map\n- " + (N.description.empty() ? string("Network map") : N.description) + ": " + fmtInt(N.n()) + " " + N.unitNoun + "s, " + fmtInt(N.m()) +
             " links, " + std::to_string(N.nClusters) + " clusters, modularity " + fmtNum(N.quality, 3) + "\n";
  bool hasYear = N.scoreIndex("Avg. pub. year") >= 0, hasCit = N.scoreIndex("Avg. citations") >= 0;
  for (int c = 0; c < N.nClusters; c++) {
    vector<int> items;
    for (int i = 0; i < N.n(); i++) if (N.nodes[size_t(i)].cluster == c) items.push_back(i);
    if (items.empty()) continue;
    std::sort(items.begin(), items.end(), [&](int a, int b) { return N.weight(a) > N.weight(b); });
    double ys = 0, yn = 0, cs = 0, cn = 0;
    for (int i : items) {
      double y = nodeScore(N, i, "Avg. pub. year"), ci = nodeScore(N, i, "Avg. citations");
      if (std::isfinite(y)) { ys += y; yn++; }
      if (std::isfinite(ci)) { cs += ci; cn++; }
    }
    s += "- Cluster " + std::to_string(c + 1);
    if (c < int(N.clusterNames.size()) && !N.clusterNames[size_t(c)].empty()) s += " \"" + N.clusterNames[size_t(c)] + "\"";
    s += " (" + std::to_string(items.size()) + " items";
    if (hasYear && yn > 0) s += ", avg. year " + fmtNum(ys / yn, 1);
    if (hasCit && cn > 0) s += ", avg. citations " + fmtNum(cs / cn, 1);
    s += "): ";
    for (size_t k = 0; k < items.size() && int(k) < maxItems; k++) s += (k ? ", " : "") + N.nodes[size_t(items[k])].label;
    s += "\n";
  }
  return s;
}

string topDocsContext(const Project& P, int n) {
  const Corpus& C = P.corpus;
  if (C.recs.empty()) return "";
  vector<int> idx(C.recs.size());
  for (size_t i = 0; i < idx.size(); i++) idx[i] = int(i);
  size_t k = std::min<size_t>(size_t(n), idx.size());
  std::partial_sort(idx.begin(), idx.begin() + long(k), idx.end(), [&](int a, int b) { return C.recs[size_t(a)].cites > C.recs[size_t(b)].cites; });
  string s = "## Most cited records\n";
  for (size_t i = 0; i < k; i++) {
    const Record& r = C.recs[size_t(idx[i])];
    if (r.cites <= 0 && i > 0) break;
    s += "- " + cite(r) + " " + truncate(r.title, 160) + ". " + truncate(r.source, 60) + ". " + std::to_string(r.cites) + " citations\n";
  }
  return s;
}

string trendContext(const Project& P) {
  const Corpus& C = P.corpus;
  if (C.recs.size() < 20) return "";
  int y1 = 0;
  for (auto& r : C.recs) y1 = std::max(y1, r.year);
  if (!y1) return "";
  int cut = y1 - 2;  // last three publication years vs. everything before
  std::map<string, std::pair<int, int>> cnt;
  std::map<string, string> disp;
  int recent = 0, before = 0;
  for (auto& r : C.recs) {
    if (!r.year) continue;
    bool rec = r.year >= cut;
    (rec ? recent : before)++;
    std::set<string> seen;
    for (auto& k : r.keywords) {
      string l = lower(trim(k));
      if (l.empty() || !seen.insert(l).second) continue;
      if (!disp.count(l)) disp[l] = trim(k);
      (rec ? cnt[l].first : cnt[l].second)++;
    }
  }
  if (recent == 0 || before == 0) return "";
  struct E { string k; double ratio; int r, b; };
  vector<E> rising, fading;
  for (auto& e : cnt) {
    int r = e.second.first, b = e.second.second;
    if (r + b < 4) continue;
    double sr = (r + 0.5) / recent, sb = (b + 0.5) / before;
    double ratio = sr / sb;
    if (ratio > 1.6 && r >= 3) rising.push_back({disp[e.first], ratio, r, b});
    if (ratio < 0.5 && b >= 4) fading.push_back({disp[e.first], ratio, r, b});
  }
  std::sort(rising.begin(), rising.end(), [](auto& a, auto& b) { return a.ratio > b.ratio; });
  std::sort(fading.begin(), fading.end(), [](auto& a, auto& b) { return a.ratio < b.ratio; });
  string s = "## Keyword trends (" + std::to_string(cut) + "-" + std::to_string(y1) + " vs. earlier)\n";
  s += "- Rising: ";
  for (size_t i = 0; i < std::min<size_t>(15, rising.size()); i++) s += (i ? "; " : "") + rising[i].k + " (" + std::to_string(rising[i].b) + " -> " + std::to_string(rising[i].r) + ")";
  if (rising.empty()) s += "none";
  s += "\n- Fading: ";
  for (size_t i = 0; i < std::min<size_t>(10, fading.size()); i++) s += (i ? "; " : "") + fading[i].k + " (" + std::to_string(fading[i].b) + " -> " + std::to_string(fading[i].r) + ")";
  if (fading.empty()) s += "none";
  s += "\n";
  return s;
}

string itemContext(const Project& P, int item) {
  const Network& N = P.net;
  if (item < 0 || item >= N.n()) return "";
  const Node& nd = N.nodes[size_t(item)];
  string s = "## Selected item: " + nd.label + "\n- Cluster " + std::to_string(nd.cluster + 1) + " (" + N.clusterName(nd.cluster) + ")\n";
  for (size_t k = 0; k < nd.w.size() && k < N.weightNames.size(); k++) s += "- " + N.weightNames[k] + ": " + fmtNum(nd.w[k], 0) + "\n";
  for (size_t k = 0; k < nd.sc.size() && k < N.scoreNames.size(); k++) if (std::isfinite(nd.sc[k])) s += "- " + N.scoreNames[k] + ": " + fmtNum(nd.sc[k], 2) + "\n";
  vector<std::pair<double, int>> nb;
  for (auto& l : N.links) {
    if (l.a == item) nb.push_back({l.w, l.b});
    else if (l.b == item) nb.push_back({l.w, l.a});
  }
  std::sort(nb.begin(), nb.end(), [](auto& a, auto& b) { return a.first > b.first; });
  if (!nb.empty()) {
    s += "- Strongest links: ";
    for (size_t k = 0; k < std::min<size_t>(15, nb.size()); k++) s += (k ? ", " : "") + N.nodes[size_t(nb[k].second)].label + " (" + fmtNum(nb[k].first, 0) + ")";
    s += "\n";
  }
  if (!nd.recs.empty()) {
    vector<int> r(nd.recs.begin(), nd.recs.end());
    std::sort(r.begin(), r.end(), [&](int a, int b) { return P.corpus.recs[size_t(a)].cites > P.corpus.recs[size_t(b)].cites; });
    s += "- Example records:\n";
    for (size_t k = 0; k < std::min<size_t>(6, r.size()); k++) {
      const Record& rr = P.corpus.recs[size_t(r[k])];
      s += "  - " + cite(rr) + " " + truncate(rr.title, 150) + "\n";
    }
  }
  return s;
}

string clusterContext(const Project& P, int cluster) {
  const Network& N = P.net;
  if (cluster < 0 || cluster >= N.nClusters) return "";
  vector<int> items;
  for (int i = 0; i < N.n(); i++) if (N.nodes[size_t(i)].cluster == cluster) items.push_back(i);
  std::sort(items.begin(), items.end(), [&](int a, int b) { return N.weight(a) > N.weight(b); });
  string s = "## Selected cluster " + std::to_string(cluster + 1) + " (" + N.clusterName(cluster) + "), " + std::to_string(items.size()) + " items\n- Items by weight: ";
  for (size_t k = 0; k < std::min<size_t>(40, items.size()); k++) s += (k ? ", " : "") + N.nodes[size_t(items[k])].label;
  s += "\n";
  std::set<int> recs;
  for (int i : items) for (int r : N.nodes[size_t(i)].recs) recs.insert(r);
  if (!recs.empty()) {
    vector<int> r(recs.begin(), recs.end());
    std::sort(r.begin(), r.end(), [&](int a, int b) { return P.corpus.recs[size_t(a)].cites > P.corpus.recs[size_t(b)].cites; });
    s += "- Most cited records of the cluster:\n";
    for (size_t k = 0; k < std::min<size_t>(8, r.size()); k++) {
      const Record& rr = P.corpus.recs[size_t(r[k])];
      s += "  - " + cite(rr) + " " + truncate(rr.title, 150) + " (" + std::to_string(rr.cites) + " citations)\n";
    }
  }
  return s;
}

string documentContext(const Project& P, int rec) {
  if (rec < 0 || rec >= int(P.corpus.recs.size())) return "";
  const Record& r = P.corpus.recs[size_t(rec)];
  string s = "## Document\n- Title: " + r.title + "\n";
  if (!r.authors.empty()) {
    s += "- Authors: ";
    for (size_t k = 0; k < std::min<size_t>(12, r.authors.size()); k++) s += (k ? "; " : "") + r.authors[k];
    if (r.authors.size() > 12) s += "; et al.";
    s += "\n";
  }
  s += "- Source: " + r.source + (r.year ? ", " + std::to_string(r.year) : string()) + "\n";
  if (!r.doi.empty()) s += "- DOI: " + r.doi + "\n";
  s += "- Citations: " + std::to_string(r.cites) + "\n";
  if (!r.keywords.empty()) {
    s += "- Keywords: ";
    for (size_t k = 0; k < r.keywords.size(); k++) s += (k ? "; " : "") + r.keywords[k];
    s += "\n";
  }
  if (!r.abstract_.empty()) s += "- Abstract: " + truncate(r.abstract_, 4000) + "\n";
  else s += "- Abstract: not available in the record\n";
  return s;
}

string buildContext(const Project& P, const ContextOpts& o) {
  string s;
  if (o.corpus) s += corpusContext(P);
  if (o.map) { string m = mapContext(P, o.maxItemsPerCluster); if (!m.empty()) s += (s.empty() ? "" : "\n") + m; }
  if (o.trends) { string t = trendContext(P); if (!t.empty()) s += "\n" + t; }
  if (o.topDocs) { string t = topDocsContext(P, 12); if (!t.empty()) s += "\n" + t; }
  if (o.selectedCluster >= 0) s += "\n" + clusterContext(P, o.selectedCluster);
  if (o.selectedItem >= 0) s += "\n" + itemContext(P, o.selectedItem);
  if (o.document >= 0) s += "\n" + documentContext(P, o.document);
  return s;
}

// ================================================================= tasks
const vector<TaskInfo>& tasks() {
  static const vector<TaskInfo> t = {
      {Task::Chat, "chat", "Ask anything", "Questions about your data, map and literature", "chat"},
      {Task::Overview, "overview", "Research landscape", "What the field is about, its structure and key works", "globe"},
      {Task::Review, "review", "Literature review draft", "Structured review built on the clusters and most cited works", "book"},
      {Task::Gaps, "gaps", "Gaps and future directions", "Under-explored themes and research questions", "target"},
      {Task::Emerging, "emerging", "Emerging topics", "Rising and fading themes from the keyword trends", "trends"},
      {Task::NameClusters, "names", "Name the clusters", "Short, meaningful names written onto the map", "tag"},
      {Task::Explain, "explain", "Explain the selection", "The selected item or cluster in context", "info"},
      {Task::Document, "document", "Summarise a document", "Key findings, methods and relevance of a paper", "file"},
      {Task::Search, "search", "Plan an OpenAlex search", "Turn a research question into keyword and semantic queries", "search"},
      {Task::Caption, "caption", "Figure caption and results", "Caption and a results paragraph for the current map", "publish"},
      {Task::Methods, "methods", "Methods paragraph", "A publication-ready description of the analysis", "quote"},
      {Task::Clean, "clean", "Clean the terms", "Merge synonyms and abbreviations, drop generic terms", "sparkle"},
  };
  return t;
}

const TaskInfo& taskInfo(Task t) {
  static const TaskInfo agent{Task::Agent, "agent", "Agent", "Works through a goal with the app's tools", "sparkle"};
  if (t == Task::Agent) return agent;
  for (auto& x : tasks()) if (x.task == t) return x;
  return tasks()[0];
}

string systemPrompt() {
  return "You are the research assistant inside VOSStudio, a bibliometric mapping application similar to VOSviewer. "
         "You help researchers interpret science maps (co-occurrence, co-authorship, citation, bibliographic coupling, co-citation), "
         "clusters, trends and the records they imported. Ground every statement in the research context supplied with the request; "
         "when you add general domain knowledge, say so. Never invent references: cite only records listed in the context, as Author (Year). "
         "Write in clear academic English, concise and well structured, using Markdown headings, bullet lists and **bold** sparingly. "
         "Do not repeat the context back verbatim.";
}

string taskPrompt(Task t, const string& extra) {
  string x = trim(extra);
  switch (t) {
    case Task::Chat: return x;
    case Task::Overview:
      return "Describe the research landscape represented by this data set and map: the main themes (one per cluster), how they relate, "
             "the most influential works and venues, and the geographic distribution. Finish with three key takeaways." + (x.empty() ? "" : "\nFocus: " + x);
    case Task::Review:
      return "Draft a literature review section based on this data set. Use one subsection per cluster with an informative heading, "
             "discuss its themes and cite the listed records as Author (Year) where they fit. Add a short introduction and a synthesis paragraph at the end. "
             "About 700-900 words." + (x.empty() ? "" : "\nFocus: " + x);
    case Task::Gaps:
      return "Identify research gaps and future directions. Consider weakly connected or small clusters, fading and rising keywords, "
             "missing links between themes, and geographic imbalance. Give 5-7 gaps, each with a one-sentence rationale grounded in the context "
             "and a concrete research question." + (x.empty() ? "" : "\nFocus: " + x);
    case Task::Emerging:
      return "Explain the emerging and declining topics in this field using the keyword trends and the cluster publication years. "
             "Group related rising keywords into themes, explain what drives them, and say which themes appear to be maturing or fading." + (x.empty() ? "" : "\nFocus: " + x);
    case Task::NameClusters:
      return "Give each cluster of the current map a short, specific name (2-5 words, Title Case, no numbering) and a one-sentence summary. "
             "Reply with only a JSON array, no prose, in the form: "
             "[{\"cluster\": 1, \"name\": \"...\", \"summary\": \"...\"}, ...] with one object per cluster in the context.";
    case Task::Explain:
      return "Explain the selected item or cluster: what it stands for in this field, why it sits where it does in the map, "
             "its strongest connections and what the example records suggest." + (x.empty() ? "" : "\n" + x);
    case Task::Document:
      return "Summarise the document: research question, approach, main findings (as far as the abstract shows), and how it relates to the "
             "themes of this data set. Use short sections. Mark clearly what cannot be judged from the abstract alone." + (x.empty() ? "" : "\n" + x);
    case Task::Search:
      return "Design an OpenAlex search for this research need: \"" + (x.empty() ? string("the topic of the current data set") : x) + "\". "
             "Reply with only a JSON object, no prose: {\"keywords\": \"term one; term two; \\\"exact phrase\\\"\", "
             "\"semantic\": [\"one-sentence description of the ideal paper\", \"an alternative angle\", \"a third angle\"], "
             "\"fromYear\": 0, \"toYear\": 0}. Use 3-8 keyword terms separated by semicolons, include synonyms, and 0 for unrestricted years.";
    case Task::Caption:
      return "Write (1) a figure caption for the current map suitable for a journal (2-3 sentences, mention the analysis type, unit, counting and number of items and clusters), "
             "and (2) a results paragraph (150-200 words) that describes what the map shows, cluster by cluster." + (x.empty() ? "" : "\n" + x);
    case Task::Clean:
      return "Below are the most frequent terms of one field of this data set, with their number of occurrences (term<TAB>count). "
             "Clean them for a science map:\n"
             "1. Merge terms that mean the same in this field: synonyms, abbreviations and their long forms, and spelling, plural or hyphenation variants.\n"
             "2. Mark generic terms that say nothing about the topic here (for example \"study\", \"analysis\", \"review\", \"results\") to be ignored.\n"
             "Use only terms that appear in the list, spelled exactly as listed. Choose as target the clearest label, usually the most frequent one. "
             "Do not merge related but different concepts (for example \"machine learning\" and \"deep learning\"). Leave out anything you are unsure about.\n"
             "Reply with only a JSON object, no prose: {\"merge\": [{\"target\": \"...\", \"members\": [\"...\"], \"kind\": \"synonym|abbreviation|variant\", "
             "\"reason\": \"a few words\"}], \"ignore\": [{\"term\": \"...\", \"reason\": \"a few words\"}]}\n\nTerms:\n" + x;
    case Task::Agent: return x;
    case Task::Methods:
      return "Write a publication-ready methods paragraph for this bibliometric analysis, based on the method details given. "
             "Mention the data source, time span, number of records, analysis type, counting method, thresholds, normalisation, layout and clustering, "
             "and the software (VOSStudio). About 150 words, one paragraph." + (x.empty() ? "" : "\nMethod details:\n" + x);
    default: return x;
  }
}

// ================================================================= structured replies
static string extractJson(const string& reply, char open, char close) {
  size_t a = reply.find(open), b = reply.rfind(close);
  if (a == string::npos || b == string::npos || b <= a) return "";
  return reply.substr(a, b - a + 1);
}

vector<ClusterName> parseClusterNames(const string& reply) {
  vector<ClusterName> out;
  Json j = Json::parse(extractJson(reply, '[', ']'));
  for (auto& o : j.a) {
    ClusterName c;
    c.cluster = o["cluster"].integer(0) - 1;
    c.name = trim(o["name"].str());
    c.summary = trim(o["summary"].str());
    if (c.cluster >= 0 && !c.name.empty()) out.push_back(c);
  }
  return out;
}

bool parseSearchPlan(const string& reply, SearchPlan& out) {
  Json j = Json::parse(extractJson(reply, '{', '}'));
  if (j.t != Json::Obj) return false;
  out.keywords = trim(j["keywords"].str());
  out.semantic.clear();
  for (auto& s : j["semantic"].a) if (!trim(s.str()).empty()) out.semantic.push_back(trim(s.str()));
  out.fromYear = j["fromYear"].integer(0);
  out.toYear = j["toYear"].integer(0);
  return !out.keywords.empty() || !out.semantic.empty();
}

// ---------------------------------------------------------------- cleaning
string cleanTermList(const vector<std::pair<string, int>>& terms, size_t maxTerms) {
  string o;
  for (size_t i = 0; i < terms.size() && i < maxTerms; i++) o += terms[i].first + "\t" + std::to_string(terms[i].second) + "\n";
  return o;
}

bool parseCleanPlan(const string& reply, vector<CleanProposal>& out) {
  out.clear();
  Json j = Json::parse(extractJson(reply, '{', '}'));
  if (j.t != Json::Obj) return false;
  for (auto& m : j["merge"].a) {
    CleanProposal p;
    p.target = trim(m["target"].str());
    for (auto& x : m["members"].a) { string v = trim(x.str()); if (!v.empty() && lower(v) != lower(p.target)) p.members.push_back(v); }
    p.kind = lower(trim(m["kind"].str("synonym")));
    if (p.kind.empty()) p.kind = "synonym";
    p.reason = trim(m["reason"].str());
    if (!p.target.empty() && !p.members.empty()) out.push_back(p);
  }
  for (auto& g : j["ignore"].a) {
    CleanProposal p;
    p.target = trim(g.t == Json::Str ? g.str() : g["term"].str());
    p.reason = g.t == Json::Str ? string() : trim(g["reason"].str());
    p.kind = "generic";
    p.ignore = true;
    if (!p.target.empty()) out.push_back(p);
  }
  return true;
}

// ---------------------------------------------------------------- agent
const vector<ToolSpec>& agentTools() {
  static const vector<ToolSpec> t = {
      {"get_overview", "{}", "The data set (records, years, fields) and the current map (analysis, items, links, clusters, quality, clustering method).", ToolKind::Read},
      {"list_analyses", "{}", "Analysis types and units that this data set supports, with field coverage.", ToolKind::Read},
      {"list_clusters", "{\"top\": 8}", "Clusters of the current map: size, name, average year and top items.", ToolKind::Read},
      {"top_items", "{\"by\": \"weight|links|citations|year\", \"n\": 15, \"cluster\": 0}", "Top items of the map, optionally within one cluster (cluster number as shown, 0 = all).", ToolKind::Read},
      {"get_item", "{\"label\": \"...\"}", "One item: cluster, weight, links, average year, citations and its strongest neighbours.", ToolKind::Read},
      {"search_items", "{\"query\": \"...\"}", "Items whose label contains the text.", ToolKind::Read},
      {"trends", "{}", "Keyword trends of the data set: growing and declining keywords and bursts.", ToolKind::Read},
      {"top_documents", "{\"n\": 10}", "The most cited documents of the data set.", ToolKind::Read},
      {"check_stability", "{\"runs\": 10}", "Re-runs the clustering with other seeds and reports agreement (ARI; near 1 = robust).", ToolKind::Read},
      {"compare_periods", "{\"a_from\": 0, \"a_to\": 0, \"b_from\": 0, \"b_to\": 0}",
       "Compares two periods on the current map (built from records): which items appear, grow, stay stable or fade in their share of documents. Omit the years to split "
       "the data into two halves. Add the figure with add_chart difference_map.", ToolKind::Read},
      {"scan_variants", "{\"unit\": \"keywords|all_keywords|index_terms|authors|sources|organisations\"}", "Spelling, plural, hyphen and acronym variants found by rules.", ToolKind::Read},
      {"get_methods", "{}", "The methods paragraph of the current map.", ToolKind::Read},
      {"show_view", "{\"view\": \"network|overlay|density|timeline|matrix|geo|3d\"}", "Switches the map view the user sees.", ToolKind::View},
      {"focus_item", "{\"label\": \"...\"}", "Selects an item on the map and zooms to it.", ToolKind::View},
      {"focus_cluster", "{\"cluster\": 1}", "Highlights one cluster on the map.", ToolKind::View},
      {"open_page", "{\"page\": \"data|build|look|analyse|trends|actors|publish\"}", "Opens a page of the left panel.", ToolKind::View},
      {"build_map", "{\"type\": \"cooccurrence|coauthorship|citation|coupling|cocitation\", \"unit\": \"keywords|all_keywords|index_terms|terms|authors|organisations|countries|documents|sources|references|cited_sources|cited_authors\", \"counting\": \"full|fractional\", \"min\": 5, \"max_items\": 1000, \"from_year\": 0, \"to_year\": 0}",
       "Builds a new map (layout and clusters). Omitted fields keep sensible defaults for the type and unit.", ToolKind::Change},
      {"recluster", "{\"resolution\": 1.0, \"min_cluster_size\": 1, \"method\": \"vosviewer|modularity\"}", "Re-runs the clustering of the current map with new settings (omitted = unchanged).", ToolKind::Change},
      {"name_clusters", "{\"names\": [{\"cluster\": 1, \"name\": \"...\"}]}", "Writes cluster names onto the map (cluster numbers as shown).", ToolKind::Change},
      {"merge_terms", "{\"unit\": \"keywords\", \"merges\": [{\"target\": \"...\", \"members\": [\"...\"]}], \"ignore\": [\"...\"]}",
       "Adds merges and ignored terms to the thesaurus (applies to every map; rebuild to see it).", ToolKind::Change},
      {"search_openalex", "{\"query\": \"...\", \"semantic\": false, \"from_year\": 0, \"to_year\": 0, \"max\": 300, \"append\": false}",
       "Fetches works from OpenAlex (titles, abstracts, authors, sources, countries, references) and loads them as the data set; replaces the current records unless append is true. "
       "Keyword search: 2 to 6 precise key terms; several alternative searches can be separated with ; and are merged. semantic true searches by meaning with a one or two sentence "
       "description (fewer, closely related works). max: 50 to 2000 works.", ToolKind::Change},
      {"read_papers", "{\"sort\": \"cited|recent|relevance\", \"n\": 12, \"query\": \"\", \"skip\": 0}",
       "Reads papers of the data set: id (R1, R2, ...), authors, year, title, source, citations, keywords and the start of the abstract. query keeps papers whose title, abstract "
       "or keywords contain all its words. n at most 30; skip pages through the list.", ToolKind::Read},
      {"add_chart", "{\"chart\": \"publications_per_year\", \"title\": \"\", \"caption\": \"\", \"unit\": \"all_keywords\"}",
       "Adds a figure to the report. Charts from the records: publications_per_year, top_sources, top_authors, top_countries, top_organisations, most_cited, citation_classes, "
       "trend_topics, keyword_bursts, thematic_evolution, three_field, country_collaboration, production_over_time, bradford, lotka, rpys (needs references). "
       "From the current map: map_network, map_overlay (average year), map_density, map_geo (countries), strategic_diagram, difference_map (change between the periods of "
       "compare_periods), resolution_sweep (clusters and robustness per resolution). From the citations: main_path (the backbone of the citation network; needs references). unit applies to trend_topics, keyword_bursts, "
       "thematic_evolution and production_over_time. title and caption are optional (a caption explains what the figure shows).", ToolKind::View},
      {"add_section", "{\"title\": \"...\", \"text\": \"...\"}",
       "Adds a text section to the report, after the figures and sections added so far. text is Markdown (paragraphs, - bullets, ### subheadings). "
       "Cite papers by their ids from read_papers: [R3] or [R3, R8].", ToolKind::View},
      {"get_report", "{}", "The report so far: its sections and figures in order.", ToolKind::Read},
      {"export_report", "{\"title\": \"...\", \"subtitle\": \"\"}",
       "Writes the report as one PDF: title, the sections and figures in the order added, and a numbered reference list of the cited papers. Returns the file path.", ToolKind::View},
  };
  return t;
}

const ToolSpec* findTool(const string& name) {
  for (auto& t : agentTools()) if (name == t.name) return &t;
  return nullptr;
}

string agentSystemPrompt() {
  string s =
      "You are the agent inside VOSStudio, a desktop application for bibliometric science maps (like VOSviewer). You help a researcher reach a goal "
      "by using the application's tools, step by step.\n\n"
      "In every reply, output exactly one JSON object and nothing else. Either call a tool:\n"
      "{\"thought\": \"one short sentence: what you do next and why\", \"tool\": \"tool_name\", \"args\": {...}}\n"
      "or finish:\n"
      "{\"thought\": \"...\", \"final\": \"your answer to the user, in Markdown\"}\n\n"
      "Rules:\n"
      "- Get facts from tools. Never invent numbers, items, clusters or documents.\n"
      "- Look before you change: read the overview or the clusters before building or re-clustering.\n"
      "- Tools marked [change] modify the project and need the user's approval. If the user declines, do not repeat the same change; continue or finish.\n"
      "- After each tool call you receive a message that starts with \"Tool result\". If a tool fails, adapt the arguments or choose another tool.\n"
      "- Use at most " + std::to_string(kAgentMaxSteps) + " tool calls. Finish as soon as the goal is reached.\n"
      "- The final answer is concise and specific, grounded in the tool results, and says what you changed in the project, if anything.\n"
      "- Write for researchers. No technical jargon about the application's internals.\n\n"
      "Literature reports. When the user asks for a literature review, summary or report on a topic, work through these steps without asking the user in between:\n"
      "1. If no records are loaded, or the loaded records are about another topic, call search_openalex (300 works is a good default, up to 1000 for broad fields; "
      "set from_year/to_year when the user names a period).\n"
      "2. get_overview, then read_papers several times: sort cited, sort recent, and query for subtopics. Read at least 30 different papers before writing.\n"
      "3. Add 3 to 6 charts that support the text: publications_per_year first, then for example top_sources, top_countries, most_cited, trend_topics or keyword_bursts.\n"
      "4. Build a keyword co-occurrence map (build_map with unit all_keywords), read the clusters, name them with name_clusters and add the map_network figure "
      "(map_overlay shows which themes are recent).\n"
      "5. Write the sections with add_section, in this order: Introduction (topic, scope and data), Research themes (one paragraph per cluster or theme), "
      "Trends and emerging topics, Research gaps and future directions, Conclusion. 150 to 400 words each, in an academic style. Figures appear where you add them, "
      "so add each chart before or after the section that discusses it and refer to it in the text (\"Figure 1 shows ...\").\n"
      "6. Cite only papers you have read, by their ids ([R12]); state only what the papers and the tool results support. Never invent papers, authors or numbers.\n"
      "7. export_report with a clear title. The final answer summarises the main findings in 3 to 5 sentences and says the PDF was saved.\n\n"
      "Tools:\n";
  for (auto& t : agentTools()) {
    s += string("- ") + t.name + " " + t.args + (t.kind == ToolKind::Change ? " [change]" : t.kind == ToolKind::View ? " [view]" : "") + ": " + t.desc + "\n";
  }
  return s;
}

bool parseAgentAction(const string& reply, AgentAction& out, string* err) {
  out = AgentAction();
  string js = extractJson(reply, '{', '}');
  Json j = Json::parse(js);
  if (j.t != Json::Obj) {
    // a model that answers in prose has finished
    string t = trim(reply);
    if (!t.empty() && t.find('{') == string::npos) { out.final = t; return true; }
    if (err) *err = "The reply was not a JSON action.";
    return false;
  }
  out.thought = trim(j["thought"].str());
  if (j.has("final") && !j.has("tool")) {
    out.final = j["final"].t == Json::Str ? j["final"].str() : j["final"].dump();
    if (trim(out.final).empty()) out.final = out.thought.empty() ? string("Done.") : out.thought;
    return true;
  }
  out.tool = trim(j["tool"].str());
  out.args = j.has("args") && j["args"].t == Json::Obj ? j["args"] : Json::object();
  if (out.tool.empty()) { if (err) *err = "The action names no tool and no final answer."; return false; }
  return true;
}

string agentGoalMessage(const string& goal, const string& context) {
  return (context.empty() ? string() : "Current state of VOSStudio:\n\n" + context + "\n---\n\n") + "Goal: " + goal;
}

string agentResultMessage(const string& tool, bool ok, const string& result) {
  string r = result.size() > 12000 ? result.substr(0, 12000) + "\n(truncated)" : result;
  return "Tool result for " + tool + (ok ? "" : " (failed)") + ":\n" + r;
}

Json turnsToJson(const vector<Turn>& t) {
  Json a = Json::array();
  for (auto& x : t) {
    Json o = Json::object();
    o.set("role", x.role);
    o.set("text", x.text);
    if (!x.task.empty()) o.set("task", x.task);
    if (!x.when.empty()) o.set("when", x.when);
    a.push(o);
  }
  return a;
}

vector<Turn> turnsFromJson(const Json& j) {
  vector<Turn> t;
  for (auto& o : j.a) t.push_back({o["role"].str(), o["text"].str(), o["task"].str(), o["when"].str()});
  return t;
}

}  // namespace ai
}  // namespace vs
