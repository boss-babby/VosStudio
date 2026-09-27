// VOSStudio Native: the Assistant's agent. The model plans with the application's tools (ai::agentTools), one JSON
// action per reply; reading and view tools run at once, changes wait for the user's approval and can be undone together.
#include "app.h"

#include <regex>

namespace vs {
namespace win {

using ai::Task;
string nowStamp();  // assistant.cpp

namespace {

struct NameMap { const char* name; int v; };
const NameMap kTypes[] = {{"cooccurrence", int(AnaType::Cooc)}, {"co-occurrence", int(AnaType::Cooc)}, {"coauthorship", int(AnaType::Coauth)},
                          {"co-authorship", int(AnaType::Coauth)}, {"citation", int(AnaType::Citation)}, {"coupling", int(AnaType::Coupling)},
                          {"bibliographic_coupling", int(AnaType::Coupling)}, {"cocitation", int(AnaType::Cocit)}, {"co-citation", int(AnaType::Cocit)}};
const NameMap kUnits[] = {{"keywords", int(Unit::Keywords)}, {"author_keywords", int(Unit::Keywords)}, {"all_keywords", int(Unit::AllKeywords)},
                          {"index_terms", int(Unit::IndexTerms)}, {"keywords_plus", int(Unit::IndexTerms)}, {"terms", int(Unit::Terms)},
                          {"authors", int(Unit::Authors)}, {"organisations", int(Unit::Orgs)}, {"organizations", int(Unit::Orgs)},
                          {"countries", int(Unit::Countries)}, {"documents", int(Unit::Docs)}, {"sources", int(Unit::Sources)},
                          {"references", int(Unit::Refs)}, {"cited_references", int(Unit::Refs)}, {"cited_sources", int(Unit::CSources)},
                          {"cited_authors", int(Unit::CAuthors)}};

string normName(const string& s) {
  string o;
  for (char ch : lower(trim(s))) o += (ch == ' ' || ch == '-') && o.size() && o.back() != '_' ? '_' : ch;
  return o;
}

bool parseType(const string& s, AnaType& t) {
  string k = normName(s);
  for (auto& m : kTypes) if (k == m.name || lower(trim(s)) == m.name) { t = AnaType(m.v); return true; }
  for (auto& ti : anaTypes()) if (k == lower(ti.id)) { t = ti.type; return true; }
  return false;
}

bool parseUnit(const string& s, Unit& u) {
  string k = normName(s);
  for (auto& m : kUnits) if (k == m.name) { u = Unit(m.v); return true; }
  return unitFromId(trim(s), u);
}

string typeName(AnaType t) {
  for (auto& m : kTypes) if (m.v == int(t)) return m.name;
  return "cooccurrence";
}

string unitName(Unit u) {
  for (auto& m : kUnits) if (m.v == int(u)) return m.name;
  return unitId(u);
}

bool parseView(const string& s, ViewKind& v) {
  static const NameMap views[] = {{"network", int(ViewKind::Network)}, {"overlay", int(ViewKind::Overlay)}, {"density", int(ViewKind::Density)},
                                  {"timeline", int(ViewKind::Timeline)}, {"matrix", int(ViewKind::Matrix)}, {"geo", int(ViewKind::Geo)},
                                  {"map", int(ViewKind::Geo)}, {"3d", int(ViewKind::ThreeD)}, {"three_d", int(ViewKind::ThreeD)}};
  string k = normName(s);
  for (auto& m : views) if (k == m.name) { v = ViewKind(m.v); return true; }
  return false;
}

// case-insensitive exact label, else the heaviest item containing the text
int findItem(const Network& N, const string& label) {
  string q = lower(trim(label));
  if (q.empty()) return -1;
  for (int i = 0; i < N.n(); i++) if (lower(N.nodes[size_t(i)].label) == q) return i;
  int best = -1;
  for (int i = 0; i < N.n(); i++)
    if (lower(N.nodes[size_t(i)].label).find(q) != string::npos && (best < 0 || N.weight(i) > N.weight(best))) best = i;
  return best;
}

string fmtW(double v) { return std::fabs(v - std::round(v)) < 1e-9 ? fmtInt(long(std::llround(v))) : fmtFixed(v, 2); }

string firstLine(const string& s, size_t n = 110) {
  string t = trim(s);
  size_t p = t.find('\n');
  if (p != string::npos) t = t.substr(0, p);
  while (!t.empty() && (t[0] == '#' || t[0] == '-' || t[0] == ' ')) t.erase(0, 1);
  while (!t.empty() && (t.back() == ':' || t.back() == ' ')) t.pop_back();
  return truncate(t, n);
}

}  // namespace

// ------------------------------------------------------------------ run control
void App::agentStart(const string& goalIn) {
  aiSyncTurns();
  string goal = trim(goalIn);
  if (goal.empty()) return;
  if (agent.active || aiLive) { ui.toast("Agent", "Wait for the current answer or stop it first.", 2, 3); return; }
  ai::Config cfg = aiConfig();
  if (!cfg.ready()) { setTab = 2; settingsWanted = true; ui.toast("Connect an AI provider", "The agent needs an AI provider and API key.", 0, 4); return; }
  agent = AgentRun();
  agent.active = true;
  agent.allowAll = agentAuto();
  if (agentReportExported) { agentReport = ReportDoc(); agentReportExported = false; }
  agent.goal = goal;
  aiTurns.push_back({"user", goal, "agent", nowStamp()});
  aiTurns.push_back({"assistant", "", "agent", nowStamp()});
  agent.turn = int(aiTurns.size()) - 1;
  aiLayout_.clear();
  aiStickBottom = true;
  ai::ContextOpts o;
  o.corpus = hasCorpus();
  o.map = hasMap();
  o.topDocs = false;
  o.trends = false;
  string ctx = hasCorpus() || hasMap() ? ai::buildContext(*P, o) : string("No records are loaded and there is no map yet.\n");
  if (!agentReport.empty()) ctx += "\nA report is in progress: " + plural(agentReport.sections(), "section") + ", " + plural(agentReport.figures(), "figure") + " (see get_report).\n";
  if (agentUndo.valid) ctx += "\n(Changes from an earlier agent run can still be undone by the user.)\n";
  agent.msgs.push_back({"user", ai::agentGoalMessage(goal, ctx)});
  agentRequest();
  aiTurns[size_t(agent.turn)].text = agentRender();
}

void App::agentRequest() {
  agent.req = aiRequest(ai::buildChat(aiConfig(), ai::agentSystemPrompt(), agent.msgs, true));
}

void App::agentStop() {
  if (!agent.active) return;
  if (agent.req) agent.req->cancel = true;
  agent.req.reset();
  if (agent.waitApproval && !agent.log.empty()) agent.log.back().status = 3;
  agent.waitApproval = false;
  if (agent.waitJob) {  // the build keeps running; its result is simply not reported to the model
    agent.waitJob = false;
    if (!agent.log.empty()) agent.log.back().detail = "Still running; the agent stopped waiting.";
  }
  agentFinish("*Stopped.*", true);
}

void App::agentPump() {
  if (!agent.active) return;
  aiSyncTurns();  // a project was opened or created meanwhile: the run ends
  if (!agent.active) return;
  if (agent.turn < 0 || agent.turn >= int(aiTurns.size())) { agent = AgentRun(); return; }
  if (agent.waitJob) {
    if (jobSeq == agent.jobSeq) return;
    agent.waitJob = false;
    if (agent.jobTool == "search_openalex") {
      if (!lastJobError.empty()) { agentFinishTool(agent.jobTool, false, "The search failed: " + lastJobError); return; }
      if (!hasCorpus()) { agentFinishTool(agent.jobTool, false, "OpenAlex returned no works. Try broader or other terms, or a wider period."); return; }
      string res = "Loaded " + plural(long(P->corpus.recs.size()), "work") + " from OpenAlex.\n" + ai::corpusContext(*P);
      if (hasMap()) res += "\nThe current map was built from the previous records; build a new map for these records.";
      if (!agent.log.empty()) agent.log.back().detail = plural(long(P->corpus.recs.size()), "work") + " loaded";
      agentFinishTool(agent.jobTool, true, res);
      return;
    }
    if (!lastJobError.empty()) { agentFinishTool(agent.jobTool, false, "The build failed: " + lastJobError); return; }
    const auto& R = P->last;
    string res = "Map built: " + (P->net.description.empty() ? string("network") : P->net.description) + ". " + plural(R.n, P->net.unitNoun) + ", " +
                 plural(R.m, "link") + ", " + plural(R.clusters, "cluster") + ", modularity Q " + fmtFixed(R.Q, 3) + ".";
    if (R.n == 0) res += " The map is empty: lower the threshold or pick another unit.";
    else if (R.n < 10) res += " Very few items: consider a lower threshold.";
    agentFinishTool(agent.jobTool, true, res);
    return;
  }
  if (!agent.req) return;
  string text, err;
  {
    std::lock_guard<std::mutex> lk(agent.req->m);
    if (!agent.req->done) return;
    text = agent.req->text;
    err = agent.req->err;
  }
  agent.req.reset();
  if (!err.empty()) { agentFinish("The agent stopped because the AI service returned an error:\n\n" + err, true); return; }
  agent.msgs.push_back({"assistant", text});
  ai::AgentAction a;
  string perr;
  if (!ai::parseAgentAction(text, a, &perr)) {
    if (++agent.parseFails <= 2) {
      agent.msgs.push_back({"user", "Your reply could not be read (" + perr + "). Reply with exactly one JSON object: a tool call or a final answer."});
      agentRequest();
      aiTurns[size_t(agent.turn)].text = agentRender();
      return;
    }
    agentFinish(text.empty() ? string("The model did not answer.") : text, text.empty());
    return;
  }
  agent.parseFails = 0;
  if (!a.final.empty() || a.tool.empty()) { agentFinish(a.final.empty() ? text : a.final); return; }
  if (agent.steps >= ai::kAgentMaxSteps) {
    agent.msgs.push_back({"user", "You have used all tool calls. Give your final answer now as {\"thought\": \"...\", \"final\": \"...\"}."});
    agent.steps++;
    if (agent.steps > ai::kAgentMaxSteps + 1) { agentFinish("The agent reached its step limit before finishing.", true); return; }
    agentRequest();
    return;
  }
  agent.steps++;
  const ai::ToolSpec* spec = ai::findTool(a.tool);
  if (!spec) {
    string names;
    for (auto& t : ai::agentTools()) names += string(names.empty() ? "" : ", ") + t.name;
    agent.log.push_back({"Unknown tool \"" + truncate(a.tool, 30) + "\"", a.thought, "", 2});
    agent.msgs.push_back({"user", ai::agentResultMessage(a.tool, false, "There is no tool with this name. Available tools: " + names)});
    agentRequest();
    aiTurns[size_t(agent.turn)].text = agentRender();
    return;
  }
  if (spec->kind == ai::ToolKind::Change && !agent.allowAll) {
    agent.pending = a;
    agent.waitApproval = true;
    agent.log.push_back({agentDescribe(a), a.thought, "", 4});
    aiTurns[size_t(agent.turn)].text = agentRender();
    aiStickBottom = true;
    if (page != PG_AI) ui.toast("The agent asks for approval", agentDescribe(a), 0, 6);
    return;
  }
  agent.log.push_back({agentDescribe(a), a.thought, "", 0});
  agentExecute(a);
}

void App::agentApprove(bool yes, bool all) {
  if (!agent.active || !agent.waitApproval) return;
  agent.waitApproval = false;
  if (all) agent.allowAll = true;
  if (yes) {
    if (!agent.log.empty()) agent.log.back().status = 0;
    agentExecute(agent.pending);
  } else {
    if (!agent.log.empty()) agent.log.back().status = 3;
    agentFinishTool(agent.pending.tool, false, "The user declined this change. Do not retry it; continue without it or finish.");
  }
}

void App::agentFinishTool(const string& tool, bool ok, const string& result) {
  if (!agent.active) return;
  if (!agent.log.empty()) {
    auto& st = agent.log.back();
    if (st.status != 3) st.status = ok ? 1 : 2;
    if (st.detail.empty() && st.status != 3) st.detail = firstLine(result);
  }
  agent.msgs.push_back({"user", ai::agentResultMessage(tool, ok, result)});
  agentRequest();
  if (agent.turn >= 0 && agent.turn < int(aiTurns.size())) aiTurns[size_t(agent.turn)].text = agentRender();
}

void App::agentFinish(const string& text, bool failed) {
  agent.final = trim(text);
  agent.active = false;
  agent.waitApproval = false;
  agent.waitJob = false;
  if (agent.req) agent.req->cancel = true;
  agent.req.reset();
  if (agent.turn >= 0 && agent.turn < int(aiTurns.size())) aiTurns[size_t(agent.turn)].text = agentRender();
  aiLayout_.clear();
  aiSaveTurns();
  if (agent.changes > 0) ui.toast("Agent finished", plural(agent.changes, "change") + " made. You can undo them together in the Assistant.", 1, 5);
  else if (page != PG_AI && !failed) ui.toast("Agent finished", "The answer is in the Assistant.", 1, 4);
}

// ------------------------------------------------------------------ rendering
string App::agentDescribe(const ai::AgentAction& a) const {
  const Json& g = a.args;
  const string& t = a.tool;
  if (t == "get_overview") return "Read the overview";
  if (t == "list_analyses") return "List the possible analyses";
  if (t == "list_clusters") return "Read the clusters";
  if (t == "top_items") {
    string by = g["by"].str("weight");
    int c = g["cluster"].integer(0);
    return "Read the top items by " + by + (c > 0 ? " in cluster " + std::to_string(c) : string());
  }
  if (t == "get_item") return "Look up \"" + truncate(g["label"].str(), 40) + "\"";
  if (t == "search_items") return "Search items for \"" + truncate(g["query"].str(), 40) + "\"";
  if (t == "trends") return "Read the keyword trends";
  if (t == "top_documents") return "Read the most cited documents";
  if (t == "check_stability") return "Check cluster stability";
  if (t == "compare_periods") return "Compare two periods";
  if (t == "scan_variants") return "Scan for term variants";
  if (t == "get_methods") return "Read the methods paragraph";
  if (t == "show_view") return string("Show the ") + g["view"].str("network") + " view";
  if (t == "focus_item") return "Focus \"" + truncate(g["label"].str(), 40) + "\"";
  if (t == "focus_cluster") return "Highlight cluster " + std::to_string(g["cluster"].integer(1));
  if (t == "open_page") return "Open " + g["page"].str("data");
  if (t == "build_map") {
    AnaType ty = P->spec.type;
    Unit u = P->spec.unit;
    parseType(g["type"].str(), ty);
    parseUnit(g["unit"].str(), u);
    string d = "Build a map: " + string(typeInfo(ty).label) + " of " + lower(unitLabel(ty, u));
    if (g.has("min")) d += ", minimum " + std::to_string(g["min"].integer(1));
    if (g["from_year"].integer(0) > 0 || g["to_year"].integer(0) > 0)
      d += ", " + (g["from_year"].integer(0) > 0 ? std::to_string(g["from_year"].integer(0)) : string("start")) + " to " +
           (g["to_year"].integer(0) > 0 ? std::to_string(g["to_year"].integer(0)) : string("end"));
    return d;
  }
  if (t == "recluster") {
    string d = "Re-cluster the map";
    vector<string> p;
    if (g.has("resolution")) p.push_back("resolution " + fmtNum(g["resolution"].num(1), 2));
    if (g.has("min_cluster_size")) p.push_back("minimum cluster size " + std::to_string(g["min_cluster_size"].integer(1)));
    if (g.has("method")) p.push_back(lower(g["method"].str()) == "modularity" ? "modularity" : "as VOSviewer");
    return p.empty() ? d : d + " (" + join(p, ", ") + ")";
  }
  if (t == "name_clusters") return "Name " + plural(long(g["names"].size()), "cluster") + " on the map";
  if (t == "merge_terms") {
    int nm = 0;
    for (auto& m : g["merges"].a) nm += int(m["members"].size());
    int ni = int(g["ignore"].size());
    return "Update the thesaurus: " + (nm ? plural(nm, "merge") : string()) + (nm && ni ? ", " : "") + (ni ? plural(ni, "ignored term") : string()) +
           (nm || ni ? string() : string("no entries"));
  }
  if (t == "search_openalex") {
    string d = "Search OpenAlex for \"" + truncate(g["query"].str(), 60) + "\"" + (g["semantic"].boolean(false) ? " by meaning" : "");
    int y0 = g["from_year"].integer(0), y1 = g["to_year"].integer(0);
    if (y0 > 0 || y1 > 0) d += ", " + (y0 > 0 ? std::to_string(y0) : string("start")) + " to " + (y1 > 0 ? std::to_string(y1) : string("today"));
    d += ", up to " + fmtInt(clampv(g["max"].integer(300), 50, 2000)) + " works";
    if (g["append"].boolean(false)) d += ", added to the current records";
    return d;
  }
  if (t == "read_papers") {
    string so = lower(g["sort"].str("cited"));
    string q = trim(g["query"].str());
    return string("Read the ") + (so == "recent" ? "most recent" : so == "relevance" ? "most relevant" : "most cited") + " papers" + (q.empty() ? string() : " on \"" + truncate(q, 40) + "\"");
  }
  if (t == "add_chart") {
    string ti = trim(g["title"].str());
    return "Add a figure: " + (ti.empty() ? replaceAll(normName(g["chart"].str()), "_", " ") : truncate(ti, 50));
  }
  if (t == "add_section") return "Write \"" + truncate(trim(g["title"].str()).empty() ? string("section") : trim(g["title"].str()), 50) + "\"";
  if (t == "get_report") return "Review the report";
  if (t == "export_report") return "Export the report as PDF";
  return t;
}

string App::agentRender() const {
  string md;
  for (auto& st : agent.log) {
    const char* mark = st.status == 1 ? "\xE2\x9C\x93" : st.status == 2 ? "\xE2\x9C\x97" : st.status == 3 ? "\xE2\x80\x93" : st.status == 4 ? "?" : "\xE2\x80\xA6";
    md += string("- ") + mark + " **" + st.title + "**";
    if (st.status == 3) md += " *(declined)*";
    else if (st.status == 4) md += " *(waiting for your approval)*";
    else if (st.status == 0) md += " *(running)*";
    else if (!st.detail.empty()) md += ": " + st.detail;
    md += "\n";
  }
  if (agent.active) {
    if (agent.req) md += string(md.empty() ? "" : "\n") + "*Thinking\xE2\x80\xA6*\n";
    return md.empty() ? string("*Thinking\xE2\x80\xA6*") : md;
  }
  string out = agent.final;
  if (!md.empty()) {
    out += "\n\n---\n\n**Steps** \xC2\xB7 " + plural(long(agent.log.size()), "tool call") + (agent.changes ? " \xC2\xB7 " + plural(agent.changes, "change") : string()) + "\n\n" + md;
  }
  return out;
}

// ------------------------------------------------------------------ undo
void App::agentSnapshot() {
  AgentUndo& u = agentUndo;
  u = AgentUndo();
  u.valid = true;
  u.spec = P->spec;
  u.params = P->params;
  u.th = P->engine.thesaurus;
  u.excluded = P->engine.excluded;
  u.net = P->net;
  u.bundles = P->bundles;
  u.mapSource = P->mapSource;
  u.builtSig = P->builtSig;
  u.last = P->last;
  u.clusterNames = P->style.clusterNames;
}

void App::agentUndoChanges() {
  if (!agentUndo.valid) return;
  if (busy()) { ui.toast("Undo agent changes", "Wait for the current task to finish.", 2, 3); return; }
  if (agent.active) agentStop();
  AgentUndo& u = agentUndo;
  bool thChanged = u.th.sig() != P->engine.thesaurus.sig() || u.excluded != P->engine.excluded;
  if (u.hasCorpus) {  // the agent searched OpenAlex: the records come back too
    P->corpus = std::move(u.corpus);
    P->corpusChanged();
    pageStateReset();
    thChanged = false;
  }
  P->spec = u.spec;
  P->params = u.params;
  P->engine.thesaurus = u.th;
  P->engine.excluded = u.excluded;
  P->net = std::move(u.net);
  P->bundles = std::move(u.bundles);
  P->mapSource = u.mapSource;
  P->builtSig = u.builtSig;
  P->last = u.last;
  P->style.clusterNames = u.clusterNames;
  P->metricsValid = false;
  if (thChanged) P->corpusChanged();
  P->dirty = true;
  u = AgentUndo();
  sensSig.clear();
  variantsScanned = false;
  undoStack.clear();
  redoStack.clear();
  afterMapChanged(true);
  styleDirty = true;
  aiLayout_.clear();
  ui.toast("Agent changes undone", "The records, map, settings and thesaurus are back to how they were before the agent's changes.", 1, 4);
}

// ------------------------------------------------------------------ tools
void App::agentExecute(const ai::AgentAction& a) {
  const string& t = a.tool;
  const Json& g = a.args;
  auto done = [&](bool ok, const string& r) { agentFinishTool(t, ok, r); };
  auto needMap = [&]() { if (hasMap()) return true; done(false, "There is no map yet. Build one with build_map first."); return false; };
  auto needCorpus = [&]() { if (hasCorpus()) return true; done(false, "No records are loaded (the map was imported), so this needs records."); return false; };
  auto change = [&]() {
    if (!agentUndo.valid || agentUndo.turn != agent.turn) { agentSnapshot(); agentUndo.turn = agent.turn; }
    agent.changes++;
  };
  const Network& N = P->net;

  if (t == "get_overview") {
    string s;
    if (hasCorpus()) s += ai::corpusContext(*P);
    if (hasMap()) {
      s += ai::mapContext(*P, 3);
      s += "- Clustering: " + string(P->params.clusterMethod == 0 ? "as VOSviewer (VOS quality)" : "modularity") + ", resolution " +
           fmtNum(P->params.cluster.resolution, 2) + ", minimum cluster size " + std::to_string(P->params.cluster.minSize) + "\n";
      s += "- Current view: " + string(viewName(view)) + "\n";
      if (P->mapSource != "analysis") s += "- The map was imported, not built from the records.\n";
      else if (P->builtSig != P->specSig()) s += "- The analysis settings or cleaning changed after the map was built; rebuild to apply them.\n";
    } else s += "- No map has been built yet.\n";
    s += "- Thesaurus: " + plural(long(P->engine.thesaurus.replace.size()), "entry") + "\n";
    if (!agent.log.empty())
      agent.log.back().detail = (hasCorpus() ? plural(long(P->corpus.recs.size()), "record") : string("No records")) +
                                (hasMap() ? ", map of " + plural(N.n(), N.unitNoun) + " in " + plural(N.nClusters, "cluster") : string(", no map yet"));
    done(true, s);
  } else if (t == "list_analyses") {
    if (!needCorpus()) return;
    const QualityReport& q = statQuality();
    string s = "Field coverage: keywords " + fmtNum(q.kwCov * 100, 0) + "%, index terms " + fmtNum(q.idCov * 100, 0) + "%, abstracts " + fmtNum(q.absCov * 100, 0) +
               "%, references " + fmtNum(q.refCov * 100, 0) + "%, affiliations " + fmtNum(q.affCov * 100, 0) + "%.\nAnalyses (type: units):\n";
    for (auto& ti : anaTypes()) {
      vector<string> us;
      for (auto& u : ti.units) us.push_back(unitName(u.first));
      s += "- " + typeName(ti.type) + ": " + join(us, ", ") + "\n";
    }
    s += "Current analysis: " + typeName(P->spec.type) + " of " + unitName(P->spec.unit) + ", minimum " + std::to_string(P->spec.min) + ".\n";
    if (q.refCov < 0.2) s += "References are sparse: citation, coupling and co-citation maps will be small.\n";
    done(true, s);
  } else if (t == "list_clusters") {
    if (!needMap()) return;
    if (!cinfoValid) { cinfo = clusterInfo(P->net, hasCorpus() ? &P->corpus : nullptr); cinfoValid = true; }
    int top = clampv(g["top"].integer(8), 1, 20);
    string s = plural(long(cinfo.size()), "cluster") + ":\n";
    for (auto& ci : cinfo) {
      s += "- Cluster " + std::to_string(ci.c + 1) + " \"" + N.clusterName(ci.c) + "\": " + plural(ci.size, "item");
      if (!std::isnan(ci.avgYear)) s += ", avg. year " + fmtFixed(ci.avgYear, 1);
      if (!ci.quadrant.empty()) s += ", " + lower(ci.quadrant);
      s += ". Top: ";
      for (int k = 0; k < std::min<int>(top, int(ci.items.size())); k++) s += (k ? "; " : "") + N.nodes[size_t(ci.items[size_t(k)])].label;
      s += "\n";
    }
    done(true, s);
  } else if (t == "top_items" || t == "search_items") {
    if (!needMap()) return;
    vector<int> deg(size_t(N.n()), 0);
    for (auto& l : N.links) { deg[size_t(l.a)]++; deg[size_t(l.b)]++; }
    int yi = N.scoreIndex("Avg. pub. year"), ci = N.scoreIndex("Avg. citations");
    for (size_t k = 0; k < N.scoreNames.size(); k++) {
      string nm = lower(N.scoreNames[k]);
      if (yi < 0 && nm.find("year") != string::npos) yi = int(k);
      if (ci < 0 && nm.find("citation") != string::npos) ci = int(k);
    }
    auto sc = [&](int i, int k) { const auto& v = N.nodes[size_t(i)].sc; return k >= 0 && k < int(v.size()) ? v[size_t(k)] : NAN; };
    vector<int> ids;
    int n = 20;
    if (t == "search_items") {
      string q = lower(trim(g["query"].str()));
      if (q.empty()) { done(false, "Give a query."); return; }
      for (int i = 0; i < N.n(); i++) if (lower(N.nodes[size_t(i)].label).find(q) != string::npos) ids.push_back(i);
      std::sort(ids.begin(), ids.end(), [&](int x, int y) { return N.weight(x) > N.weight(y); });
    } else {
      int c = g["cluster"].integer(0);
      if (c > N.nClusters) { done(false, "The map has " + plural(N.nClusters, "cluster") + "."); return; }
      for (int i = 0; i < N.n(); i++) if (c <= 0 || N.nodes[size_t(i)].cluster == c - 1) ids.push_back(i);
      string by = lower(g["by"].str("weight"));
      std::function<double(int)> key = [&](int i) { return N.weight(i); };
      if (by == "links") key = [&](int i) { return double(deg[size_t(i)]); };
      else if (by == "citations") { if (ci < 0) { done(false, "This map has no citation scores."); return; } key = [&](int i) { double v = sc(i, ci); return std::isnan(v) ? -1 : v; }; }
      else if (by == "year") { if (yi < 0) { done(false, "This map has no publication years."); return; } key = [&](int i) { double v = sc(i, yi); return std::isnan(v) ? -1 : v; }; }
      std::stable_sort(ids.begin(), ids.end(), [&](int x, int y) { return key(x) > key(y); });
      n = clampv(g["n"].integer(15), 1, 50);
    }
    if (ids.empty()) { done(true, "No matching items."); return; }
    string wn = N.weightIdx >= 0 && N.weightIdx < int(N.weightNames.size()) ? N.weightNames[size_t(N.weightIdx)] : string("Weight");
    string s = "label | cluster | " + lower(wn) + " | links" + (yi >= 0 ? " | avg. year" : "") + (ci >= 0 ? " | avg. citations" : "") + "\n";
    for (int k = 0; k < std::min<int>(n, int(ids.size())); k++) {
      int i = ids[size_t(k)];
      s += N.nodes[size_t(i)].label + " | " + std::to_string(N.nodes[size_t(i)].cluster + 1) + " | " + fmtW(N.weight(i)) + " | " + std::to_string(deg[size_t(i)]);
      if (yi >= 0) s += " | " + (std::isnan(sc(i, yi)) ? string("-") : fmtFixed(sc(i, yi), 1));
      if (ci >= 0) s += " | " + (std::isnan(sc(i, ci)) ? string("-") : fmtFixed(sc(i, ci), 1));
      s += "\n";
    }
    if (int(ids.size()) > n) s += "(" + std::to_string(ids.size()) + " matches in total)\n";
    done(true, s);
  } else if (t == "get_item") {
    if (!needMap()) return;
    int i = findItem(N, g["label"].str());
    if (i < 0) { done(false, "No item matches \"" + g["label"].str() + "\". Try search_items."); return; }
    const Node& nd = N.nodes[size_t(i)];
    string s = "\"" + nd.label + "\" in cluster " + std::to_string(nd.cluster + 1) + " (" + N.clusterName(nd.cluster) + ")\n";
    for (size_t k = 0; k < N.weightNames.size() && k < nd.w.size(); k++) s += "- " + N.weightNames[k] + ": " + fmtW(nd.w[k]) + "\n";
    for (size_t k = 0; k < N.scoreNames.size() && k < nd.sc.size(); k++) if (!std::isnan(nd.sc[k])) s += "- " + N.scoreNames[k] + ": " + fmtFixed(nd.sc[k], 2) + "\n";
    vector<std::pair<double, int>> nb;
    for (auto& l : N.links) {
      if (l.a == i) nb.push_back({l.w, l.b});
      else if (l.b == i) nb.push_back({l.w, l.a});
    }
    std::sort(nb.begin(), nb.end(), [](auto& x, auto& y) { return x.first > y.first; });
    s += "Strongest links (" + std::to_string(nb.size()) + " in total): ";
    for (size_t k = 0; k < std::min<size_t>(10, nb.size()); k++)
      s += (k ? "; " : "") + N.nodes[size_t(nb[k].second)].label + " (" + fmtW(nb[k].first) + (N.nodes[size_t(nb[k].second)].cluster != nd.cluster ? ", cluster " + std::to_string(N.nodes[size_t(nb[k].second)].cluster + 1) : string()) + ")";
    if (!nd.title.empty() && nd.title != nd.label) s += "\nTitle: " + nd.title;
    done(true, s);
  } else if (t == "trends") {
    if (!needCorpus()) return;
    string s = ai::trendContext(*P);
    done(!s.empty(), s.empty() ? string("Not enough years with keywords for trends.") : s);
  } else if (t == "top_documents") {
    if (!needCorpus()) return;
    done(true, ai::topDocsContext(*P, clampv(g["n"].integer(10), 1, 25)));
  } else if (t == "check_stability") {
    if (!needMap()) return;
    if (N.n() < 3) { done(false, "The map is too small."); return; }
    stab = clusterStability(P->net, P->params.clusterOpts(), clampv(g["runs"].integer(10), 3, 20));
    stabValid = true;
    string s = "Over " + std::to_string(stab.runs) + " re-runs: mean ARI " + fmtFixed(stab.meanAri, 2) + ", min ARI " + fmtFixed(stab.minAri, 2) + ", mean NMI " + fmtFixed(stab.meanNmi, 2) + ". ";
    s += stab.meanAri >= 0.9 ? "Robust clusters." : stab.meanAri >= 0.7 ? "Mostly stable; some items switch clusters." : "Unstable: consider another resolution or a higher minimum cluster size.";
    if (stab.freshReference) s += " (The map's clusters came from other settings, so runs were compared with a fresh clustering.)";
    vector<int> ord;
    for (int i = 0; i < int(stab.itemStability.size()); i++) if (stab.itemStability[size_t(i)] < 0.999) ord.push_back(i);
    std::sort(ord.begin(), ord.end(), [&](int x, int y) { return stab.itemStability[size_t(x)] < stab.itemStability[size_t(y)]; });
    if (!ord.empty()) {
      s += "\nLeast stable items: ";
      for (size_t k = 0; k < std::min<size_t>(8, ord.size()); k++) s += (k ? "; " : "") + N.nodes[size_t(ord[k])].label + " (" + fmtNum(stab.itemStability[size_t(ord[k])] * 100, 0) + "%)";
    }
    done(true, s);
  } else if (t == "compare_periods") {
    if (!needMap()) return;
    int a0 = g["a_from"].integer(0), a1 = g["a_to"].integer(0), b0 = g["b_from"].integer(0), b1 = g["b_to"].integer(0);
    if (a0 && a1 && b0 && b1) { cmpA0 = a0; cmpA1 = a1; cmpB0 = b0; cmpB1 = b1; }
    else suggestPeriods(P->corpus, cmpA0, cmpA1, cmpB0, cmpB1);
    diffCompute();
    if (!pdiff.ok) { done(false, pdiff.error + " Build a map from the records first."); return; }
    auto per = [](int x, int y) { return x == y ? std::to_string(x) : std::to_string(x) + " to " + std::to_string(y); };
    string s = "Period A " + per(pdiff.a0, pdiff.a1) + " (" + plural(pdiff.nA, "document") + "), period B " + per(pdiff.b0, pdiff.b1) + " (" + plural(pdiff.nB, "document") + "). ";
    s += std::to_string(pdiff.counts[0]) + " appearing, " + std::to_string(pdiff.counts[1]) + " growing, " + std::to_string(pdiff.counts[2]) + " stable, " + std::to_string(pdiff.counts[3]) + " fading items.";
    for (int cls : {0, 1, 3}) {
      vector<int> idx;
      for (auto& it : pdiff.items) if (int(it.status) == cls) idx.push_back(it.node);
      std::sort(idx.begin(), idx.end(), [&](int x, int y) {
        const ItemChange &p = pdiff.items[size_t(x)], &q = pdiff.items[size_t(y)];
        return cls == 0 ? p.b > q.b : cls == 3 ? p.logRatio < q.logRatio : p.logRatio > q.logRatio;
      });
      if (idx.empty()) continue;
      s += string("\n") + changeLabel(Change(cls)) + ": ";
      for (size_t k = 0; k < std::min<size_t>(10, idx.size()); k++) {
        const ItemChange& c = pdiff.items[size_t(idx[k])];
        s += (k ? "; " : "") + N.nodes[size_t(idx[k])].label + " (" + std::to_string(c.a) + " to " + std::to_string(c.b) + " documents)";
      }
    }
    done(true, s);
  } else if (t == "scan_variants") {
    if (!needCorpus()) return;
    Unit u = Unit::Keywords;
    string un = g["unit"].str("keywords");
    if (!parseUnit(un, u)) { done(false, "Unknown unit \"" + un + "\"."); return; }
    bool okUnit = false;
    for (auto& c : cleanUnits()) okUnit |= c.first == u;
    if (!okUnit) { done(false, "Variants can be scanned for keywords, all_keywords, index_terms, authors, sources and organisations."); return; }
    auto vs = findVariants(P->corpus, u, P->engine.thesaurus);
    if (vs.empty()) { done(true, "No rule-based variants found."); return; }
    string s = plural(long(vs.size()), "variant group") + " found:\n";
    for (size_t k = 0; k < std::min<size_t>(30, vs.size()); k++) {
      s += "- " + vs[k].target + " (" + std::to_string(vs[k].targetCount) + ") \xE2\x86\x90 ";
      for (size_t m = 0; m < vs[k].members.size(); m++) s += (m ? ", " : "") + vs[k].members[m] + " (" + std::to_string(vs[k].counts.size() > m ? vs[k].counts[m] : 0) + ")";
      s += " [" + vs[k].reason + "]\n";
    }
    done(true, s);
  } else if (t == "get_methods") {
    if (!needMap()) return;
    done(true, P->methods());
  } else if (t == "show_view") {
    if (!needMap()) return;
    ViewKind v;
    if (!parseView(g["view"].str(), v)) { done(false, "Unknown view. Use network, overlay, density, timeline, matrix, geo or 3d."); return; }
    if (v == ViewKind::Geo && !geoAvailable()) { done(false, "The Geo view needs a map of countries or organisations with locations."); return; }
    setView(v, true);
    done(true, string("The user now sees the ") + viewName(v) + " view.");
  } else if (t == "focus_item") {
    if (!needMap()) return;
    int i = findItem(N, g["label"].str());
    if (i < 0) { done(false, "No item matches \"" + g["label"].str() + "\"."); return; }
    focusOn(i);
    done(true, "Selected and zoomed to \"" + N.nodes[size_t(i)].label + "\".");
  } else if (t == "focus_cluster") {
    if (!needMap()) return;
    int c = g["cluster"].integer(0);
    if (c < 1 || c > N.nClusters) { done(false, "Cluster numbers run from 1 to " + std::to_string(N.nClusters) + "."); return; }
    focusCluster(c - 1);
    done(true, "Cluster " + std::to_string(c) + " (" + N.clusterName(c - 1) + ") is highlighted.");
  } else if (t == "open_page") {
    static const NameMap pages[] = {{"data", PG_DATA}, {"build", PG_BUILD}, {"look", PG_LOOK}, {"analyse", PG_ANALYSE}, {"analyze", PG_ANALYSE},
                                    {"trends", PG_TRENDS}, {"actors", PG_ACTORS}, {"publish", PG_PUBLISH}};
    string k = normName(g["page"].str());
    for (auto& p : pages) if (k == p.name) { page = Page(p.v); done(true, "Opened " + k + "."); return; }
    done(false, "Unknown page.");
  } else if (t == "build_map") {
    if (!needCorpus()) return;
    if (busy()) { done(false, "Another task is running. Try again later."); return; }
    AnaSpec sp = P->spec;
    if (g.has("type") && !parseType(g["type"].str(), sp.type)) { done(false, "Unknown type \"" + g["type"].str() + "\"."); return; }
    if (g.has("unit") && !parseUnit(g["unit"].str(), sp.unit)) { done(false, "Unknown unit \"" + g["unit"].str() + "\"."); return; }
    const TypeInfo& ti = typeInfo(sp.type);
    bool okUnit = false;
    for (auto& u : ti.units) okUnit |= u.first == sp.unit;
    if (!okUnit) {
      vector<string> us;
      for (auto& u : ti.units) us.push_back(unitName(u.first));
      done(false, typeName(sp.type) + " maps support these units: " + join(us, ", ") + ".");
      return;
    }
    bool newKind = sp.type != P->spec.type || sp.unit != P->spec.unit;
    if (newKind) sp.setDefaults();
    if (g.has("counting")) sp.fractional = lower(g["counting"].str()) == "fractional";
    if (g.has("min")) sp.min = clampv(g["min"].integer(sp.min), 1, 100000);
    if (g.has("max_items")) sp.maxItems = clampv(g["max_items"].integer(sp.maxItems), 2, 5000);
    if (g.has("from_year")) sp.y0 = std::max(0, g["from_year"].integer(0));
    if (g.has("to_year")) sp.y1 = std::max(0, g["to_year"].integer(0));
    change();
    P->spec = sp;
    sensSig.clear();
    agent.jobTool = t;
    agent.jobSeq = jobSeq;
    cmdBuild();
    if (!busy()) {  // the job did not start
      done(false, lastJobError.empty() ? string("The build could not start.") : lastJobError);
      return;
    }
    agent.waitJob = true;
    if (agent.turn >= 0 && agent.turn < int(aiTurns.size())) aiTurns[size_t(agent.turn)].text = agentRender();
  } else if (t == "recluster") {
    if (!needMap()) return;
    if (busy()) { done(false, "Another task is running. Try again later."); return; }
    change();
    if (g.has("resolution")) P->params.cluster.resolution = std::max(0.01, std::min(20.0, g["resolution"].num(1)));
    if (g.has("min_cluster_size")) P->params.cluster.minSize = clampv(g["min_cluster_size"].integer(1), 1, 1000);
    if (g.has("method")) P->params.clusterMethod = lower(g["method"].str()) == "modularity" ? 1 : 0;
    cmdRecluster();
    stabValid = false;
    done(true, "Re-clustered (resolution " + fmtNum(P->params.cluster.resolution, 2) + ", minimum size " + std::to_string(P->params.cluster.minSize) + "): " +
                   plural(P->net.nClusters, "cluster") + ", modularity Q " + fmtFixed(P->last.Q, 3) + ". Cluster names were reset.");
  } else if (t == "name_clusters") {
    if (!needMap()) return;
    const Json& names = g["names"];
    if (names.size() == 0) { done(false, "Give names as [{\"cluster\": 1, \"name\": \"...\"}]."); return; }
    change();
    pushUndo("Name clusters");
    if (int(P->net.clusterNames.size()) < P->net.nClusters) P->net.clusterNames.resize(size_t(P->net.nClusters));
    int n = 0, bad = 0;
    for (auto& e : names.a) {
      int c = e["cluster"].integer(0);
      string nm = trim(e["name"].str());
      if (c < 1 || c > P->net.nClusters || nm.empty()) { bad++; continue; }
      P->net.clusterNames[size_t(c - 1)] = truncate(nm, 48);
      n++;
    }
    P->style.clusterNames = true;
    styleDirty = true;
    P->dirty = true;
    figSig.clear();
    cinfoValid = false;
    done(n > 0, plural(n, "cluster") + " named" + (bad ? "; " + std::to_string(bad) + " entries skipped (no such cluster)" : string()) + ".");
  } else if (t == "merge_terms") {
    if (!needCorpus()) return;
    Unit u = Unit::Keywords;
    if (g.has("unit") && !parseUnit(g["unit"].str(), u)) { done(false, "Unknown unit."); return; }
    bool okUnit = false;
    for (auto& c : cleanUnits()) okUnit |= c.first == u;
    if (!okUnit) { done(false, "The thesaurus can be edited for keywords, all_keywords, index_terms, authors, sources and organisations."); return; }
    std::unordered_map<string, string> have;  // key -> label as in the data
    for (auto& tc : termCounts(P->corpus, u, P->engine.thesaurus)) have[thesaurusKey(u, tc.first)] = tc.first;
    std::map<string, string> add;
    int merged = 0, ignored = 0, missing = 0;
    vector<string> miss;
    for (auto& m : g["merges"].a) {
      string target = bibClean(m["target"].str());
      if (target.empty()) continue;
      auto tt = have.find(thesaurusKey(u, target));
      if (tt != have.end()) target = tt->second;
      for (auto& mem : m["members"].a) {
        string k = thesaurusKey(u, mem.str());
        if (!have.count(k)) { missing++; if (miss.size() < 6) miss.push_back(mem.str()); continue; }
        if (k == thesaurusKey(u, target)) continue;
        add[k] = target;
        merged++;
      }
    }
    for (auto& ig : g["ignore"].a) {
      string k = thesaurusKey(u, ig.str());
      if (!have.count(k)) { missing++; if (miss.size() < 6) miss.push_back(ig.str()); continue; }
      add[k] = "";
      ignored++;
    }
    if (add.empty()) { done(false, "None of these terms occur in the data" + (miss.empty() ? string() : " (for example: " + join(miss, "; ") + ")") + ". Use scan_variants or search_items for exact spellings."); return; }
    change();
    ThUndo undo{"agent's thesaurus changes", P->engine.thesaurus.replace};
    for (auto& kv : add) P->engine.thesaurus.replace[kv.first] = kv.second;
    thUndo.push_back(std::move(undo));
    if (thUndo.size() > 20) thUndo.erase(thUndo.begin());
    P->corpusChanged();
    P->dirty = true;
    sensSig.clear();
    variantsScanned = false;
    string s = "Thesaurus updated: " + plural(merged, "merge") + ", " + plural(ignored, "ignored term") + ".";
    if (missing) s += " " + std::to_string(missing) + " terms not found in the data were skipped" + (miss.empty() ? string() : " (" + join(miss, "; ") + ")") + ".";
    s += hasMap() && P->mapSource == "analysis" ? " Rebuild the map (build_map with no arguments keeps the settings) to apply it." : "";
    done(true, s);
  } else if (t == "search_openalex") {
    if (busy()) { done(false, "Another task is running. Try again later."); return; }
    string q = trim(g["query"].str());
    if (q.empty()) { done(false, "Give a query."); return; }
    bool sem = g["semantic"].boolean(false);
    change();
    if (!agentUndo.hasCorpus) { agentUndo.hasCorpus = true; agentUndo.corpus = P->corpus; }
    oaKind = sem ? 1 : 0;
    if (sem) oaSemQueries = {q};
    else oaQuery = q;
    int y0 = g["from_year"].integer(0), y1 = g["to_year"].integer(0);
    oaFrom = y0 > 0 ? std::to_string(y0) : string();
    oaTo = y1 > 0 ? std::to_string(y1) : string();
    oaMax = std::to_string(clampv(g["max"].integer(300), 50, 2000));
    oaAppend = g["append"].boolean(false);
    oaMode = 0;
    oaField = 1;  // titles and abstracts: fewer off-topic works than full-text matches
    agent.jobTool = t;
    agent.jobSeq = jobSeq;
    cmdOpenAlex();
    if (!busy()) { done(false, lastJobError.empty() ? string("The search could not start.") : lastJobError); return; }
    agent.waitJob = true;
    if (agent.turn >= 0 && agent.turn < int(aiTurns.size())) aiTurns[size_t(agent.turn)].text = agentRender();
  } else if (t == "read_papers") {
    if (!needCorpus()) return;
    string s = agentReadPapers(g);
    done(!s.empty(), s.empty() ? string("No paper matches this query. Try fewer or other words.") : s);
  } else if (t == "add_chart") {
    ReportBlock b;
    string err = agentChart(g["chart"].str(), g, b);
    if (!err.empty()) { done(false, err); return; }
    agentReport.blocks.push_back(std::move(b));
    int nf = agentReport.figures();
    const ReportBlock& bb = agentReport.blocks.back();
    string s = "Figure " + std::to_string(nf) + " added: " + bb.title + ".";
    if (!bb.text.empty()) s += " Caption: " + bb.text;
    s += "\n" + bb.figId;  // the data summary (set by agentChart)
    agentReport.blocks.back().figId = normName(g["chart"].str());
    if (!agent.log.empty()) agent.log.back().detail = "Figure " + std::to_string(nf);
    done(true, s);
  } else if (t == "add_section") {
    string title = trim(g["title"].str()), text = g["text"].t == Json::Str ? g["text"].str() : string();
    if (trim(text).empty()) { done(false, "Give the section text."); return; }
    ReportBlock b;
    b.kind = ReportBlock::Section;
    b.title = truncate(title, 140);
    b.text = replaceAll(text, "\r", "");
    int words = 0, cites = 0, bad = 0;
    for (auto& w : splitAny(b.text, " \n\t")) words += !w.empty();
    std::regex rx(R"(R(\d+))");
    std::regex grp(R"(\[R\d+(?:\s*[,;]\s*R\d+)*\])");
    for (std::sregex_iterator it(b.text.begin(), b.text.end(), grp), end; it != end; ++it) {
      string gtxt = it->str();
      for (std::sregex_iterator k(gtxt.begin(), gtxt.end(), rx), e2; k != e2; ++k) {
        int r = std::atoi((*k)[1].str().c_str());
        cites++;
        if (r < 1 || r > int(P->corpus.recs.size())) bad++;
      }
    }
    agentReport.blocks.push_back(std::move(b));
    string s = "Section " + std::to_string(agentReport.sections()) + " added (" + plural(words, "word") + ", " + plural(cites, "citation") + "). The report now has " +
               plural(agentReport.sections(), "section") + " and " + plural(agentReport.figures(), "figure") + ".";
    if (bad) s += " " + plural(bad, "citation") + " did not match a paper id and will be left out.";
    if (!agent.log.empty()) agent.log.back().detail = plural(words, "word") + (cites ? ", " + plural(cites, "citation") : string());
    done(true, s);
  } else if (t == "get_report") {
    if (agentReport.empty()) { done(true, "The report is empty."); return; }
    string s;
    int fi = 0, si = 0;
    for (auto& b : agentReport.blocks) {
      if (b.kind == ReportBlock::Figure) s += "- Figure " + std::to_string(++fi) + ": " + b.title + "\n";
      else {
        int words = 0;
        for (auto& w : splitAny(b.text, " \n\t")) words += !w.empty();
        s += "- Section " + std::to_string(++si) + ": " + (b.title.empty() ? string("(untitled)") : b.title) + " (" + plural(words, "word") + ")\n";
      }
    }
    done(true, s);
  } else if (t == "export_report") {
    if (agentReport.empty()) { done(false, "The report is empty. Add sections and figures first."); return; }
    string msg;
    bool ok = agentExportReport(g, msg);
    done(ok, msg);
  } else {
    done(false, "This tool is not available.");
  }
}

// ------------------------------------------------------------------ report tools
namespace {
struct UnitName { Unit u; const char* label; };
const UnitName kCorpusUnits[] = {{Unit::AllKeywords, "Keywords"}, {Unit::Keywords, "Author keywords"}, {Unit::IndexTerms, "Index terms"},
                                 {Unit::Terms, "Title and abstract terms"}, {Unit::Authors, "Authors"}, {Unit::Orgs, "Organisations"},
                                 {Unit::Countries, "Countries"}, {Unit::Sources, "Sources"}};
const char* corpusUnitName(Unit u) {
  for (auto& x : kCorpusUnits) if (x.u == u) return x.label;
  return nullptr;
}
string authorsShort(const Record& r) {
  if (r.authors.empty()) return "Anonymous";
  string s;
  for (size_t k = 0; k < std::min<size_t>(3, r.authors.size()); k++) s += (k ? "; " : "") + r.authors[k];
  if (r.authors.size() > 3) s += " et al.";
  return s;
}
}  // namespace

string App::agentReadPapers(const Json& g) {
  const auto& R = P->corpus.recs;
  string so = lower(g["sort"].str("cited"));
  int n = clampv(g["n"].integer(12), 1, 30), skip = std::max(0, g["skip"].integer(0));
  vector<string> words;
  for (auto& w : splitAny(lower(g["query"].str()), " ,;")) if (w.size() > 1) words.push_back(w);
  vector<int> ids;
  for (int i = 0; i < int(R.size()); i++) {
    if (!words.empty()) {
      const Record& r = R[size_t(i)];
      string hay = lower(r.title + " " + r.abstract_ + " " + join(r.keywords, " ") + " " + join(r.indexTerms, " "));
      bool all = true;
      for (auto& w : words) if (hay.find(w) == string::npos) { all = false; break; }
      if (!all) continue;
    }
    ids.push_back(i);
  }
  if (ids.empty()) return "";
  if (so == "recent") std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return R[size_t(a)].year != R[size_t(b)].year ? R[size_t(a)].year > R[size_t(b)].year : R[size_t(a)].cites > R[size_t(b)].cites; });
  else if (so != "relevance") std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) { return R[size_t(a)].cites > R[size_t(b)].cites; });
  if (skip >= int(ids.size())) return plural(long(ids.size()), "paper") + " match; skip is past the end.";
  int end = std::min(int(ids.size()), skip + n);
  size_t absLen = n > 16 ? 300 : 480;
  string s = plural(long(ids.size()), "paper") + " match" + (ids.size() == 1 ? "es" : "") + ". Papers " + std::to_string(skip + 1) + " to " + std::to_string(end) + ", sorted by " +
             (so == "recent" ? "year" : so == "relevance" ? "search relevance" : "citations") + ":\n";
  for (int k = skip; k < end; k++) {
    const Record& r = R[size_t(ids[size_t(k)])];
    s += "\n[R" + std::to_string(ids[size_t(k)] + 1) + "] " + authorsShort(r) + " (" + (r.year ? std::to_string(r.year) : string("n.d.")) + "). " + trim(r.title) + ". " +
         (r.source.empty() ? string() : sourceCase(r.source) + ". ") + "Cited " + std::to_string(r.cites) + " times.\n";
    vector<string> kw(r.keywords.begin(), r.keywords.begin() + long(std::min<size_t>(6, r.keywords.size())));
    if (kw.empty()) kw.assign(r.indexTerms.begin(), r.indexTerms.begin() + long(std::min<size_t>(6, r.indexTerms.size())));
    if (!kw.empty()) s += "Keywords: " + join(kw, "; ") + "\n";
    string ab = trim(r.abstract_);
    s += ab.empty() ? string("(no abstract)\n") : "Abstract: " + truncate(ab, absLen) + "\n";
  }
  if (end < int(ids.size())) s += "\n(" + std::to_string(int(ids.size()) - end) + " more; use skip " + std::to_string(end) + ")";
  if (!agent.log.empty()) agent.log.back().detail = plural(end - skip, "paper");
  return s;
}

