// AI assistant: settings, streaming chat, one-click research tasks, Markdown rendering and actions that write back into the project.
#include <ctime>
#include <thread>

#include "app.h"

namespace vs {
namespace win {

using ai::Task;

// ================================================================= configuration
ai::Config App::aiConfig() const {
  if (aiCfgValid_) return aiCfg_;
  ai::Config c;
  int p = clampv(settings.j["aiProvider"].integer(0), 0, ai::kProviders - 1);
  c.provider = ai::Provider(p);
  c.key = unprotectSecret(settings.j["aiKey" + std::to_string(p)].str());
  if (c.key.empty()) c.key = settings.j["aiKeyPlain" + std::to_string(p)].str();  // DPAPI unavailable (rare)
  c.model = settings.j["aiModel" + std::to_string(p)].str();
  c.baseUrl = settings.j["aiBase"].str();
  aiCfg_ = c;
  aiCfgValid_ = true;
  return c;
}

string nowStamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
  return b;
}

// ================================================================= conversation state
void App::aiSyncTurns() {
  if (aiTurnsOf == P.get()) return;
  if (aiLive) aiStop();
  // another project: the agent's run and its undo snapshot belong to the previous one
  if (agent.req) agent.req->cancel = true;
  agent = AgentRun();
  agentUndo = AgentUndo();
  aiTurns = ai::turnsFromJson(P->assistant);
  aiTurnsOf = P.get();
  aiLayout_.clear();
  aiDoc = -1;
  aiPending = Task::Chat;
  aiStickBottom = true;
}

void App::aiSaveTurns() {
  vector<ai::Turn> keep;
  for (auto& t : aiTurns) if (t.role != "error") keep.push_back(t);
  P->assistant = ai::turnsToJson(keep);
  P->dirty = true;
}

ai::ContextOpts App::aiContextOpts(Task t) const {
  ai::ContextOpts o;
  o.corpus = aiCtxCorpus;
  o.map = aiCtxMap && hasMap();
  o.trends = aiCtxTrends;
  o.topDocs = aiCtxDocs;
  // task-specific essentials regardless of the toggles
  if (t == Task::NameClusters || t == Task::Caption) { o.map = hasMap(); o.maxItemsPerCluster = 20; }
  if (t == Task::Review || t == Task::Overview) o.topDocs = true;
  if (t == Task::Emerging) o.trends = true;
  if (t == Task::Search) { o.map = false; o.topDocs = false; o.trends = false; }
  if (t == Task::Clean) { o.map = false; o.topDocs = false; o.trends = false; o.corpus = true; }
  if (aiCtxSel || t == Task::Explain) {
    if (hasMap() && selection.size() == 1) o.selectedItem = selection[0];
    if (hasMap() && clusterFilter >= 0) o.selectedCluster = clusterFilter;
  }
  if (t == Task::Document) o.document = aiDoc >= 0 ? aiDoc : docPreview;
  else if (aiDoc >= 0) o.document = aiDoc;
  return o;
}

void App::aiSend(Task t, const string& userText, const string& shown) {
  aiSyncTurns();
  if (aiLive || agent.active) { ui.toast("Assistant", "Wait for the current answer or stop it first.", 2, 3); return; }
  ai::Config cfg = aiConfig();
  if (!cfg.ready()) {
    setTab = 2;
    settingsWanted = true;
    ui.toast("Connect an AI provider", "Choose a provider and add an API key to use the assistant.", 0, 4);
    return;
  }
  if (!hasCorpus() && !hasMap() && t != Task::Chat && t != Task::Search) { ui.toast("Assistant", "Import records or open a map first.", 2, 3); return; }
  ai::ContextOpts co = aiContextOpts(t);
  if (t == Task::Explain && co.selectedItem < 0 && co.selectedCluster < 0) { ui.toast("Explain the selection", "Select an item on the map or a cluster in Analyse first.", 2, 4); return; }
  if (t == Task::Document && co.document < 0) { ui.toast("Summarise a document", "Open a document first: click a paper in Actors, Analyse or the inspector.", 2, 4); return; }
  if (t == Task::Methods && trim(userText).empty()) { aiSend(t, P->methods(), shown.empty() ? string("Methods paragraph") : shown); return; }
  string ctx = (hasCorpus() || hasMap()) ? ai::buildContext(*P, co) : "";
  string prompt = ai::taskPrompt(t, userText);
  string content = ctx.empty() ? prompt : "Research context from VOSStudio:\n\n" + ctx + "\n---\n\n" + prompt;
  // history: recent turns without their context blocks
  vector<ai::Msg> msgs;
  size_t start = aiTurns.size() > 12 ? aiTurns.size() - 12 : 0;
  for (size_t i = start; i < aiTurns.size(); i++) {
    const ai::Turn& x = aiTurns[i];
    if (x.role == "error" || x.text.empty()) continue;
    msgs.push_back({x.role == "user" ? "user" : "assistant", x.text});
  }
  msgs.push_back({"user", content});
  string disp = !shown.empty() ? shown : (t == Task::Chat ? userText : string(ai::taskInfo(t).title) + (trim(userText).empty() || t == Task::Methods ? "" : ": " + trim(userText)));
  aiTurns.push_back({"user", disp, ai::taskInfo(t).id, nowStamp()});
  aiTurns.push_back({"assistant", "", ai::taskInfo(t).id, nowStamp()});
  aiLiveTurn = int(aiTurns.size()) - 1;
  aiLiveTask = t;
  aiStickBottom = true;
  aiLive = aiRequest(ai::buildChat(cfg, ai::systemPrompt(), msgs, true));
}

// Sends a chat request on a worker thread; the reply streams into the returned object (text, err, done).
std::shared_ptr<App::AiStream> App::aiRequest(const ai::HttpReq& rq) {
  ai::Config cfg = aiConfig();
  auto st = std::make_shared<AiStream>();
  HWND h = hwnd;
  std::thread([st, rq, cfg, h]() {
    ai::StreamParser sp(cfg.provider);
    int status = 0;
    string err;
    double lastPost = 0;
    string body = httpRequest(rq.method, rq.url, rq.headers, rq.body, &status, &err, [&](const char* d, size_t n) {
      if (status >= 200 && status < 300) {
        sp.feed(d, n);
        std::lock_guard<std::mutex> lk(st->m);
        st->text = sp.text;
      }
      double t = nowSeconds();
      if (t - lastPost > 0.05) { PostMessageW(h, WM_NULL, 0, 0); lastPost = t; }
      return !st->cancel.load();
    });
    sp.finish();
    string text = sp.text, e;
    if (status < 200 || status >= 300) e = ai::describeError(cfg, status, body, err);
    else if (!sp.error.empty()) e = sp.error;
    else if (text.empty()) {  // server ignored "stream": plain JSON reply
      string pe;
      text = ai::parseChat(cfg, body, &pe);
      if (text.empty() && !st->cancel) e = pe.empty() ? string("The model returned an empty answer.") : pe;
    }
    if (sp.stopReason == "length" || sp.stopReason == "max_tokens" || sp.stopReason == "MAX_TOKENS") text += "\n\n*The answer was cut off at the length limit.*";
    {
      std::lock_guard<std::mutex> lk(st->m);
      st->text = text;
      st->err = st->cancel ? string() : e;
      st->done = true;
    }
    PostMessageW(h, WM_NULL, 0, 0);
  }).detach();
  return st;
}

// Structured replies (cluster names, search plans) are shown as readable Markdown instead of their raw JSON.
static string aiDisplayText(const ai::Turn& T, const Network* net) {
  if (T.task == "clean") {
    vector<ai::CleanProposal> v;
    if (!ai::parseCleanPlan(T.text, v) || v.empty()) return T.text;
    string md = "Proposed cleaning\n\nReview and apply these in **Data \xE2\x86\x92 Clean terms**. Proposals for terms that do not occur in the data are left out there.\n\n";
    for (auto& p : v) {
      if (p.ignore) md += "- Ignore **" + p.target + "**" + (p.reason.empty() ? string() : ": " + p.reason) + "\n";
      else md += "- **" + p.target + "** \xE2\x86\x90 " + join(p.members, ", ") + " *(" + p.kind + (p.reason.empty() ? string() : ", " + p.reason) + ")*\n";
    }
    return md;
  }
  if (T.task == "names") {
    auto v = ai::parseClusterNames(T.text);
    if (v.empty()) return T.text;
    string md = "Suggested cluster names\n\n| Cluster | Name | Summary |\n|---|---|---|\n";
    for (auto& cn : v) {
      md += "| " + std::to_string(cn.cluster + 1) + " | **" + replaceAll(cn.name, "|", "/") + "** | " + replaceAll(cn.summary, "|", "/") + " |\n";
    }
    return md;
  }
  if (T.task == "search") {
    ai::SearchPlan sp;
    if (!ai::parseSearchPlan(T.text, sp)) return T.text;
    string md = "Search plan for OpenAlex\n\n**Keyword search**\n\n`" + sp.keywords + "`\n\n";
    if (!sp.semantic.empty()) {
      md += "**Semantic descriptions**\n\n";
      for (auto& q : sp.semantic) md += "- " + q + "\n";
      md += "\n";
    }
    if (sp.fromYear > 0 || sp.toYear > 0)
      md += "**Years:** " + (sp.fromYear > 0 ? std::to_string(sp.fromYear) : string("any")) + " to " + (sp.toYear > 0 ? std::to_string(sp.toYear) : string("now")) + "\n";
    return md;
  }
  return T.text;
}

static bool s_aiFocusInput = false;
static std::map<size_t, string> s_aiShown;  // rendered text per turn, refreshed with aiLayout_  // focus the composer on the next frame (its id lives in the "assistant" scope)

void App::aiRun(Task t, const string& extra) {
  if (t == Task::Clean && trim(extra).empty()) { aiCleanTerms(cleanUnit < 0 ? 0 : cleanUnit); return; }
  page = PG_AI;
  if (t == Task::Search && trim(extra).empty()) {
    aiPending = Task::Search;
    s_aiFocusInput = true;
    return;
  }
  aiSend(t, extra);
}

void App::aiStop() {
  if (!aiLive) return;
  aiLive->cancel = true;
  std::lock_guard<std::mutex> lk(aiLive->m);
  if (aiLiveTurn >= 0 && aiLiveTurn < int(aiTurns.size())) {
    aiTurns[size_t(aiLiveTurn)].text = aiLive->text + (aiLive->text.empty() ? "*Stopped.*" : "\n\n*Stopped.*");
  }
  aiLive.reset();
  aiLiveTurn = -1;
  aiSaveTurns();
}

void App::aiPump() {
  if (!aiLive) return;
  ui.animating = true;
  string text, err;
  bool done;
  {
    std::lock_guard<std::mutex> lk(aiLive->m);
    text = aiLive->text;
    err = aiLive->err;
    done = aiLive->done;
  }
  if (aiLiveTurn < 0 || aiLiveTurn >= int(aiTurns.size())) { aiLive.reset(); return; }
  ai::Turn& T = aiTurns[size_t(aiLiveTurn)];
  T.text = text;
  if (!done) return;
  if (!err.empty()) { T.role = "error"; T.text = err; }
  Task t = aiLiveTask;
  aiLive.reset();
  aiLiveTurn = -1;
  aiSaveTurns();
  if (err.empty() && t == Task::NameClusters) aiApplyNames(T.text);
  if (err.empty() && t == Task::Clean) { aiApplyClean(T.text); return; }
  if (page != PG_AI && err.empty()) ui.toast("Assistant", string(ai::taskInfo(t).title) + " is ready.", 1, 3);
  if (!err.empty() && page != PG_AI) ui.toast("Assistant", err, 3, 6);
}

void App::aiNewChat() {
  if (aiLive) aiStop();
  aiTurns.clear();
  aiLayout_.clear();
  aiDoc = -1;
  aiPending = Task::Chat;
  aiSaveTurns();
}

void App::aiExport() {
  if (aiTurns.empty()) return;
  string path = saveFileDialog(hwnd, "Export conversation", {{"Markdown", "*.md"}, {"Text", "*.txt"}}, "VOSStudio assistant.md", "md");
  if (path.empty()) return;
  string md = "# VOSStudio assistant\n\n";
  if (hasMap()) md += "*Map: " + P->net.description + " (" + std::to_string(P->net.n()) + " items, " + std::to_string(P->net.nClusters) + " clusters)*\n\n";
  for (auto& t : aiTurns) {
    if (t.role == "user") md += "## " + t.text + "\n\n";
    else if (t.role == "assistant") md += t.text + "\n\n";
  }
  if (writeFileU(path, md)) ui.toast("Conversation exported", fileName(path), 1, 3);
  else ui.toast("Export failed", "Could not write " + path, 3);
}

bool App::aiApplyNames(const string& reply, bool quiet) {
  auto names = ai::parseClusterNames(reply);
  if (names.empty() || !hasMap()) { if (!quiet) ui.toast("Cluster names", "The answer did not contain names in the expected format.", 2); return false; }
  pushUndo("Name clusters with AI");
  if (int(P->net.clusterNames.size()) < P->net.nClusters) P->net.clusterNames.resize(size_t(P->net.nClusters));
  int n = 0;
  for (auto& c : names)
    if (c.cluster < P->net.nClusters) { P->net.clusterNames[size_t(c.cluster)] = c.name; n++; }
  P->style.clusterNames = true;
  styleDirty = true;
  P->dirty = true;
  figSig.clear();
  if (!quiet) ui.toast("Cluster names applied", std::to_string(n) + " clusters renamed. Undo with Ctrl+Z.", 1, 4);
  return n > 0;
}

bool App::aiApplySearch(const string& reply) {
  ai::SearchPlan sp;
  if (!ai::parseSearchPlan(reply, sp)) { ui.toast("OpenAlex search", "The answer did not contain a search plan.", 2); return false; }
  if (!sp.semantic.empty()) {
    oaKind = 1;
    oaSemQueries = sp.semantic;
    if (oaSemQueries.size() > 5) oaSemQueries.resize(5);
  } else oaKind = 0;
  if (!sp.keywords.empty()) oaQuery = sp.keywords;
  oaFrom = sp.fromYear > 0 ? std::to_string(sp.fromYear) : "";
  oaTo = sp.toYear > 0 ? std::to_string(sp.toYear) : "";
  page = PG_DATA;
  ui.scrollTo("page0", 1e6f);
  ui.toast("Search prepared", sp.semantic.empty() ? "Keyword query filled in. Review it and choose Fetch Works." : "Semantic descriptions and keywords filled in. Review them and choose Fetch Works.", 1, 5);
  return true;
}

// ================================================================= AI cleaning agent
void App::aiCleanTerms(int unitIdx) {
  if (!hasCorpus()) { ui.toast("Clean terms", "Import records first.", 2, 3); return; }
  const auto& cu = cleanUnits();
  unitIdx = clampv(unitIdx, 0, int(cu.size()) - 1);
  auto terms = termCounts(P->corpus, cu[size_t(unitIdx)].first, P->engine.thesaurus);
  if (terms.size() < 2) { ui.toast("Clean terms", string("No ") + lower(cu[size_t(unitIdx)].second) + " to clean in this data set.", 2, 3); return; }
  cleanUnit = unitIdx;
  size_t n = std::min<size_t>(300, terms.size());
  string shown = string("Clean the terms: ") + cu[size_t(unitIdx)].second + " (" + (n < terms.size() ? "top " + fmtInt(long(n)) + " of " : "") + plural(long(terms.size()), "term") + ")";
  aiSend(Task::Clean, ai::cleanTermList(terms, n), shown);
}

bool App::aiApplyClean(const string& reply) {
  vector<ai::CleanProposal> props;
  if (!ai::parseCleanPlan(reply, props)) { ui.toast("AI cleaning", "The answer did not contain proposals in the expected format.", 2, 4); return false; }
  if (!hasCorpus()) return false;
  const auto& cu = cleanUnits();
  Unit u = cu[size_t(clampv(cleanUnit, 0, int(cu.size()) - 1))].first;
  std::unordered_map<string, std::pair<string, int>> have;  // thesaurus key -> (label, count)
  for (auto& t : termCounts(P->corpus, u, P->engine.thesaurus)) have[thesaurusKey(u, t.first)] = t;
  vector<VariantGroup> ai;
  int merges = 0, generic = 0, dropped = 0;
  for (auto& p : props) {
    VariantGroup g;
    g.ai = true;
    g.note = p.reason;
    g.reason = p.kind == "abbreviation" ? "abbrev." : p.kind;
    auto tt = have.find(thesaurusKey(u, p.target));
    if (p.ignore) {
      if (tt == have.end()) { dropped++; continue; }
      g.target = tt->second.first;
      g.targetCount = tt->second.second;
      g.ignore = true;
      g.safe = false;
      g.apply = false;  // removing a term is opt-in
      ai.push_back(g);
      generic++;
      continue;
    }
    g.target = tt != have.end() ? tt->second.first : bibClean(p.target);
    g.targetCount = tt != have.end() ? tt->second.second : 0;
    std::set<string> seen{thesaurusKey(u, g.target)};
    for (auto& m : p.members) {
      auto mt = have.find(thesaurusKey(u, m));
      if (mt == have.end()) { dropped++; continue; }
      if (!seen.insert(mt->first).second) continue;
      g.members.push_back(mt->second.first);
      g.counts.push_back(mt->second.second);
    }
    if (g.members.empty()) continue;
    g.safe = true;
    g.apply = true;
    ai.push_back(g);
    merges++;
  }
  // AI proposals first, then the rule-based variants of the same field
  vector<VariantGroup> rules = findVariants(P->corpus, u, P->engine.thesaurus);
  variants = ai;
  variants.insert(variants.end(), rules.begin(), rules.end());
  variantsScanned = true;
  cleanAiUnit = cleanUnit;
  cleanEdit = -1;
  string summary = plural(merges, "merge") + " and " + plural(generic, "generic term") + " proposed";
  if (dropped) summary += " (" + std::to_string(dropped) + " not in the data, left out)";
  cleanNote = ai.empty() ? "The AI found nothing to clean." : string();
  ui.toast("AI cleaning", summary + ". Review them in Data.", 0, 5);
  return !ai.empty();
}

// ================================================================= Markdown
float App::aiMarkdown(float x, float y, float w, const string& md, bool draw) {
  float s = ui.s, y0 = y;
  const float fs = 13.5f * s;
  vector<string> lines = split(replaceAll(md, "\r", ""), '\n', true);
  auto isBullet = [](const string& l, size_t& ind, size_t& skip) {
    size_t i = 0;
    while (i < l.size() && l[i] == ' ') i++;
    ind = i / 2;
    if (i + 1 < l.size() && (l[i] == '-' || l[i] == '*' || l[i] == '+') && l[i + 1] == ' ') { skip = i + 2; return true; }
    if (l.compare(i, 4, "\xE2\x80\xA2 ") == 0) { skip = i + 4; return true; }
    return false;
  };
  auto isNumbered = [](const string& l, size_t& ind, size_t& skip, string& num) {
    size_t i = 0;
    while (i < l.size() && l[i] == ' ') i++;
    ind = i / 2;
    size_t j = i;
    while (j < l.size() && isdigit((unsigned char)l[j])) j++;
    if (j > i && j + 1 < l.size() && (l[j] == '.' || l[j] == ')') && l[j + 1] == ' ') { num = l.substr(i, j - i + 1); skip = j + 2; return true; }
    return false;
  };
  bool first = true;
  for (size_t i = 0; i < lines.size();) {
    string l = lines[i];
    string t = trim(l);
    if (t.empty()) { i++; continue; }
    if (!first) y += 8 * s;
    first = false;
    // fenced code
    if (t.rfind("```", 0) == 0) {
      string code;
      i++;
      while (i < lines.size() && trim(lines[i]).rfind("```", 0) != 0) { code += (code.empty() ? "" : "\n") + lines[i]; i++; }
      i++;
      float h = ui.textWrap({x + 12 * s, y + 10 * s, w - 24 * s, 10000}, code.empty() ? " " : code, 12.5f * s, ui.c.text, 400, false);
      if (draw) {
        ui.fill({x, y, w, h + 20 * s}, ui.c.text.withA(ui.dark ? 0.07f : 0.045f), 8 * s);
        ui.textWrap({x + 12 * s, y + 10 * s, w - 24 * s, 10000}, code.empty() ? " " : code, 12.5f * s, ui.c.text);
      }
      y += h + 20 * s;
      continue;
    }
    // heading
    if (t[0] == '#') {
      size_t k = 0;
      while (k < t.size() && t[k] == '#') k++;
      string ht = trim(t.substr(k));
      float hs = k <= 1 ? 18 * s : k == 2 ? 15.5f * s : 13.5f * s;
      if (y > y0) y += (k <= 2 ? 8 : 4) * s;
      y += ui.richText({x, y, w, 0}, ht, hs, ui.c.text, 650, draw, 1.25f) + 2 * s;
      i++;
      continue;
    }
    // horizontal rule
    if (t == "---" || t == "***" || t == "___") {
      if (draw) ui.line(x, y + 6 * s, x + w, y + 6 * s, ui.c.border);
      y += 12 * s;
      i++;
      continue;
    }
    // table
    if (t[0] == '|') {
      vector<vector<string>> rows;
      while (i < lines.size() && !trim(lines[i]).empty() && trim(lines[i])[0] == '|') {
        string row = trim(lines[i]);
        i++;
        if (row.find("---") != string::npos && row.find_first_not_of("|-: ") == string::npos) continue;
        vector<string> cells = split(row.substr(1, row.size() > 1 && row.back() == '|' ? row.size() - 2 : row.size() - 1), '|', true);
        for (auto& c : cells) c = trim(c);
        rows.push_back(cells);
      }
      size_t nc = 0;
      for (auto& r : rows) nc = std::max(nc, r.size());
      if (!nc) continue;
      float cw = w / float(nc);
      float ty = y;
      for (size_t r = 0; r < rows.size(); r++) {
        float rh = 0;
        for (size_t c = 0; c < nc; c++) rh = std::max(rh, ui.richText({x + cw * c + 8 * s, ty + 6 * s, cw - 16 * s, 0}, c < rows[r].size() ? rows[r][c] : "", 12.5f * s, ui.c.text, r == 0 ? 600 : 400, false));
        rh += 12 * s;
        if (draw) {
          if (r == 0) ui.fill({x, ty, w, rh}, ui.c.text.withA(ui.dark ? 0.06f : 0.035f), 0);
          for (size_t c = 0; c < nc; c++) ui.richText({x + cw * c + 8 * s, ty + 6 * s, cw - 16 * s, 0}, c < rows[r].size() ? rows[r][c] : "", 12.5f * s, ui.c.text, r == 0 ? 600 : 400, true);
          ui.line(x, ty + rh - 0.5f, x + w, ty + rh - 0.5f, ui.c.border);
        }
        ty += rh;
      }
      if (draw) ui.stroke({x, y, w, ty - y}, ui.c.border, 6 * s);
      y = ty;
      continue;
    }
    // quote
    if (t[0] == '>') {
      string q;
      while (i < lines.size() && !trim(lines[i]).empty() && trim(lines[i])[0] == '>') { q += (q.empty() ? "" : " ") + trim(trim(lines[i]).substr(1)); i++; }
      float h = ui.richText({x + 14 * s, y, w - 14 * s, 0}, q, fs, ui.c.textDim, 400, draw);
      if (draw) ui.fill({x + 2 * s, y + 2 * s, 3 * s, h - 2 * s}, ui.c.borderStrong, 1.5f * s);
      y += h;
      continue;
    }
    // lists (consecutive items share tight spacing)
    size_t ind = 0, skip = 0;
    string num;
    bool bul = isBullet(l, ind, skip), numd = !bul && isNumbered(l, ind, skip, num);
    if (bul || numd) {
      bool firstItem = true;
      while (i < lines.size()) {
        string li = lines[i];
        size_t in2 = 0, sk2 = 0;
        string n2;
        bool b2 = isBullet(li, in2, sk2), m2 = !b2 && isNumbered(li, in2, sk2, n2);
        if (!b2 && !m2) break;
        string body = li.substr(sk2);
        i++;
        // agent step lines start with a status mark: drawn as an icon instead of the bullet
        int mark = 0;  // 1 done, 2 failed, 3 declined, 4 waiting, 5 running
        if (b2) {
          static const char* marks[5] = {"\xE2\x9C\x93 ", "\xE2\x9C\x97 ", "\xE2\x80\x93 ", "? ", "\xE2\x80\xA6 "};
          for (int k = 0; k < 5; k++) if (body.compare(0, strlen(marks[k]), marks[k]) == 0) { mark = k + 1; body = body.substr(strlen(marks[k])); break; }
        }
        // continuation lines (indented, not a new item)
        while (i < lines.size() && !trim(lines[i]).empty() && lines[i].size() > 1 && lines[i][0] == ' ' && !isBullet(lines[i], in2, sk2) && !isNumbered(lines[i], in2, sk2, n2)) { body += " " + trim(lines[i]); i++; }
        if (!firstItem) y += 4 * s;
        firstItem = false;
        float lx = x + float(std::min<size_t>(in2, 3)) * 18 * s;
        float bw = m2 ? std::max(18 * s, ui.textW(n2, fs) + 6 * s) : 16 * s;
        float h = ui.richText({lx + bw, y, w - (lx - x) - bw, 0}, trim(body), fs, ui.c.text, 400, draw);
        if (draw) {
          float cy = y + fs * 0.68f;
          if (m2) ui.text({lx, y, bw, fs * 1.35f}, n2, fs, ui.c.textDim, AL_LEFT);
          else if (mark == 1) ui.icon("check", lx + 6 * s, cy, 12 * s, ui.c.ok, 2.f);
          else if (mark == 2) ui.icon("x", lx + 6 * s, cy, 10 * s, ui.c.danger, 2.f);
          else if (mark == 3) ui.icon("minus", lx + 6 * s, cy, 10 * s, ui.c.textFaint, 2.f);
          else if (mark == 4) ui.circle(lx + 6 * s, cy, 4 * s, ui.c.accent);
          else if (mark == 5) { ui.circle(lx + 6 * s, cy, 4.5f * s, ui.c.accent.withA(0.25f)); ui.circle(lx + 6 * s, cy, 2.3f * s, ui.c.accent); }
          else ui.circle(lx + 6 * s, cy, 2.3f * s, ui.c.textDim);
        }
        y += h;
      }
      continue;
    }
    // paragraph: join until a blank line or a block start
    string para = t;
    i++;
    while (i < lines.size()) {
      string nx = trim(lines[i]);
      size_t a, b;
      string c;
      if (nx.empty() || nx[0] == '#' || nx[0] == '|' || nx[0] == '>' || nx.rfind("```", 0) == 0 || isBullet(lines[i], a, b) || isNumbered(lines[i], a, b, c)) break;
      para += " " + nx;
      i++;
    }
    y += ui.richText({x, y, w, 0}, para, fs, ui.c.text, 400, draw);
  }
  return y - y0;
}

// ================================================================= main workspace
void App::drawAssistant(const Rect& R) {
  float s = ui.s;
  aiSyncTurns();
  ai::Config cfg = aiConfig();
  bool ready = cfg.ready();
  ui.fill(R, ui.c.bg);
  ui.pushId("assistant");
  // header
  Rect hdr{R.x, R.y, R.w, 52 * s};
  ui.text({hdr.x + 20 * s, hdr.y + 8 * s, 300 * s, 20 * s}, "Assistant", 15 * s, ui.c.text, AL_LEFT, 650);
  string sub = ready ? string(ai::providerName(cfg.provider)) + " \xC2\xB7 " + cfg.effectiveModel() : "Not connected";
  ui.circle(hdr.x + 24 * s, hdr.y + 36 * s, 3.2f * s, ready ? ui.c.ok : ui.c.textFaint);
  ui.text({hdr.x + 32 * s, hdr.y + 28 * s, 420 * s, 16 * s}, sub, 11.5f * s, ui.c.textDim);
  float bx = hdr.r() - 16 * s;
  auto hbtn = [&](const string& label, bool en) { float bw = ui.textW(label, 12.5f * s, 500) + 24 * s; bx -= bw; Rect b{bx, hdr.y + 12 * s, bw, 28 * s}; bx -= 8 * s; return ui.button(b, label, BTN_NORMAL, "", en); };
  if (hbtn("Settings\xE2\x80\xA6", true)) { setTab = 2; settingsWanted = true; }
  if (hbtn("Export\xE2\x80\xA6", !aiTurns.empty())) aiExport();
  if (hbtn("New Chat", !aiTurns.empty())) aiNewChat();
  ui.line(R.x, hdr.b() - 0.5f, R.r(), hdr.b() - 0.5f, ui.c.border);

  float colW = std::min(760 * s, R.w - 64 * s);
  float cx = std::round(R.x + (R.w - colW) / 2);
  // composer geometry (grows with the text)
  static float composerTextH = 0;
  float taH = clampv(composerTextH, 46 * s, 170 * s);
  float chipsH = 30 * s;
  float compH = chipsH + taH + 34 * s;
  Rect comp{R.x, R.b() - compH, R.w, compH};
  Rect body{R.x, hdr.b(), R.w, comp.y - hdr.b()};

  // ---- conversation
  ui.popId();  // the scroll area lives at the root so ui.scrollSet("aichat") reaches it
  ui.beginScroll("aichat", body);
  ui.pushId("assistant");
  float sy = ui.scrollY();
  float y = body.y + 24 * s - sy;
  float yStart = y;
  if (aiTurns.empty()) {
    float ey = body.y + std::max(30 * s, body.h * 0.14f);
    ui.circle(cx + colW / 2, ey + 22 * s, 24 * s, ui.c.accent.withA(ui.dark ? 0.2f : 0.1f));
    ui.icon("sparkle", cx + colW / 2, ey + 22 * s, 24 * s, ui.c.accent, 1.7f);
    ey += 60 * s;
    if (!ready) {
      ui.text({cx, ey, colW, 26 * s}, "Connect an AI Provider", 19 * s, ui.c.text, AL_CENTER, 650);
      ey += 34 * s;
      float th = ui.textWrap({cx + colW * 0.12f, ey, colW * 0.76f, 200}, "Use OpenAI, Google Gemini, Anthropic Claude or any OpenAI-compatible service, including local models through Ollama or LM Studio. Your key is encrypted and stays on this computer.", 13 * s, ui.c.textDim, 400, false);
      Rect tr{cx + colW / 2 - std::min(colW * 0.76f, 520 * s) / 2, ey, std::min(colW * 0.76f, 520 * s), th + 60};
      th = ui.textWrap(tr, "Use OpenAI, Google Gemini, Anthropic Claude or any OpenAI-compatible service, including local models through Ollama or LM Studio. Your key is encrypted and stays on this computer.", 13 * s, ui.c.textDim);
      ey += th + 22 * s;
      if (ui.button({cx + colW / 2 - 80 * s, ey, 160 * s, 32 * s}, "Set Up AI\xE2\x80\xA6", BTN_PRIMARY)) { setTab = 2; settingsWanted = true; }
      ey += 60 * s;
    } else {
      ui.text({cx, ey, colW, 26 * s}, "What would you like to know?", 19 * s, ui.c.text, AL_CENTER, 650);
      ey += 32 * s;
      string ctxLine = hasMap() ? "The assistant reads a summary of your " + fmtInt(long(P->corpus.recs.size())) + " records and the current map of " + fmtInt(P->net.n()) + " " + P->net.unitNoun + "s."
                     : hasCorpus() ? "The assistant reads a summary of your " + fmtInt(long(P->corpus.recs.size())) + " records." : "Import records to ground the answers in your data, or plan a search.";
      ui.text({cx, ey, colW, 18 * s}, ctxLine, 12.5f * s, ui.c.textDim, AL_CENTER);
      ey += 40 * s;
    }
    // suggestion tiles
    Task tiles[8] = {Task::Agent, Task::Overview, Task::Review, Task::Gaps, Task::Emerging, Task::NameClusters, Task::Clean, Task::Search};
    float gap = 10 * s, tw = (colW - gap) / 2, th = 64 * s;
    for (int k = 0; k < 8; k++) {
      ai::TaskInfo ti = ai::taskInfo(tiles[k]);
      if (tiles[k] == Task::Agent) { ti.title = "Work with the agent"; ti.sub = "Give it a goal: it builds, checks and explains the map"; ti.icon = "agent"; ti.id = "agent"; }
      Rect tr{cx + (k % 2) * (tw + gap), ey + (k / 2) * (th + gap), tw, th};
      bool en = ready && (tiles[k] == Task::Search || (tiles[k] == Task::NameClusters ? hasMap() : tiles[k] == Task::Clean ? hasCorpus() : (hasCorpus() || hasMap())));
      uint64_t tid = ui.id(string("tile:") + ti.id);
      bool hov = false;
      bool clicked = ui.behave(tid, tr, &hov) && en;
      ui.fill(tr, hov && en ? ui.c.hover : ui.c.card, 10 * s);
      ui.stroke(tr, ui.c.border, 10 * s);
      ui.icon(ti.icon, tr.x + 22 * s, tr.y + 22 * s, 16 * s, en ? ui.c.accent : ui.c.textFaint, 1.7f);
      ui.text({tr.x + 40 * s, tr.y + 10 * s, tr.w - 50 * s, 20 * s}, ti.title, 13 * s, en ? ui.c.text : ui.c.textFaint, AL_LEFT, 600);
      ui.text({tr.x + 40 * s, tr.y + 31 * s, tr.w - 50 * s, 18 * s}, truncate(ti.sub, 60), 11.5f * s, ui.c.textDim);
      if (clicked) {
        if (tiles[k] == Task::Agent) { agentMode = true; aiPending = Task::Chat; s_aiFocusInput = true; }
        else aiRun(tiles[k]);
      }
    }
    y = ey + 4 * (th + gap) + 20 * s;
  } else {
    for (size_t i = 0; i < aiTurns.size(); i++) {
      const ai::Turn& T = aiTurns[i];
      bool live = int(i) == aiLiveTurn || (agent.active && int(i) == agent.turn);
      ui.pushId("t" + std::to_string(i));
      if (T.role == "user") {
        float maxW = colW * 0.8f;
        float tw = std::min(maxW - 28 * s, ui.textW(T.text.size() > 400 ? T.text.substr(0, 400) : T.text, 13.5f * s) + 2 * s);
        float th = ui.textWrap({0, 0, tw, 10000}, T.text, 13.5f * s, ui.c.text, 400, false);
        Rect b{cx + colW - tw - 28 * s, y, tw + 28 * s, th + 20 * s};
        if (b.b() > body.y && b.y < body.b()) {
          ui.fill(b, ui.c.text.withA(ui.dark ? 0.1f : 0.06f), 14 * s);
          ui.textWrap({b.x + 14 * s, b.y + 10 * s, tw, th + 4}, T.text, 13.5f * s, ui.c.text);
        }
        y += b.h + 18 * s;
      } else if (T.role == "error") {
        float th = ui.textWrap({0, 0, colW - 60 * s, 10000}, T.text, 12.5f * s, ui.c.text, 400, false);
        Rect b{cx, y, colW, th + 58 * s};
        ui.fill(b, ui.c.danger.withA(ui.dark ? 0.14f : 0.07f), 10 * s);
        ui.stroke(b, ui.c.danger.withA(0.35f), 10 * s);
        ui.icon("warn", b.x + 20 * s, b.y + 20 * s, 15 * s, ui.c.danger, 1.8f);
        ui.textWrap({b.x + 38 * s, b.y + 11 * s, colW - 60 * s, th + 4}, T.text, 12.5f * s, ui.c.text);
        float bxx = b.x + 38 * s;
        Rect r1{bxx, b.b() - 36 * s, 86 * s, 26 * s};
        if (ui.button(r1, "Try Again", BTN_NORMAL, "", !aiLive) && i >= 1 && aiTurns[i - 1].role == "user") {
          string task = aiTurns[i - 1].task, q = aiTurns[i - 1].text;
          Task tk = Task::Chat;
          for (auto& x : ai::tasks()) if (task == x.id) tk = x.task;
          aiTurns.erase(aiTurns.begin() + long(i - 1), aiTurns.begin() + long(i + 1));
          aiLayout_.clear();
          string extra = tk == Task::Chat ? q : (q.find(": ") != string::npos ? q.substr(q.find(": ") + 2) : string());
          ui.popId();
          ui.endScroll(y - yStart + 60 * s);
          ui.popId();
          aiSend(tk, extra, q);
          return;
        }
        if (ui.button({r1.r() + 8 * s, r1.y, 100 * s, 26 * s}, "Open Settings", BTN_GHOST)) { setTab = 2; settingsWanted = true; }
        y += b.h + 18 * s;
      } else {
        // assistant message: avatar + Markdown, then actions
        float mx = cx + 34 * s, mw = colW - 34 * s;
        AiLayout& lay = aiLayout_[i];
        float h;
        bool vis = true;
        bool structured = T.task == "names" || T.task == "search";
        if (!live && lay.len == T.text.size() && lay.w == mw) {
          h = lay.h;
          vis = y + h > body.y - 40 * s && y < body.b() + 40 * s;
        } else {
          if (!live || !structured) {
            s_aiShown[i] = live ? T.text : aiDisplayText(T, hasMap() ? &P->net : nullptr);
            h = aiMarkdown(mx, y, mw, s_aiShown[i], false);
          } else h = 22 * s;
          lay.len = T.text.size(); lay.w = mw; lay.h = h;
        }
        const string& shownText = s_aiShown[i];
        if (y < body.b() && y + 28 * s > body.y) {
          ui.circle(cx + 12 * s, y + 11 * s, 12 * s, ui.c.accent.withA(ui.dark ? 0.22f : 0.12f));
          ui.icon("sparkle", cx + 12 * s, y + 11 * s, 13 * s, ui.c.accent, 1.6f);
        }
        if (live && (T.text.empty() || structured)) {
          // thinking indicator (structured replies are shown once complete)
          double tt = ui.time * 3;
          for (int k = 0; k < 3; k++) ui.circle(mx + 6 * s + k * 12 * s, y + 11 * s, 3 * s, ui.c.textDim.withA(float(0.35 + 0.65 * std::max(0.0, std::sin(tt - k * 0.7)))));
          string what = T.text.empty() ? "Thinking\xE2\x80\xA6" : T.task == "names" ? "Writing cluster names\xE2\x80\xA6" : "Writing the search plan\xE2\x80\xA6";
          ui.text({mx + 44 * s, y + 2 * s, 300 * s, 18 * s}, what, 12.5f * s, ui.c.textFaint);
          ui.animating = true;
          h = 22 * s;
        } else if (vis) {
          aiMarkdown(mx, y, mw, live ? T.text : shownText, true);
        }
        y += h + 8 * s;
        if (live && T.task == "agent" && agent.waitApproval) {
          // the agent wants to change the map, the settings or the thesaurus
          string what = agentDescribe(agent.pending);
          string why = trim(agent.pending.thought);
          float tw2 = mw - 36 * s;
          float wh = why.empty() ? 0 : ui.textWrap({0, 0, tw2, 1000}, why, 12 * s, ui.c.textDim, 400, false) + 4 * s;
          Rect cb{mx, y, mw, 104 * s + wh};
          ui.fill(cb, ui.c.card, 10 * s);
          ui.stroke(cb, ui.c.accent.withA(0.55f), 10 * s);
          ui.text({cb.x + 18 * s, cb.y + 12 * s, tw2, 18 * s}, "Allow this change?", 13 * s, ui.c.text, AL_LEFT, 650);
          ui.text({cb.x + 18 * s, cb.y + 32 * s, tw2, 18 * s}, truncate(what, 110), 12.5f * s, ui.c.text);
          if (!why.empty()) ui.textWrap({cb.x + 18 * s, cb.y + 52 * s, tw2, wh}, why, 12 * s, ui.c.textDim);
          float by = cb.b() - 42 * s, bx2 = cb.x + 18 * s;
          auto cbtn = [&](const string& label, int kind, const string& tip) {
            float bw = ui.textW(label, 12.5f * s, 500) + 26 * s;
            Rect b{bx2, by, bw, 28 * s};
            bx2 += bw + 8 * s;
            bool c = ui.button(b, label, kind);
            if (!tip.empty()) ui.tipFor(ui.id("btn:" + label + std::to_string(int(b.x)) + ":" + std::to_string(int(b.y))), tip);
            return c;
          };
          if (cbtn("Allow", BTN_PRIMARY, "")) agentApprove(true);
          else if (cbtn("Allow All Changes", BTN_NORMAL, "Allow the rest of this task's changes without asking")) agentApprove(true, true);
          else if (cbtn("Decline", BTN_GHOST, "")) agentApprove(false);
          ui.text({cb.r() - 220 * s, by, 204 * s, 28 * s}, "You can undo the agent's changes", 11 * s, ui.c.textFaint, AL_RIGHT);
          y += cb.h + 10 * s;
        }
        if (!live) {
          // action row
          float ax = mx;
          auto act = [&](const string& ic, const string& label, bool primary = false) {
            float bw = ui.textW(label, 12 * s, primary ? 600 : 500) + (ic.empty() ? 22 : 40) * s;
            Rect b{ax, y, bw, 26 * s};
            ax += bw + 6 * s;
            uint64_t aid = ui.id("act:" + label);
            bool hov = false;
            bool c = ui.behave(aid, b, &hov);
            if (primary) { ui.fill(b, hov ? ui.c.accentHover : ui.c.accent, 6 * s); }
            else if (hov) ui.fill(b, ui.c.hover, 6 * s);
            Color fg = primary ? ui.c.accentText : ui.c.textDim;
            if (!ic.empty()) ui.icon(ic, b.x + 16 * s, b.y + 13 * s, 13 * s, fg, 1.7f);
            ui.text({b.x + (ic.empty() ? 10 : 28) * s, b.y, bw, b.h}, label, 12 * s, fg, AL_LEFT, primary ? 600 : 500);
            return c;
          };
          string task = T.task;
          if (task == "names" && hasMap() && act("tag", "Apply Names to Map", true)) aiApplyNames(T.text);
          if (task == "search" && act("search", "Use in OpenAlex Search", true)) aiApplySearch(T.text);
          if (task == "agent" && agentReportTurn == int(i) && !agentReportPath.empty()) {
            if (act("file", "Open Report", true)) openUrl(agentReportPath);
            if (act("folder", "Show in Folder")) revealInExplorer(agentReportPath);
          }
          if (task == "agent" && agentUndo.valid && agentUndo.turn == int(i) && act("undo", "Undo Agent Changes")) agentUndoChanges();
          if (task == "clean" && hasCorpus() && act("text", "Review in Data", true)) { aiApplyClean(T.text); page = PG_DATA; }
          if (act("copy", "Copy")) { setClipboardText(hwnd, replaceAll(structured ? shownText : T.text, "\n", "\r\n")); ui.toast("Copied", "", 1, 1.5); }
          if (i + 1 == aiTurns.size() && i >= 1 && aiTurns[i - 1].role == "user" && !aiLive && act("refresh", "Regenerate")) {
            string tsk = aiTurns[i - 1].task, q = aiTurns[i - 1].text;
            Task tk = Task::Chat;
            for (auto& x : ai::tasks()) if (tsk == x.id) tk = x.task;
            aiTurns.erase(aiTurns.begin() + long(i - 1), aiTurns.end());
            aiLayout_.clear();
            if (tsk == "agent") {
              if (agentUndo.turn >= int(i) - 1) agentUndo.turn = -2;  // keeps the undo, detached from the erased turn
              ui.popId();
              ui.endScroll(y - yStart + 60 * s);
              ui.popId();
              agentStart(q);
              return;
            }
            string extra = tk == Task::Chat ? q : (q.find(": ") != string::npos ? q.substr(q.find(": ") + 2) : string());
            ui.popId();
            ui.endScroll(y - yStart + 60 * s);
            ui.popId();
            aiSend(tk, extra, q);
            return;
          }
          y += 26 * s;
        }
        y += 22 * s;
      }
      ui.popId();
    }
  }
  float contentH = y - yStart + 30 * s;
  ui.endScroll(contentH);
  // follow the end of the conversation while the view sits at the bottom; scrolling up stops following
  if (sy + body.h >= contentH - 12 * s) aiStickBottom = true;
  if (ui.in.wheel > 0 && body.has(ui.in.mx, ui.in.my)) aiStickBottom = false;
  if (aiStickBottom && contentH > body.h) ui.scrollSet("aichat", contentH);

  // ---- composer
  ui.fill(comp, ui.c.bg);
  float y2 = comp.y + 4 * s;
  // context chips
  {
    float chx = cx;
    auto chip = [&](const string& label, bool& on, bool avail) {
      if (!avail) return;
      float w = ui.textW(label, 11.5f * s, 500) + 26 * s;
      Rect r{chx, y2, w, 22 * s};
      chx += w + 6 * s;
      uint64_t cid = ui.id("chip:" + label);
      bool hov = false;
      if (ui.behave(cid, r, &hov)) on = !on;
      ui.fill(r, on ? ui.c.text.withA(ui.dark ? 0.12f : 0.07f) : (hov ? ui.c.hover : Color(0, 0, 0, 0)), 11 * s);
      ui.stroke(r, on ? Color(0, 0, 0, 0) : ui.c.border, 11 * s);
      ui.circle(r.x + 11 * s, r.y + 11 * s, 2.6f * s, on ? ui.c.accent : ui.c.textFaint);
      ui.text({r.x + 18 * s, r.y, w, r.h}, label, 11.5f * s, on ? ui.c.text : ui.c.textFaint, AL_LEFT, 500);
      ui.tipFor(cid, on ? "Included in the next request. Click to leave it out." : "Left out. Click to include it.");
    };
    {
      // Ask / Agent switch
      const char* modes[2] = {"Ask", "Agent"};
      for (int k = 0; k < 2; k++) {
        float w = ui.textW(modes[k], 11.5f * s, 600) + 22 * s;
        Rect r{chx, y2, w, 22 * s};
        chx += w + 2 * s;
        uint64_t mid = ui.id(string("mode:") + modes[k]);
        bool hov = false;
        bool on = agentMode == (k == 1);
        if (ui.behave(mid, r, &hov) && !agent.active) { agentMode = k == 1; if (agentMode) aiPending = Task::Chat; s_aiFocusInput = true; }
        ui.fill(r, on ? ui.c.accent.withA(ui.dark ? 0.24f : 0.13f) : (hov ? ui.c.hover : Color(0, 0, 0, 0)), 11 * s);
        ui.text({r.x, r.y, r.w, r.h}, modes[k], 11.5f * s, on ? ui.c.accent : ui.c.textDim, AL_CENTER, 600);
        ui.tipFor(mid, k == 0 ? "Ask questions about your data and the map" : "Give a goal: the agent uses the app's tools step by step, from searching the literature to a finished PDF report");
      }
      chx += 12 * s;
    }
    if (agentMode) {
      ui.text({chx, y2, cx + colW - chx, 22 * s}, agent.active ? (agent.waitApproval ? "Waiting for your approval" : agent.waitJob ? "Waiting for a task to finish\xE2\x80\xA6" : "Working\xE2\x80\xA6") : agentAuto() ? "Works on its own: searches, builds maps and writes reports." : "Uses the app's tools. Asks before changing anything.", 11.5f * s, ui.c.textFaint);
      if (agent.active) ui.animating = agent.req != nullptr;
    }
    if (!agentMode) {
    ui.text({chx, y2, 60 * s, 22 * s}, "Context", 11.5f * s, ui.c.textFaint);
    chx += 54 * s;
    chip("Data set", aiCtxCorpus, hasCorpus());
    chip("Map", aiCtxMap, hasMap());
    chip("Trends", aiCtxTrends, hasCorpus());
    chip("Most cited", aiCtxDocs, hasCorpus());
    string selLabel = hasMap() && selection.size() == 1 ? "Selected: " + truncate(P->net.nodes[size_t(selection[0])].label, 24)
                    : hasMap() && clusterFilter >= 0 ? "Cluster " + std::to_string(clusterFilter + 1) : "";
    if (!selLabel.empty()) chip(selLabel, aiCtxSel, true);
    if (aiDoc >= 0 && aiDoc < int(P->corpus.recs.size())) {
      bool on = true;
      chip("Document: " + truncate(P->corpus.recs[size_t(aiDoc)].title, 26), on, true);
      if (!on) aiDoc = -1;
    }
    // approximate size of the context
    string key = std::to_string(P->corpusVersion) + ":" + std::to_string(P->net.n()) + ":" + std::to_string(aiCtxCorpus) + std::to_string(aiCtxMap) + std::to_string(aiCtxTrends) +
                 std::to_string(aiCtxDocs) + std::to_string(aiCtxSel) + ":" + selLabel + ":" + std::to_string(aiDoc);
    if (key != aiCtxKey_ && (hasCorpus() || hasMap())) { aiCtxKey_ = key; aiCtxLen_ = ai::buildContext(*P, aiContextOpts(Task::Chat)).size(); }
    if ((hasCorpus() || hasMap()) && aiPending == Task::Chat) ui.text({cx + colW - 140 * s, y2, 140 * s, 22 * s}, "\xE2\x89\x88 " + fmtInt(long(aiCtxLen_ / 4)) + " tokens", 11 * s, ui.c.textFaint, AL_RIGHT);
    }
  }
  y2 += chipsH;
  Rect ta{cx, y2, colW, taH};
  string ph = agentMode ? "Describe a goal, for example: map the author keywords, check that the clusters are stable and name them"
            : aiPending == Task::Search ? "Describe your research question, for example: large language models as tutors in higher education"
            : aiPending == Task::Explain ? "What would you like to know about the selection?"
            : !ready ? "Set up an AI provider in Settings to start" : "Ask about your data, the map or the literature";
  bool submit = false;
  float need = 0;
  if (s_aiFocusInput) { ui.focus = ui.id("ta:ai:input"); s_aiFocusInput = false; }
  ui.textArea({ta.x, ta.y, ta.w, ta.h}, "ai:input", aiInput, ph, &submit, &need);
  composerTextH = need + 4 * s;
  // send / stop
  Rect sb{ta.r() - 38 * s, ta.b() - 38 * s, 30 * s, 30 * s};
  uint64_t sid = ui.id("send");
  bool sh = false;
  bool canSend = ready && !trim(aiInput).empty();
  bool sc = ui.behave(sid, sb, &sh);
  if (aiLive || agent.active) {
    ui.circle(sb.x + sb.w / 2, sb.y + sb.h / 2, 15 * s, sh ? ui.c.text : ui.c.text.withA(0.85f));
    ui.fill({sb.x + 10 * s, sb.y + 10 * s, 10 * s, 10 * s}, ui.c.bg, 2 * s);
    ui.tipFor(sid, agent.active ? "Stop the agent" : "Stop");
    if (sc) { if (agent.active) agentStop(); else aiStop(); }
  } else {
    ui.circle(sb.x + sb.w / 2, sb.y + sb.h / 2, 15 * s, canSend ? (sh ? ui.c.accentHover : ui.c.accent) : ui.c.text.withA(0.12f));
    ui.icon("send", sb.x + sb.w / 2, sb.y + sb.h / 2, 15 * s, canSend ? ui.c.accentText : ui.c.textFaint, 2.2f);
    ui.tipFor(sid, "Send  (Enter)");
    if ((sc || submit) && canSend) {
      string q = trim(aiInput);
      aiInput.clear();
      if (agentMode) agentStart(q);
      else {
        Task t = aiPending;
        aiPending = Task::Chat;
        aiSend(t, q, t == Task::Chat ? q : string(ai::taskInfo(t).title) + ": " + q);
      }
    }
  }
  if (aiPending != Task::Chat && !agentMode) {
    // the task the next message runs, shown as a removable pill above the box on the right
    string pl = ai::taskInfo(aiPending).title;
    float pw = ui.textW(pl, 11.5f * s, 600) + 40 * s;
    Rect pr{cx + colW - pw, ta.y - chipsH + 4 * s, pw, 22 * s};
    ui.fill(pr, ui.c.accent.withA(ui.dark ? 0.22f : 0.12f), 11 * s);
    ui.text({pr.x + 10 * s, pr.y, pw, pr.h}, pl, 11.5f * s, ui.c.accent, AL_LEFT, 600);
    Rect xr{pr.r() - 22 * s, pr.y + 2 * s, 18 * s, 18 * s};
    uint64_t xid = ui.id("pendx");
    bool xh = false;
    if (ui.behave(xid, xr, &xh)) aiPending = Task::Chat;
    ui.icon("x", xr.x + 9 * s, xr.y + 9 * s, 10 * s, xh ? ui.c.text : ui.c.accent, 1.8f);
  }
  ui.text({cx, ta.b() + 6 * s, colW, 16 * s}, "Answers are generated by AI and can be wrong. Check important statements against the records.", 11 * s, ui.c.textFaint, AL_CENTER);
  ui.popId();
}

// ================================================================= left panel
void App::pageAssistant(Lay& L) {
  float s = ui.s;
  aiSyncTurns();
  ai::Config cfg = aiConfig();
  bool ready = cfg.ready();
  // status
  {
    Rect box = L.row(64 * s);
    ui.fill(box, ui.c.card, 8 * s);
    ui.stroke(box, ui.c.border, 8 * s);
    ui.circle(box.x + 18 * s, box.y + 22 * s, 4 * s, ready ? ui.c.ok : ui.c.textFaint);
    ui.text({box.x + 30 * s, box.y + 12 * s, box.w - 40 * s, 20 * s}, ready ? ai::providerName(cfg.provider) : "Not connected", 13 * s, ui.c.text, AL_LEFT, 600);
    ui.text({box.x + 30 * s, box.y + 33 * s, box.w - 120 * s, 18 * s}, ready ? truncate(cfg.effectiveModel(), 34) : "Add a provider and API key", 11.5f * s, ui.c.textDim);
    if (ui.button({box.r() - 88 * s, box.y + 18 * s, 76 * s, 28 * s}, ready ? "Change" : "Set Up", ready ? BTN_NORMAL : BTN_PRIMARY)) { setTab = 2; settingsWanted = true; }
  }
  // agent
  sectionTitle(L, "Agent", "Give the agent a goal, for example \"write a literature review on microplastics in soil since 2015\" or \"map the author keywords of the last five years and name the clusters\". It plans the steps and uses the app's tools: it searches OpenAlex, reads papers, builds maps, checks cluster stability, cleans terms, names clusters and writes PDF reports with charts, maps and references. Ask First: every change to the records, the map, the settings or the thesaurus waits for your approval. Automatic: the agent works through the whole task on its own. All of a run's changes can be undone together.");
  {
    Rect box = L.row(agent.active ? 58 * s : 44 * s);
    ui.fill(box, ui.c.card, 8 * s);
    ui.stroke(box, ui.c.border, 8 * s);
    ui.icon("agent", box.x + 20 * s, box.y + 22 * s, 15 * s, ready ? ui.c.accent : ui.c.textFaint, 1.7f);
    string st = !ready ? "Connect an AI provider to use the agent" : agent.active ? (agent.waitApproval ? "Waiting for your approval" : agent.waitJob ? (agent.jobTool == "search_openalex" ? "Searching OpenAlex\xE2\x80\xA6" : "Building the map\xE2\x80\xA6") : "Working\xE2\x80\xA6")
              : agentUndo.valid ? "Finished. Its changes can be undone." : "Ready";
    ui.text({box.x + 40 * s, box.y + 12 * s, box.w - 50 * s, 20 * s}, st, 12.5f * s, ready ? ui.c.text : ui.c.textFaint, AL_LEFT, 550);
    if (agent.active) ui.text({box.x + 40 * s, box.y + 32 * s, box.w - 50 * s, 16 * s}, "Step " + std::to_string(agent.steps) + " of at most " + std::to_string(ai::kAgentMaxSteps) + (agent.changes ? " \xC2\xB7 " + plural(agent.changes, "change") : string()), 11 * s, ui.c.textDim);
    {
      Rect r = L.row(28 * s);
      ui.text({r.x, r.y, 90 * s, r.h}, "Changes", 12.5f * s, ui.c.textDim);
      int mode = agentAuto() ? 1 : 0;
      if (ui.segmented({r.x + 90 * s, r.y, r.w - 90 * s, r.h}, {"Ask First", "Automatic"}, mode, "agentmode")) {
        settings.j.set("agentAuto", mode == 1);
        settings.save();
        if (agent.active && mode == 1) { agent.allowAll = true; if (agent.waitApproval) agentApprove(true, true); }
        if (agent.active && mode == 0) agent.allowAll = false;
      }
    }
    if (!agentReportPath.empty() && !agent.active) {
      Rect r = L.row(28 * s);
      auto c3 = cols(r, 2, 8 * s);
      if (ui.button(c3[0], "Open Report", BTN_NORMAL, "file")) openUrl(agentReportPath);
      if (ui.button(c3[1], "Show in Folder", BTN_NORMAL, "folder")) revealInExplorer(agentReportPath);
    }
    auto c2 = cols(L.row(28 * s), 2, 8 * s);
    if (agent.active) {
      if (ui.button(c2[0], "Show", BTN_NORMAL)) page = PG_AI;
      if (ui.button(c2[1], "Stop", BTN_NORMAL)) agentStop();
    } else {
      if (ui.button(c2[0], "New Goal\xE2\x80\xA6", BTN_PRIMARY, "", ready && !aiLive)) { agentMode = true; aiPending = Task::Chat; s_aiFocusInput = true; }
      if (ui.button(c2[1], "Undo Changes", BTN_NORMAL, "", agentUndo.valid)) agentUndoChanges();
    }
  }
  // tools
  sectionTitle(L, "Research tools", "Each tool sends a compact summary of your data (record counts, top keywords, sources and countries, the clusters and their top items, keyword trends and the most cited records) together with a task-specific instruction. Full records are never uploaded.");
  {
    Task list[] = {Task::Overview, Task::Review, Task::Gaps, Task::Emerging, Task::NameClusters, Task::Clean, Task::Explain, Task::Document, Task::Search, Task::Caption, Task::Methods};
    int n = int(sizeof list / sizeof list[0]);
    float rh = 46 * s;
    Rect box = L.row(rh * float(n));
    ui.fill(box, ui.c.card, 8 * s);
    ui.stroke(box, ui.c.border, 8 * s);
    for (int k = 0; k < n; k++) {
      const ai::TaskInfo& ti = ai::taskInfo(list[k]);
      Rect r{box.x, box.y + rh * float(k), box.w, rh};
      bool need = list[k] == Task::NameClusters || list[k] == Task::Caption ? hasMap()
                : list[k] == Task::Explain ? hasMap() && (selection.size() == 1 || clusterFilter >= 0)
                : list[k] == Task::Document ? (docPreview >= 0 || aiDoc >= 0)
                : list[k] == Task::Clean ? hasCorpus()
                : list[k] == Task::Search ? true : (hasCorpus() || hasMap());
      bool en = need && !aiLive && !agent.active;
      uint64_t idv = ui.id(string("tool:") + ti.id);
      bool hov = false;
      bool clicked = ui.behave(idv, r, &hov) && en;
      if (hov && en) ui.fill(inset(r, 1, 1), ui.c.hover, 7 * s);
      if (k) ui.line(r.x + 40 * s, r.y, r.r(), r.y, ui.c.border);
      ui.icon(ti.icon, r.x + 20 * s, r.y + rh / 2, 15 * s, en ? ui.c.accent : ui.c.textFaint, 1.7f);
      ui.text({r.x + 40 * s, r.y + 6 * s, r.w - 60 * s, 18 * s}, ti.title, 12.5f * s, en ? ui.c.text : ui.c.textFaint, AL_LEFT, 550);
      ui.text({r.x + 40 * s, r.y + 24 * s, r.w - 60 * s, 16 * s}, truncate(ti.sub, 48), 11 * s, ui.c.textDim);
      ui.icon("chev-right", r.r() - 16 * s, r.y + rh / 2, 11 * s, ui.c.textFaint, 1.6f);
      if (!need) ui.tipFor(idv, list[k] == Task::Explain ? "Select an item on the map or a cluster first" : list[k] == Task::Document ? "Open a document first" : "Needs a map");
      if (clicked) {
        if (list[k] == Task::Document && aiDoc < 0) aiDoc = docPreview;
        aiRun(list[k]);
      }
    }
  }
  // context
  sectionTitle(L, "Context", "Choose what the assistant reads with each question. The research tools add what they need automatically.");
  {
    ui.toggle(L.row(26 * s), "Data set summary", aiCtxCorpus);
    ui.toggle(L.row(26 * s), "Current map and clusters", aiCtxMap);
    ui.toggle(L.row(26 * s), "Keyword trends", aiCtxTrends);
    ui.toggle(L.row(26 * s), "Most cited records", aiCtxDocs);
    ui.toggle(L.row(26 * s), "Selection on the map", aiCtxSel);
  }
  // conversation
  sectionTitle(L, "Conversation");
  {
    int nq = 0;
    for (auto& t : aiTurns) if (t.role == "user") nq++;
    ui.text(L.row(18 * s), nq ? plural(nq, "question") + ", saved with the project" : "No questions yet", 12 * s, ui.c.textDim);
    auto c2 = cols(L.row(28 * s), 2, 8 * s);
    if (ui.button(c2[0], "New Chat", BTN_NORMAL, "", !aiTurns.empty())) aiNewChat();
    if (ui.button(c2[1], "Export\xE2\x80\xA6", BTN_NORMAL, "", !aiTurns.empty())) aiExport();
  }
}

// ================================================================= settings
void App::openSettings() {
  setKey = settings.j["openalexKey"].str();
  setEmail = settings.j["openalexEmail"].str();
  setAiProv = clampv(settings.j["aiProvider"].integer(0), 0, ai::kProviders - 1);
  for (int p = 0; p < ai::kProviders; p++) {
    setAiKey[p] = unprotectSecret(settings.j["aiKey" + std::to_string(p)].str());
    if (setAiKey[p].empty()) setAiKey[p] = settings.j["aiKeyPlain" + std::to_string(p)].str();
    setAiModel[p] = settings.j["aiModel" + std::to_string(p)].str();
  }
  setAiBase = settings.j["aiBase"].str();
  aiProbe.reset();
  paletteOpen = false;
  ui.openModal("settings");
}

void App::drawSettings() {
  if (settingsWanted) { settingsWanted = false; openSettings(); }
  if (!ui.isModalOpen("settings")) return;
  auto dialog = [this]() {
    float s = ui.s;
    Rect full{0, 0, float(g.W), float(g.H)};
    ui.fill(full, Color(0, 0, 0, ui.dark ? 0.40f : 0.16f));
    float w = std::min(560 * s, float(g.W) - 60 * s), h = std::min(520 * s, float(g.H) - 60 * s);
    Rect d{std::round((g.W - w) / 2), std::round(std::max(30 * s, (g.H - h) / 2 - 20 * s)), w, h};
    ui.shadow(d, 12 * s, 28 * s);
    ui.fill(d, ui.c.panel2, 12 * s);
    ui.stroke(d, ui.c.border, 12 * s);
    float x = d.x + 28 * s, cw = d.w - 56 * s, y = d.y + 18 * s;
    ui.text({x, y, cw, 26 * s}, "Settings", 15 * s, ui.c.text, AL_CENTER, 650);
    y += 36 * s;
    ui.segmented({d.x + d.w / 2 - 150 * s, y, 300 * s, 28 * s}, {"General", "OpenAlex", "AI Assistant"}, setTab, "set:tab");
    y += 46 * s;
    auto label = [&](const string& t) { ui.text({x, y, cw, 18 * s}, t, 12.5f * s, ui.c.text, AL_LEFT, 550); y += 22 * s; };
    auto note = [&](const string& t) { float th = ui.textWrap({x, y, cw, 80 * s}, t, 11.5f * s, ui.c.textDim); y += th + 8 * s; };
    auto link = [&](const string& l, const string& url) {
      float lw = ui.textW(l, 12 * s, 550);
      Rect lr{x, y, lw + 18 * s, 20 * s};
      uint64_t lid = ui.id("set:link:" + l);
      bool lh = false;
      if (ui.behave(lid, lr, &lh)) openUrl(url);
      ui.text(lr, l, 12 * s, lh ? ui.c.accent.withA(0.8f) : ui.c.accent, AL_LEFT, 550);
      ui.icon("external", x + lw + 10 * s, y + 10 * s, 11 * s, ui.c.accent, 1.6f);
      y += 26 * s;
    };
    if (setTab == 0) {
      label("Appearance");
      int th2 = ui.dark ? 1 : 0;
      if (ui.segmented({x, y, 200 * s, 30 * s}, {"Light", "Dark"}, th2, "set:theme")) setTheme(th2 == 1);
      y += 44 * s;
      note("The map canvas follows its own look (Look page). The VOSviewer look keeps a white canvas in both themes.");
    } else if (setTab == 1) {
      label("OpenAlex API key");
      ui.maskNext = true;
      ui.textInput({x, y, cw, 32 * s}, "set:key", setKey, "Paste your key");
      y += 38 * s;
      note("Needed for semantic search and recommended for all OpenAlex requests. Keys are free and include about 1,000 searches a day.");
      link("Get a free key", "https://openalex.org/settings/api");
      y += 6 * s;
      label("Contact email (optional)");
      ui.textInput({x, y, cw, 32 * s}, "set:mail", setEmail, "name@example.org");
      y += 40 * s;
    } else {
      label("Provider");
      int prov = setAiProv;
      ui.combo({x, y, cw, 30 * s}, "set:aiprov", {"OpenAI", "Google Gemini", "Anthropic Claude", "OpenAI-compatible (OpenRouter, Ollama, \xE2\x80\xA6)"}, prov);
      if (prov != setAiProv) { setAiProv = prov; aiProbe.reset(); }
      y += 38 * s;
      ai::Provider P_ = ai::Provider(setAiProv);
      if (P_ == ai::Provider::Compatible) {
        label("Server address");
        ui.textInput({x, y, cw, 32 * s}, "set:aibase", setAiBase, "https://openrouter.ai/api/v1  or  http://localhost:11434/v1");
        y += 40 * s;
      }
      label(P_ == ai::Provider::Compatible ? "API key (if the server needs one)" : "API key");
      ui.maskNext = true;
      ui.textInput({x, y, cw, 32 * s}, "set:aikey" + std::to_string(setAiProv), setAiKey[setAiProv], "Paste your key");
      y += 38 * s;
      note(string(ai::providerHint(P_)) + ". The key is encrypted with your Windows account and never leaves this computer except to the provider.");
      label("Model");
      auto& models = aiModelList[setAiProv];
      vector<string> opts = models.empty() ? ai::suggestedModels(P_) : models;
      string& m = setAiModel[setAiProv];
      Rect mr{x, y, cw - 136 * s, 30 * s};
      ui.textInput(mr, "set:aimodel" + std::to_string(setAiProv), m, ai::defaultModel(P_).empty() ? string("model name") : ai::defaultModel(P_) + " (default)");
      int pick = -1;
      for (size_t k = 0; k < opts.size(); k++) if (opts[k] == m) pick = int(k);
      int before = pick;
      Rect pr{mr.r() + 6 * s, y, 30 * s, 30 * s};
      // compact picker: a combo showing only the arrow
      ui.combo({pr.x, pr.y, 30 * s, 30 * s}, "set:aimodelpick" + std::to_string(setAiProv), opts, pick);
      if (pick != before && pick >= 0 && pick < int(opts.size())) m = opts[size_t(pick)];
      bool probing = aiProbe && !aiProbe->done;
      if (ui.button({pr.r() + 6 * s, y, 94 * s, 30 * s}, probing && aiProbeKind == 2 ? "Loading\xE2\x80\xA6" : "Load List", BTN_NORMAL, "", !probing)) {
        ai::Config c; c.provider = P_; c.key = trim(setAiKey[setAiProv]); c.model = trim(m); c.baseUrl = trim(setAiBase);
        auto pb = std::make_shared<AiProbe>();
        aiProbe = pb;
        aiProbeKind = 2;
        HWND hw = hwnd;
        std::thread([pb, c, hw]() {
          ai::HttpReq rq = ai::buildModelList(c);
          int st = 0; string err;
          string body = httpRequest(rq.method, rq.url, rq.headers, "", &st, &err, nullptr, 30000);
          std::lock_guard<std::mutex> lk(pb->m);
          if (st >= 200 && st < 300) { pb->models = ai::parseModelList(c, body); pb->ok = !pb->models.empty(); pb->msg = pb->ok ? std::to_string(pb->models.size()) + " models available" : "No chat models were listed."; }
          else pb->msg = ai::describeError(c, st, body, err);
          pb->done = true;
          PostMessageW(hw, WM_NULL, 0, 0);
        }).detach();
      }
      y += 40 * s;
      // test connection
      if (ui.button({x, y, 132 * s, 30 * s}, probing && aiProbeKind == 1 ? "Testing\xE2\x80\xA6" : "Test Connection", BTN_NORMAL, "", !probing)) {
        ai::Config c; c.provider = P_; c.key = trim(setAiKey[setAiProv]); c.model = trim(m); c.baseUrl = trim(setAiBase);
        c.maxTokens = 64;
        auto pb = std::make_shared<AiProbe>();
        aiProbe = pb;
        aiProbeKind = 1;
        HWND hw = hwnd;
        std::thread([pb, c, hw]() {
          ai::HttpReq rq = ai::buildChat(c, "", {{"user", "Reply with the single word: OK"}}, false);
          int st = 0; string err;
          string body = httpRequest(rq.method, rq.url, rq.headers, rq.body, &st, &err, nullptr, 60000);
          std::lock_guard<std::mutex> lk(pb->m);
          if (st >= 200 && st < 300) { string pe; ai::parseChat(c, body, &pe); pb->ok = pe.empty(); pb->msg = pb->ok ? "Connected to " + string(ai::providerName(c.provider)) + " with " + c.effectiveModel() + "." : pe; }
          else pb->msg = ai::describeError(c, st, body, err);
          pb->done = true;
          PostMessageW(hw, WM_NULL, 0, 0);
        }).detach();
      }
      if (aiProbe) {
        std::lock_guard<std::mutex> lk(aiProbe->m);
        if (aiProbe->done) {
          if (aiProbeKind == 2 && aiProbe->ok) aiModelList[setAiProv] = aiProbe->models;
          Rect mr2{x + 144 * s, y, cw - 144 * s, 30 * s};
          ui.icon(aiProbe->ok ? "check" : "warn", mr2.x + 8 * s, mr2.y + 15 * s, 14 * s, aiProbe->ok ? ui.c.ok : ui.c.danger, 2.f);
          ui.textWrap({mr2.x + 22 * s, mr2.y + 6 * s, mr2.w - 22 * s, 60 * s}, truncate(aiProbe->msg, 160), 11.5f * s, aiProbe->ok ? ui.c.textDim : ui.c.danger);
        } else ui.animating = true;
      }
      y += 40 * s;
    }
    // buttons
    float by = d.b() - 20 * s - 32 * s;
    ui.line(d.x, by - 16 * s, d.r(), by - 16 * s, ui.c.border);
    if (ui.button({d.r() - 24 * s - 96 * s, by, 96 * s, 32 * s}, "Save", BTN_PRIMARY)) {
      settings.j.set("openalexKey", trim(setKey));
      settings.j.set("openalexEmail", trim(setEmail));
      settings.j.set("aiProvider", setAiProv);
      for (int p = 0; p < ai::kProviders; p++) {
        string k = trim(setAiKey[p]);
        string enc = protectSecret(k);
        settings.j.set("aiKey" + std::to_string(p), enc);
        settings.j.set("aiKeyPlain" + std::to_string(p), enc.empty() && !k.empty() ? k : string());
        settings.j.set("aiModel" + std::to_string(p), trim(setAiModel[p]));
      }
      settings.j.set("aiBase", trim(setAiBase));
      settings.save();
      aiCfgValid_ = false;
      ui.focus = 0;
      ui.closeModal();
      ui.toast("Settings saved", "", 1, 2);
    }
    if (ui.button({d.r() - 24 * s - 96 * s - 8 * s - 96 * s, by, 96 * s, 32 * s}, "Cancel", BTN_NORMAL)) { ui.closeModal(); }
  };
  // drawn in the modal layer: the dialog's own combos open above it without closing it
  ui.beginModalLayer();
  dialog();
  ui.endModalLayer();
}

}  // namespace win
}  // namespace vs