// A report figure: the chart or map, a default title and caption, and (in out.figId) a short data summary for the model.
string App::agentChart(const string& id0, const Json& g, ReportBlock& out) {
  string id = normName(id0);
  if (id.empty()) return "Name a chart.";
  out = ReportBlock();
  out.kind = ReportBlock::Figure;
  ChartTheme th = chartTheme(true);
  th.transparent = false;
  const double W = 500;
  const Corpus& C = P->corpus;
  Unit unit = id == "production_over_time" ? Unit::Authors : Unit::AllKeywords;
  string un = trim(g["unit"].str());
  if (!un.empty() && !parseUnit(un, unit)) return "Unknown unit \"" + un + "\".";
  const char* unitL = corpusUnitName(unit);
  string summary;
  auto needC = [&]() { return hasCorpus(); };
  auto topList = [&](const vector<std::pair<string, double>>& rows, size_t k) {
    string s;
    for (size_t i = 0; i < std::min(k, rows.size()); i++) s += (i ? "; " : "") + rows[i].first + " (" + fmtW(rows[i].second) + ")";
    return s;
  };
  auto overlayFigure = [&](const Network& net, const ViewStyle& st) {
    FigureSpec sp = P->fig;
    sp.panelNetwork = sp.panelDensity = sp.panelTimeline = sp.panelGeo = sp.panel3D = sp.panelMatrix = false;
    sp.panelOverlay = true;
    sp.wmm = 170; sp.hmm = 125; sp.theme = FigTheme::Print; sp.transparent = false; sp.footer = false; sp.pdfPages = false; sp.letters = false;
    sp.title.clear(); sp.caption.clear();
    FigureSpec saved = P->fig;
    P->fig = sp;
    auto defs = figurePanelDefs();
    P->fig = saved;
    return buildFigure(net, st, sp, &P->bundles, P->methodsShort(), &defs);
  };
  if (startsWith(id, "map_") || id == "strategic_diagram" || id == "difference_map" || id == "resolution_sweep") {
    if (!hasMap()) return "There is no map yet. Build one with build_map first.";
    const Network& N = P->net;
    if (id == "strategic_diagram") {
      if (!cinfoValid) { cinfo = clusterInfo(P->net, hasCorpus() ? &P->corpus : nullptr); cinfoValid = true; }
      if (cinfo.size() < 2) return "The strategic diagram needs at least two clusters.";
      out.fig = chartStrategic(cinfo, clusterColors(), W, 380, th);
      out.title = "Strategic diagram of the clusters";
      out.text = "Clusters by centrality (links to other clusters) and density (internal links); bubble size shows the number of items";
      for (auto& ci : cinfo) summary += "Cluster " + std::to_string(ci.c + 1) + " \"" + N.clusterName(ci.c) + "\": " + (ci.quadrant.empty() ? string("-") : lower(ci.quadrant)) + "\n";
    } else if (id == "difference_map") {
      if (!cmpA0) suggestPeriods(P->corpus, cmpA0, cmpA1, cmpB0, cmpB1);
      diffCompute();
      if (!pdiff.ok) return pdiff.error;
      Network tmp = P->net;
      ViewStyle st = P->style;
      double m = diffScoreInto(tmp);
      st.scheme = "coolwarm"; st.scoreMin = -m; st.scoreMax = m;
      out.fig = overlayFigure(tmp, st);
      auto per = [](int x, int y) { return x == y ? std::to_string(x) : std::to_string(x) + "-" + std::to_string(y); };
      out.title = "Difference map: " + per(pdiff.a0, pdiff.a1) + " compared with " + per(pdiff.b0, pdiff.b1);
      out.text = "Colours show how the share of documents of each item changed between the two periods: red items grew, blue items faded, grey items stayed about the same";
      summary = std::to_string(pdiff.counts[0]) + " appearing, " + std::to_string(pdiff.counts[1]) + " growing, " + std::to_string(pdiff.counts[2]) + " stable, " + std::to_string(pdiff.counts[3]) + " fading items.";
    } else if (id == "resolution_sweep") {
      if (N.n() < 3) return "The map is too small for a resolution sweep.";
      sweep = resolutionSweep(P->net, P->params.clusterOpts(), defaultSweepResolutions(), sweepSeeds);
      sweepValid = true;
      sweepSig = std::to_string(P->net.n()) + ":" + std::to_string(P->net.m()) + ":" + clusterTag(P->params.clusterOpts());
      out.fig = chartSweep(sweep, P->params.cluster.resolution, W, 250, th);
      out.title = "Clustering resolution sweep";
      out.text = "Number of clusters (bars) and agreement between runs with different seeds (line, adjusted Rand index) for each resolution";
      for (auto& p : sweep.pts) summary += (summary.empty() ? "" : "; ") + string("resolution ") + fmtNum(p.resolution, 2) + ": " + std::to_string(p.clusters) + " clusters, ARI " + fmtFixed(p.meanAri, 2);
      if (sweep.best >= 0) summary += ". Most reproducible: resolution " + fmtNum(sweep.pts[size_t(sweep.best)].resolution, 2) + ". Current: " + fmtNum(P->params.cluster.resolution, 2) + ".";
    } else {
      FigureSpec sp = P->fig;
      sp.panelNetwork = sp.panelOverlay = sp.panelDensity = sp.panelTimeline = sp.panelGeo = sp.panel3D = sp.panelMatrix = false;
      string what, cap;
      if (id == "map_network") { sp.panelNetwork = true; what = "Network map"; cap = "Colours show clusters; circle size shows the weight of an item; lines show the strongest links"; }
      else if (id == "map_overlay") { sp.panelOverlay = true; what = "Overlay map"; cap = "Colours show the average publication year of the documents of each item"; }
      else if (id == "map_density") { sp.panelDensity = true; what = "Density map"; cap = "Brighter areas contain more, and heavier, items"; }
      else if (id == "map_timeline") { sp.panelTimeline = true; what = "Timeline map"; cap = "Items placed by average publication year"; }
      else if (id == "map_geo") {
        if (!geoAvailable()) return "The map_geo figure needs country information in the records or a map of countries.";
        sp.panelGeo = true; what = "Geographic map"; cap = "Countries of the authors";
      } else return "Unknown map figure. Use map_network, map_overlay, map_density, map_timeline or map_geo.";
      sp.wmm = 170;
      sp.hmm = sp.panelGeo ? 100 : 125;
      sp.theme = FigTheme::Print;
      sp.transparent = false;
      sp.footer = false;
      sp.pdfPages = false;
      sp.letters = false;
      sp.title.clear();
      sp.caption.clear();
      FigureSpec saved = P->fig;
      P->fig = sp;
      auto defs = figurePanelDefs();
      P->fig = saved;
      out.fig = buildFigure(P->net, P->style, sp, &P->bundles, P->methodsShort(), &defs);
      string desc = N.description.empty() ? string("the map") : N.description;
      out.title = what + ": " + desc;
      out.text = plural(N.n(), N.unitNoun) + " in " + plural(N.nClusters, "cluster") + ". " + cap;
      summary = "The map has " + plural(N.n(), N.unitNoun) + ", " + plural(N.m(), "link") + " and " + plural(N.nClusters, "cluster") + ".";
    }
  } else {
    if (!needC()) return "No records are loaded. Search OpenAlex first (search_openalex) or import records.";
    if (!unitL && (id == "trend_topics" || id == "keyword_bursts" || id == "thematic_evolution" || id == "production_over_time"))
      return "This chart works with keywords, author_keywords, index_terms, terms, authors, organisations, countries or sources.";
    string uL = unitL ? lower(unitL) : string("keywords");
    const Growth& gr = statGrowth();
    if (id == "publications_per_year" || id == "annual_production" || id == "growth") {
      if (gr.perYear.empty()) return "The records have no publication years.";
      out.fig = chartGrowth(gr, W, 250, th);
      out.title = "Annual scientific production";
      out.text = "Number of documents per publication year";
      int tot = 0, peak = 0, peakN = 0;
      for (auto& p : gr.perYear) { tot += p.second; if (p.second > peakN) { peakN = p.second; peak = p.first; } }
      summary = std::to_string(tot) + " documents with a year, " + std::to_string(gr.y0) + " to " + std::to_string(gr.y1) + "; peak " + std::to_string(peak) + " (" +
                std::to_string(peakN) + " documents); annual growth rate " + fmtNum(gr.cagr * 100, 1) + "%.";
      summary += " Per year: ";
      for (size_t k = 0; k < gr.perYear.size(); k++) summary += (k ? ", " : "") + std::to_string(gr.perYear[k].first) + ": " + std::to_string(gr.perYear[k].second);
    } else if (id == "top_sources" || id == "top_authors" || id == "top_countries" || id == "top_organisations" || id == "top_organizations") {
      Unit u = id == "top_sources" ? Unit::Sources : id == "top_authors" ? Unit::Authors : id == "top_countries" ? Unit::Countries : Unit::Orgs;
      string nm = id == "top_sources" ? "sources" : id == "top_authors" ? "authors" : id == "top_countries" ? "countries" : "organisations";
      auto a = topActors(C, u, 12);
      vector<std::pair<string, double>> rows;
      for (size_t k = 0; k < std::min<size_t>(12, a.size()); k++) rows.push_back({truncate(u == Unit::Sources ? sourceCase(a[k].label) : a[k].label, 58), double(a[k].docs)});
      if (rows.empty()) return "The records have no " + nm + ".";
      out.fig = chartBars(rows, "", W, double(rows.size()) * 22 + 34, th, "");
      out.title = "Most productive " + nm;
      out.text = "Number of documents";
      summary = "Top " + nm + " (documents): " + topList(rows, 12);
    } else if (id == "most_cited" || id == "most_cited_papers") {
      vector<int> ord;
      for (int i = 0; i < int(C.recs.size()); i++) if (C.recs[size_t(i)].cites > 0) ord.push_back(i);
      if (ord.empty()) return "The records have no citation counts.";
      std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return C.recs[size_t(a)].cites > C.recs[size_t(b)].cites; });
      vector<DocBar> rows;
      summary = "Most cited: ";
      for (size_t k = 0; k < std::min<size_t>(12, ord.size()); k++) {
        const Record& r = C.recs[size_t(ord[k])];
        rows.push_back({ord[k], shortCite(r), double(r.cites), ""});
        summary += (k ? "; " : "") + string("[R") + std::to_string(ord[k] + 1) + "] " + shortCite(r) + " (" + std::to_string(r.cites) + ")";
      }
      out.fig = chartDocBars(rows, W, double(rows.size()) * 22 + 34, th, "");
      out.title = "Most cited documents";
      out.text = "Citations reported by the database";
    } else if (id == "citation_classes") {
      const CitationSummary& cs = statCiteSummary();
      if (cs.bins.empty()) return "The records have no citation counts.";
      vector<std::pair<string, double>> b;
      for (auto& x : cs.bins) b.push_back({x.first, double(x.second)});
      out.fig = chartBars(b, "", W, double(b.size()) * 22 + 30, th, "");
      out.title = "Distribution of citations";
      out.text = "Number of documents per citation class";
      summary = "h-index " + std::to_string(cs.h) + ", mean citations " + fmtNum(cs.mean, 1) + ", median " + fmtNum(cs.median, 1) + ", uncited " + fmtNum(100.0 * cs.uncited / std::max(1, cs.docs), 0) + "%. Classes: " + topList(b, 12);
    } else if (id == "trend_topics") {
      auto v = trendTopics(C, unit, 3, 3, &P->engine.thesaurus);
      if (v.empty()) return "Too few repeated " + uL + " per year for trend topics.";
      out.fig = chartTrendTopics(v, W, std::min(640.0, std::max(160.0, double(v.size()) * 17 + 44)), th);
      out.title = "Trend topics (" + uL + ")";
      out.text = "For each term, the line spans the first to third quartile of its publication years and the dot marks the median year; dot size shows frequency";
      summary = "Terms by median year: ";
      for (size_t k = 0; k < std::min<size_t>(24, v.size()); k++) summary += (k ? "; " : "") + v[k].label + " (" + fmtNum(v[k].med, 0) + ", " + std::to_string(v[k].freq) + ")";
    } else if (id == "keyword_bursts" || id == "bursts") {
      auto b = detectBursts(C, unit, 3, 2.0, 1.0);
      if (b.empty()) return "No bursts were found for " + uL + ".";
      int n = std::min<int>(15, int(b.size()));
      out.fig = chartBursts(b, gr.y0, gr.y1, 15, W, std::max(140.0, 26.0 * n + 30), th);
      out.title = "Strongest bursts (" + uL + ")";
      out.text = "Periods in which a term was used much more often than usual (Kleinberg's burst detection)";
      summary = "Bursts: ";
      for (int k = 0; k < n; k++) summary += (k ? "; " : "") + b[size_t(k)].label + " " + std::to_string(b[size_t(k)].start) + "-" + std::to_string(b[size_t(k)].end) + " (strength " + fmtNum(b[size_t(k)].strength, 1) + ")";
    } else if (id == "thematic_evolution") {
      Evolution e = thematicEvolution(C, unit, {}, 2, &P->engine.thesaurus);
      if (e.periods.size() < 2 || e.themes.empty()) return "Too few years or terms for a thematic evolution.";
      out.fig = chartSankey(e, W, 320, th);
      out.title = "Thematic evolution (" + uL + ")";
      out.text = "Themes per period; bands show shared terms between themes of consecutive periods";
      for (size_t p = 0; p < e.periods.size(); p++) {
        summary += std::to_string(e.periods[p].first) + "-" + std::to_string(e.periods[p].second) + ": ";
        int k = 0;
        for (auto& t2 : e.themes) if (t2.period == int(p)) summary += string(k++ ? "; " : "") + t2.name + " (" + std::to_string(t2.size) + ")";
        summary += "\n";
      }
    } else if (id == "three_field") {
      Unit mid = un.empty() ? Unit::AllKeywords : unit;
      ThreeField tf = threeField(C, Unit::Authors, mid, Unit::Sources, 12);
      out.fig = chartThreeField(tf, "Authors", corpusUnitName(mid) ? corpusUnitName(mid) : "Keywords", "Sources", W, 380, th);
      out.title = "Three-field plot: authors, " + lower(corpusUnitName(mid) ? corpusUnitName(mid) : "keywords") + " and sources";
      out.text = "Bands connect items that occur in the same documents";
      summary = "Three-field plot of the top 12 authors, terms and sources.";
    } else if (id == "country_collaboration") {
      auto v = countryCollaboration(C, 12);
      if (v.empty()) return "The records have no country information.";
      out.fig = chartCollab(v, W, double(std::max<size_t>(6, v.size())) * 21 + 40, th);
      out.title = "Single- and multiple-country publications";
      out.text = "SCP: all authors from one country; MCP: co-authors from several countries";
      summary = "Countries: ";
      for (size_t k = 0; k < v.size(); k++) summary += (k ? "; " : "") + v[k].country + " (" + std::to_string(v[k].scp + v[k].mcp) + " documents, MCP " + std::to_string(v[k].mcp) + ")";
    } else if (id == "production_over_time") {
      Production pr = productionOverTime(C, unit, 10);
      if (pr.labels.empty() || pr.y1 <= pr.y0) return "Too little data for production over time.";
      out.fig = chartProduction(pr, W, double(pr.labels.size()) * 24 + 46, th);
      out.title = "Production over time (" + uL + ")";
      out.text = "Circle size shows documents per year; colour intensity shows citations per year";
      summary = "Top " + uL + ": " + join(pr.labels, "; ");
    } else if (id == "bradford" || id == "bradfords_law") {
      const Bradford& b = statBradford();
      if (b.sources.empty()) return "The records have no sources.";
      out.fig = chartBradford(b, W, 240, th);
      out.title = "Bradford's law: concentration of sources";
      out.text = "Core zone sources publish a third of the documents";
      summary = std::to_string(b.zone1) + " core sources: ";
      for (size_t k = 0; k < std::min<size_t>(size_t(b.zone1), std::min<size_t>(10, b.sources.size())); k++) summary += (k ? "; " : "") + b.sources[k].first;
    } else if (id == "lotka" || id == "lotkas_law") {
      const Lotka& lk = statLotka();
      out.fig = chartLotka(lk, W, 240, th);
      out.title = "Lotka's law: author productivity";
      out.text = "Share of authors by number of documents";
      summary = "Fitted exponent " + fmtNum(lk.exponent, 2) + " (classic value 2), R2 " + fmtNum(lk.r2, 2) + ".";
    } else if (id == "rpys") {
      bool anyRefs = false;
      for (auto& r : C.recs) if (!r.refs.empty()) { anyRefs = true; break; }
      if (!anyRefs) return "RPYS needs cited references in the records.";
      Rpys r = rpys(C, 0, 0);
      out.fig = chartRpys(r, W, 240, th);
      out.title = "Reference publication year spectroscopy";
      out.text = "Cited references by publication year; peaks mark the historical roots of the field";
      summary = "Peak years: ";
      for (size_t k = 0; k < r.peaks.size(); k++) summary += (k ? "; " : "") + std::to_string(r.peaks[k].year) + " (" + truncate(r.peaks[k].topRef, 80) + ")";
    } else if (id == "main_path") {
      const MainPath& M = mainPathNow();
      if (!M.ok) return M.error.empty() ? string("No main path was found (needs citations between the documents).") : M.error;
      out.fig = chartMainPath(C, M, W, 330, th);
      out.title = "Main path of the citation network";
      out.text = "Documents by publication year; the blue line is the global main path by search path count, orange documents lie on key routes";
      summary = "Global main path, oldest first: ";
      for (size_t k = 0; k < M.global.size(); k++) summary += (k ? "; " : "") + string("[R") + std::to_string(M.global[k] + 1) + "] " + shortCite(C.recs[size_t(M.global[k])]);
      summary += ". " + plural(M.dagNodes, "document") + " and " + plural(M.dagLinks, "citation") + " in the citation network.";
    } else if (id == "records_flow") {
      out.fig = chartFlow(recordsFlow(), 520, 560, th);
      out.title = "Records flow";
      out.text = "Records from the search to the analysis";
      summary = "Records flow diagram.";
    } else {
      return "Unknown chart \"" + id0 + "\". See the add_chart description for the list.";
    }
  }
  if (out.fig.W <= 0 || out.fig.items.empty()) return "The chart could not be drawn from these records.";
  string ti = trim(g["title"].str()), ca = trim(g["caption"].str());
  if (!ti.empty()) out.title = truncate(ti, 160);
  if (!ca.empty()) out.text = truncate(ca, 400);
  while (!out.title.empty() && out.title.back() == '.') out.title.pop_back();
  while (!out.text.empty() && out.text.back() == '.') out.text.pop_back();
  out.figId = truncate(summary, 1800);
  return "";
}

bool App::agentExportReport(const Json& g, string& msg) {
  ReportDoc d = agentReport;
  d.title = trim(g["title"].str());
  if (d.title.empty()) d.title = "Literature report";
  d.subtitle = trim(g["subtitle"].str());
  SYSTEMTIME st;
  GetLocalTime(&st);
  static const char* months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  d.date = "Prepared with VOSStudio on " + std::to_string(st.wDay) + " " + months[clampv(int(st.wMonth) - 1, 0, 11)] + " " + std::to_string(st.wYear) +
           ". Text written by an AI model from the papers listed; check it before use.";
  if (hasCorpus()) {
    const Growth& gr = statGrowth();
    vector<string> src;
    for (auto& f : P->corpus.files) src.push_back(f.name);
    string m = "Data: " + plural(long(P->corpus.recs.size()), "record");
    if (gr.y0 && gr.y1) m += ", " + std::to_string(gr.y0) + "\xE2\x80\x93" + std::to_string(gr.y1);
    if (!src.empty()) m += " (" + truncate(join(src, "; "), 160) + ")";
    d.meta.push_back(m);
  }
  vector<int> order = numberCitations(d, int(P->corpus.recs.size()));
  for (int r : order) d.references.push_back(apaCitation(P->corpus.recs[size_t(r - 1)]));
  vector<Scene> pages = layoutReport(d);
  string slug;
  for (char ch : lower(d.title)) {
    if (isalnum(uint8_t(ch))) slug += ch;
    else if (!slug.empty() && slug.back() != '-') slug += '-';
    if (slug.size() >= 48) break;
  }
  while (!slug.empty() && slug.back() == '-') slug.pop_back();
  if (slug.empty()) slug = "report";
  char stamp[32];
  snprintf(stamp, sizeof stamp, "%04d-%02d-%02d-%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
  string path = reportsDir() + "\\" + slug + "-" + stamp + ".pdf";
  if (!writeFileU(path, toPDF(pages, d.title))) { msg = "The PDF could not be written to " + path + "."; return false; }
  agentReportPath = path;
  agentReportTurn = agent.turn;
  agentReportExported = true;
  msg = "Saved the report as " + path + ": " + plural(long(pages.size()), "page") + ", " + plural(d.sections(), "section") + ", " + plural(d.figures(), "figure") + ", " +
        plural(long(d.references.size()), "reference") + ". The user can open it from the conversation.";
  if (!agent.log.empty()) agent.log.back().detail = plural(long(pages.size()), "page") + ", " + plural(long(d.references.size()), "reference");
  ui.toast("Report saved", fileName(path), 1, 5);
  return true;
}

}  // namespace win
}  // namespace vs
