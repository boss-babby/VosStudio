// VOSStudio Native: the Assistant's agent. The model plans with the application's tools (ai::agentTools), one JSON
// action per reply; reading and view tools run at once, changes wait for the user's approval and can be undone together.
#include "app.h"
#include "../core/oa.h"
#include "../core/docx.h"

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
  agentReportExported = false;  // the document stays: it belongs to the user and lives in the writer
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
  if (!wdoc.empty()) ctx += "\nThe document in the writer has " + writerSummary() + " (see get_report; add_section, add_chart and edit_section change it).\n";
  if (!agentUndos.empty()) ctx += "\n(" + plural(long(agentUndos.size()), "earlier change") + " by an assistant can still be undone by the user, newest first.)\n";
  agent.msgs.push_back({"user", ai::agentGoalMessage(goal, ctx)});
  agentRequest();
  aiTurns[size_t(agent.turn)].text = agentRender();
}

void App::agentRequest() {
  agent.req = aiRequest(ai::buildChat(aiConfig(), ai::agentSystemPrompt(), agent.msgs, true));
}

void App::agentStop() {
  if (!agent.active) return;
  if (agent.writeReq) { agent.writeReq->cancel = true; agent.writeReq.reset(); }
  agent.waitWrite = false;
  agent.waitShot = false;
  if (agent.live) {  // a Live AI tool: the model gets an error result; a running build simply finishes unreported
    string tool = agent.waitApproval ? agent.pending.tool : (agent.waitJob || agent.waitCmds ? agent.jobTool : live.toolName);
    if (agent.waitApproval && !agent.log.empty()) agent.log.back().status = 3;
    agent.waitApproval = false;
    agent.waitJob = false;
    agent.waitCmds = false;
    agent.cmds.clear();
    liveToolDone(tool, false, "The user stopped this tool.");
    return;
  }
  agent.waitCmds = false;
  agent.cmds.clear();
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
  aiSyncTurns();  // a project was opened or created meanwhile: the run ends (unless a tool of the run opened it)
  if (!agent.active) return;
  if (!agent.live && (agent.turn < 0 || agent.turn >= int(aiTurns.size()))) { agent = AgentRun(); return; }
  if (agent.waitCmds) { agentPumpCommands(); return; }
  if (agent.waitWrite) {  // write_report: the text model is drafting
    if (!agent.writeReq) { agent.waitWrite = false; agentFinishTool(agent.jobTool, false, "The writing request was lost."); return; }
    string text, err;
    {
      std::lock_guard<std::mutex> lk(agent.writeReq->m);
      if (!agent.writeReq->done) { ui.animating = true; return; }
      text = agent.writeReq->text;
      err = agent.writeReq->err;
    }
    agent.writeReq.reset();
    agent.waitWrite = false;
    agentWriteFinish(text, err);
    return;
  }
  if (agent.waitShot) return;  // look_at_screen: the frame end sends the picture and finishes the tool
  if (agent.waitJob) {
    if (jobSeq == agent.jobSeq) return;
    agent.waitJob = false;
    if (agent.jobTool == "get_pdfs") {
      if (!lastJobError.empty()) { agentFinishTool(agent.jobTool, false, "Getting the PDFs failed: " + lastJobError); return; }
      string res;
      if (fetchRun) {
        std::lock_guard<std::mutex> lk(fetchRun->mu);
        res = std::to_string(fetchRun->attached) + " of " + plural(fetchRun->total, "paper") + " attached and linked (files in " + fetchRun->dir + ").";
        if (fetchRun->closed) res += " " + plural(fetchRun->closed, "paper") + " not openly available (the user can attach them by hand or read them on the DOI page).";
        if (fetchRun->refused) res += " " + plural(fetchRun->refused, "paper") + " open but refused by the site" + (fetchRun->cachedNoKey && !fetchRun->haveKey ? " (OpenAlex has a copy of " + std::to_string(fetchRun->cachedNoKey) + ": a free OpenAlex key in Preferences would fetch them)." : ".");
        if (fetchRun->budget) res += " " + plural(fetchRun->budget, "paper") + " waiting for OpenAlex's daily budget (midnight UTC).";
        if (fetchRun->failed) res += " " + plural(fetchRun->failed, "lookup") + " failed.";
        if (fetchRun->cancelled) res += " The run was stopped by the user.";
        if (!agent.log.empty()) agent.log.back().detail = std::to_string(fetchRun->attached) + "/" + std::to_string(fetchRun->total) + " attached";
      }
      res += "\nThe Read page lists the papers without a PDF with the reason for each. " + fetchSummary();
      agentFinishTool(agent.jobTool, true, res);
      return;
    }
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
  if (actionKind(a) == ai::ToolKind::Change && !agent.allowAll) {
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
  if (agent.live) { liveToolDone(tool, ok, result); return; }
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
  if (agent.changes > 0) ui.toast("Agent finished", plural(agent.changes, "change") + " made. The Assistant can undo them one at a time, newest first.", 1, 5);
  else if (page != PG_AI && !failed) ui.toast("Agent finished", "The answer is in the Assistant.", 1, 4);
}

// ------------------------------------------------------------------ rendering
ai::ToolKind App::actionKind(const ai::AgentAction& a) const {
  const ai::ToolSpec* spec = ai::findTool(a.tool);
  if (!spec) return ai::ToolKind::Read;
  if (a.tool == "run_commands") {  // a change only when one of the commands changes the project or writes a file
    const Json& c = a.args["commands"];
    vector<string> lines;
    if (c.t == Json::Arr) { for (size_t i = 0; i < c.size(); i++) lines.push_back(c[i].str()); }
    else if (c.t == Json::Str) lines = splitAny(c.str(), "\n;");
    for (auto& l : lines) if (commandKind(split(trim(l), ' ')[0]) == 1) return ai::ToolKind::Change;
    return ai::ToolKind::View;
  }
  return spec->kind;
}

string App::agentDescribe(const ai::AgentAction& a) const {
  const Json& g = a.args;
  const string& t = a.tool;
  if (t == "get_ui_state") return "Look at the interface";
  if (t == "list_commands") return "Read the command reference";
  if (t == "new_project") return g["discard_unsaved"].boolean(false) ? "Start a blank project (discard the current one)" : "Start a blank project";
  if (t == "show_papers") { string q = trim(g["query"].str()); return q.empty() ? "Show the papers table" : "Show the papers table for \"" + truncate(q, 40) + "\""; }
  if (t == "show_chart") { string c = normName(g["chart"].str()); return c.empty() || c == "none" || c == "close" ? "Show the map again" : "Show the " + replaceAll(c, "_", " ") + " chart in the main area"; }
  if (t == "run_commands") {
    const Json& c = g["commands"];
    vector<string> lines;
    if (c.t == Json::Arr) { for (size_t i = 0; i < c.size(); i++) lines.push_back(trim(c[i].str())); }
    else if (c.t == Json::Str) { for (auto& l : splitAny(c.str(), "\n;")) lines.push_back(trim(l)); }
    string d = lines.empty() ? string("commands") : truncate(join(lines, "; "), 70);
    return "Run: " + d;
  }
  if (t == "load_data") {
    string src = normName(g["source"].str("files"));
    if (src.rfind("sample", 0) == 0 || src == "scopus" || src == "wos") return string("Load the ") + (src.find("scopus") != string::npos ? "Scopus" : "Web of Science") + " sample";
    const Json& pj = g["paths"];
    string first = pj.t == Json::Arr && pj.size() ? fileName(pj[size_t(0)].str()) : pj.t == Json::Str ? fileName(pj.str()) : string();
    if (src == "project") return "Open the project " + first;
    return "Import " + (pj.t == Json::Arr && pj.size() > 1 ? plural(long(pj.size()), "file") : first.empty() ? string("files") : first);
  }
  if (t == "save_project") return "Save the project" + (trim(g["path"].str()).empty() ? string() : " as " + fileName(g["path"].str()));
  if (t == "set_look") return "Change the look";
  if (t == "export_figure") return string("Export the ") + (lower(g["what"].str("figure")) == "view" ? "view" : "figure") + " as " + upper(g["format"].str("png"));
  if (t == "screenshot") return "Take a screenshot";
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
  if (t == "focus_cluster") return g["cluster"].integer(1) <= 0 ? string("Clear the cluster highlight") : "Highlight cluster " + std::to_string(g["cluster"].integer(1));
  if (t == "show_compare") { string l = lower(trim(g["layout"].str())); return l == "none" || l == "off" ? string("Close the comparison") : l == "difference" ? string("Show the difference map") : string("Compare side by side"); }
  if (t == "open_page") return "Open " + g["page"].str("data") + (trim(g["tab"].str()).empty() ? string() : " \xE2\x80\xBA " + g["tab"].str());
  if (t == "reading_list") return "Read the reading list";
  if (t == "get_pdfs") return "Get the open-access PDFs of " + (lower(trim(g["papers"].str("all"))) == "all" ? string("every record without one") : truncate(g["papers"].str(), 40));
  if (t == "open_pdf") return "Open the PDF of " + truncate(g["paper"].str(), 40);
  if (t == "set_reading") return "Update the reading of " + truncate(g["paper"].str(), 40);
  if (t == "paper_notes") return "Read the notes on " + truncate(g["paper"].str(), 40);
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
  if (t == "write_report") { const Json& sec = g["sections"]; return "Draft the report" + (sec.t == Json::Arr && sec.size() ? " (" + plural(long(sec.size()), "section") + ")" : string()) + " with the text model"; }
  if (t == "edit_section") return string(g["remove"].boolean(false) ? "Remove" : "Edit") + " section " + std::to_string(g["index"].integer(1));
  if (t == "look_at_screen") return string("Look at the ") + (lower(g["what"].str("canvas")) == "window" ? "window" : "map canvas");
  if (t == "export_report") { string f = lower(trim(g["format"].str())); return f == "docx" || f == "word" ? "Export the report as Word (.docx)" : f == "both" ? "Export the report as PDF and Word" : "Export the report as PDF"; }
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
void App::agentSnapshot(const string& label, bool withCorpus) {
  AgentUndo u;
  u.valid = true;
  u.label = label;
  u.turn = agent.live ? -1 : agent.turn;
  u.live = agent.live;
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
  if (withCorpus) { u.hasCorpus = true; u.corpus = P->corpus; }
  agentUndos.push_back(std::move(u));
  while (agentUndos.size() > kAgentUndoMax) agentUndos.erase(agentUndos.begin());
  size_t withC = 0;  // record copies are large: only the newest few keep one
  for (size_t i = agentUndos.size(); i-- > 0;) {
    if (!agentUndos[i].hasCorpus) continue;
    if (++withC > kAgentUndoCorpusMax) { agentUndos[i].hasCorpus = false; agentUndos[i].corpus = Corpus(); }
  }
}

void App::agentUndoChanges() {
  if (agentUndos.empty()) return;
  if (busy()) { ui.toast("Undo", "Wait for the current task to finish.", 2, 3); return; }
  if (agent.active && !agent.live) agentStop();
  if (agent.active && agent.live && !agent.waitApproval) { ui.toast("Undo", "Wait for the live assistant's current tool to finish.", 2, 3); return; }
  AgentUndo u = std::move(agentUndos.back());
  agentUndos.pop_back();
  bool thChanged = u.th.sig() != P->engine.thesaurus.sig() || u.excluded != P->engine.excluded;
  if (u.hasCorpus) {  // the change replaced or extended the records: they come back too
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
  sensSig.clear();
  variantsScanned = false;
  undoStack.clear();
  redoStack.clear();
  afterMapChanged(true);
  styleDirty = true;
  aiLayout_.clear();
  if (u.live && live.changes > 0) live.changes--;
  string more = agentUndos.empty() ? string("Nothing older to undo.") : plural(long(agentUndos.size()), "earlier change") + " can still be undone.";
  ui.toast("Undone: " + truncate(u.label, 60), more, 1, 4);
}

// ------------------------------------------------------------------ tools
void App::agentExecute(const ai::AgentAction& a) {
  const string& t = a.tool;
  const Json& g = a.args;
  auto done = [&](bool ok, const string& r) { agentFinishTool(t, ok, r); };
  auto needMap = [&]() { if (hasMap()) return true; done(false, "There is no map yet. Build one with build_map first."); return false; };
  auto needCorpus = [&]() { if (hasCorpus()) return true; done(false, "No records are loaded (the map was imported), so this needs records."); return false; };
  auto change = [&](bool corpus = false) {  // one undo step per change; corpus = the records are replaced or extended
    agentSnapshot(agentDescribe(a), corpus);
    agent.changes++;
  };
  const Network& N = P->net;
  // the loaded records are the user's work: tools that would replace them refuse unless the model says so explicitly
  auto loadedDesc = [&]() {
    string d = plural(long(P->corpus.recs.size()), "record");
    if (!P->corpus.files.empty()) { vector<string> f; for (size_t i = 0; i < P->corpus.files.size() && i < 3; i++) f.push_back(fileName(P->corpus.files[i].name)); d += " from " + join(f, ", ") + (P->corpus.files.size() > 3 ? ", \xE2\x80\xA6" : ""); }
    if (hasMap()) d += " and a map of " + plural(N.n(), N.unitNoun);
    if (!P->path.empty()) d += " (project " + fileName(P->path) + (P->dirty ? ", unsaved changes" : "") + ")";
    else if (P->dirty) d += " (never saved)";
    return d;
  };
  auto refuseReplace = [&](const string& how) {
    done(false, "Refused: the project already holds " + loadedDesc() + ", and this call would discard them. " + how +
                " Only discard the user's data when they asked for that, and say so.");
  };

  if (t == "new_project") {
    if (busy()) { done(false, "Another task is running (" + jobLabel + "). Try again in a moment."); return; }
    bool has = hasCorpus() || hasMap();
    if (has && P->dirty && !g["discard_unsaved"].boolean(false)) {
      done(false, "Not done: the current project has unsaved work (" + loadedDesc() + "). Ask the user whether to save it (save_project) or to discard it; "
                  "to discard, call new_project again with discard_unsaved true. If they want to keep it and add data, use append or load into the current project instead.");
      return;
    }
    if (has) change(true);  // the records and the map come back with Undo
    newProjectNow();
    showStart = false;
    if (page == PG_AI) page = PG_DATA;
    done(true, "Started a blank project: no records, no map. Nothing of the previous project is loaded any more (the user can undo this). Add data with search_openalex or load_data, then build_map.");
    return;
  }
  if (t == "show_papers") {
    if (!needCorpus()) return;
    string q = trim(g["query"].str());
    int sort = papersSortFromName(g["sort"].str());
    bool desc = !g.has("descending") || g["descending"].boolean(true);
    if (sort < 0 && g.has("sort") && !trim(g["sort"].str()).empty()) { done(false, "Unknown sort \"" + g["sort"].str() + "\": use year, title, authors, source or citations."); return; }
    papersFilter = q;
    openPapers(q, sort, desc);
    if (g.has("detailed")) papersDetailed = g["detailed"].boolean(true);
    string res = papersSummary(clampv(g["n"].integer(15), 0, 40));
    if (papersRows.empty() && !q.empty()) res += "No paper matches; the table shows an empty list with that filter. Try fewer words.\n";
    done(true, res + "The table is on screen in the main area (the map is hidden behind it until it is closed or a view is chosen).");
    return;
  }
  if (t == "show_chart") {
    string id = normName(g["chart"].str());
    if (id.empty() || id == "none" || id == "close" || id == "map") {
      bool was = mainChartOpen;
      mainChartOpen = false;
      mainChartSummary.clear();
      done(true, was ? "The chart is closed; the main area shows the map again." : "No chart was open; the main area shows the map" + string(hasMap() ? "." : " (there is no map yet)."));
      return;
    }
    string err = agentShowChart(id, g);
    if (!err.empty()) { done(false, err); return; }
    done(true, "The \"" + mainChart.title + "\" chart now fills the main area (" + std::to_string(int(canvasR.w / ui.s)) + "\xC3\x97" + std::to_string(int(canvasR.h / ui.s)) + " px; the user sees it; it stays until show_chart none, another chart or a map view). " +
               (mainChartSummary.empty() ? string() : "What it shows: " + mainChartSummary));
    return;
  }

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
    if (c <= 0) { clearScope(); done(true, "The cluster highlight is cleared; pages and the papers table show all records again."); return; }
    if (c > N.nClusters) { done(false, "Cluster numbers run from 1 to " + std::to_string(N.nClusters) + "."); return; }
    focusCluster(c - 1);
    string msg = "Cluster " + std::to_string(c) + " (" + N.clusterName(c - 1) + ") is highlighted.";
    if (scopeActive()) msg += " Linked selection: the Trends and Actors pages, the papers table and the geo view now cover its " + plural(long(scopeRecs_.size()), "record") + " (of " + fmtInt(long(P->corpus.recs.size())) + ").";
    else if (!linkScope) msg += " Linked selection is off, so the pages still show all records.";
    done(true, msg);
  } else if (t == "show_compare") {
    if (!needMap()) return;
    string kind = normName(g["kind"].str()), layout = normName(g["layout"].str());
    if (kind == "periods" || kind == "period" || kind == "years") cmpKind = 0;
    else if (kind == "sources" || kind == "files" || kind == "source") cmpKind = 1;
    else if (kind == "thresholds" || kind == "threshold") cmpKind = 2;
    else if (!kind.empty()) { done(false, "kind must be periods, sources or thresholds."); return; }
    if (layout == "none" || layout == "off" || layout == "close") { compareClose(); diffClear(); done(true, "The comparison is closed."); return; }
    if (cmpKind == 0) {
      int a0 = g["a_from"].integer(0), a1 = g["a_to"].integer(0), b0 = g["b_from"].integer(0), b1 = g["b_to"].integer(0);
      if (a0 && a1 && b0 && b1) { cmpA0 = a0; cmpA1 = a1; cmpB0 = b0; cmpB1 = b1; }
      else if (!cmpA0 && hasCorpus()) suggestPeriods(P->corpus, cmpA0, cmpA1, cmpB0, cmpB1);
    } else if (cmpKind == 1) {
      auto pick = [&](const Json& v, int& out) {
        if (v.t == Json::Num) { int k = v.integer(0); if (k >= 1 && size_t(k) <= P->corpus.files.size()) out = k - 1; return; }
        string want = lower(trim(v.str()));
        if (want.empty()) return;
        for (size_t k = 0; k < P->corpus.files.size(); k++) if (contains(lower(fileName(P->corpus.files[k].name)), want)) { out = int(k); return; }
        if (isdigit(uint8_t(want[0]))) { int k = toInt(want); if (k >= 1 && size_t(k) <= P->corpus.files.size()) out = k - 1; }
      };
      pick(g["file_a"], cmpFileA);
      pick(g["file_b"], cmpFileB);
    } else {
      if (g["min_a"].num(0) > 0) cmpThA = g["min_a"].num(0);
      if (g["min_b"].num(0) > 0) cmpThB = g["min_b"].num(0);
    }
    cmpValid = false;
    pdiffValid = false;
    page = PG_TRENDS;
    trTab = 3;
    string err;
    if (layout == "difference" || layout == "diff" || layout == "difference_map") {
      if (cmpKind == 2) { done(false, "Thresholds have no difference map; use layout side_by_side."); return; }
      diffCompute();
      if (!pdiff.ok) { done(false, pdiff.error); return; }
      diffShowOnMap();
      done(true, "The difference map is on the map: " + std::to_string(pdiff.counts[0]) + " appearing, " + std::to_string(pdiff.counts[1]) + " growing, " + std::to_string(pdiff.counts[2]) + " stable, " + std::to_string(pdiff.counts[3]) + " fading items (A: " + plural(pdiff.nA, "document") + ", B: " + plural(pdiff.nB, "document") + "). The Trends \xE2\x80\xBA Compare tab lists them.");
      return;
    }
    if (!compareShowSideBySide(&err)) { done(false, err); return; }
    done(true, "Side-by-side comparison is in the main area: " + compareSummary() + ".");
  } else if (t == "open_page") {
    static const NameMap pages[] = {{"data", PG_DATA}, {"build", PG_BUILD}, {"look", PG_LOOK}, {"analyse", PG_ANALYSE}, {"analyze", PG_ANALYSE},
                                    {"trends", PG_TRENDS}, {"actors", PG_ACTORS}, {"read", PG_READ}, {"reader", PG_READ}, {"library", PG_READ}, {"publish", PG_PUBLISH}, {"write", PG_WRITER}, {"writer", PG_WRITER}};
    string k = normName(g["page"].str());
    for (auto& p : pages) {
      if (k != p.name) continue;
      if (readerOpen && p.v != PG_READ) closeReader();
      page = Page(p.v);
      if (p.v == PG_WRITER && !writerOpen) openWriter();
      string tabName = normName(g["tab"].str()), tabNote;
      if (!tabName.empty()) {
        static const NameMap ana[] = {{"clusters", 0}, {"items", 1}, {"network", 2}, {"stability", 3}, {"sweep", 3}};
        static const NameMap tr[] = {{"growth", 0}, {"bursts", 1}, {"themes", 2}, {"thematic_evolution", 2}, {"compare", 3}, {"comparison", 3}, {"difference_map", 3},
                                     {"three_field", 4}, {"3_field", 4}, {"topics", 5}, {"rpys", 6}, {"main_path", 7}};
        static const NameMap ac[] = {{"authors", 0}, {"sources", 1}, {"journals", 1}, {"countries", 2}, {"organisations", 3}, {"organizations", 3}, {"documents", 4}, {"laws", 5}, {"bibliometric_laws", 5}};
        int found = -1;
        if (page == PG_ANALYSE) { for (auto& m : ana) if (tabName == m.name) found = m.v; if (found >= 0) anaTab = found; }
        else if (page == PG_TRENDS) { for (auto& m : tr) if (tabName == m.name) found = m.v; if (found >= 0) trTab = found; }
        else if (page == PG_ACTORS) { for (auto& m : ac) if (tabName == m.name) found = m.v; if (found >= 0) acTab = found; }
        else if (isdigit(uint8_t(tabName[0]))) { found = toInt(tabName, 0); anaTab = trTab = acTab = found; }
        if (found < 0 && isdigit(uint8_t(tabName[0]))) { found = toInt(tabName, 0); if (page == PG_ANALYSE) anaTab = found; else if (page == PG_TRENDS) trTab = found; else if (page == PG_ACTORS) acTab = found; }
        tabNote = found >= 0 ? " on the " + g["tab"].str() + " tab" : " (no tab named " + g["tab"].str() + " there)";
      }
      done(true, "Opened " + k + tabNote + ".");
      return;
    }
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
    bool app = g["append"].boolean(false);
    if (!app && hasCorpus() && !g["replace"].boolean(false)) {
      refuseReplace("Choose one: append true adds the results to the loaded records; new_project first starts a fresh project for a new topic; replace true discards the loaded records (only when the user asked to start over).");
      return;
    }
    change(true);
    oaKind = sem ? 1 : 0;
    if (sem) oaSemQueries = {q};
    else oaQuery = q;
    int y0 = g["from_year"].integer(0), y1 = g["to_year"].integer(0);
    oaFrom = y0 > 0 ? std::to_string(y0) : string();
    oaTo = y1 > 0 ? std::to_string(y1) : string();
    oaMax = std::to_string(clampv(g["max"].integer(300), 50, 2000));
    oaAppend = app;
    oaMode = 0;
    oaField = 1;  // titles and abstracts: fewer off-topic works than full-text matches
    agent.jobTool = t;
    agent.jobSeq = jobSeq;
    cmdOpenAlex();
    if (!busy()) { done(false, lastJobError.empty() ? string("The search could not start.") : lastJobError); return; }
    agent.waitJob = true;
    if (agent.turn >= 0 && agent.turn < int(aiTurns.size())) aiTurns[size_t(agent.turn)].text = agentRender();
  } else if (t == "get_pdfs") {
    if (!needCorpus()) return;
    if (busy()) { done(false, "Another task is running. Try again later."); return; }
    string which = lower(trim(g["papers"].str("all")));
    vector<int> among;
    if (which == "selection") {
      for (size_t i = 0; i < papersSel.size(); i++) if (papersSel[i]) among.push_back(int(i));
      if (among.empty()) { done(false, "No papers are selected in the papers table."); return; }
    } else if (which.empty() || which == "all") {
      for (size_t i = 0; i < P->corpus.recs.size(); i++) among.push_back(int(i));
    } else {
      for (auto& tok : splitAny(which, " ,;")) {
        string x = trim(tok);
        if (x.size() > 1 && x[0] == 'r' && isdigit(uint8_t(x[1]))) { int rec = toInt(x.substr(1), 0) - 1; if (rec >= 0 && size_t(rec) < P->corpus.recs.size()) among.push_back(rec); }
      }
      if (among.empty()) { done(false, "papers must be all, selection, or record ids such as R3, R7."); return; }
    }
    const bool retry = g["retry"].boolean(false);
    vector<int> cands;
    int haveFile = 0, noDoi = 0, skipped = 0;
    for (int rec : among) {
      const Record& r = P->corpus.recs[size_t(rec)];
      if (fetchRecordHasFile(rec)) { haveFile++; continue; }
      if (oa::normDoi(r.doi).empty()) { noDoi++; continue; }
      auto f = P->library.fetch.find(PdfLibrary::keyOf(r));
      if (!retry && f != P->library.fetch.end() && f->second.needsFile()) { skipped++; continue; }
      cands.push_back(rec);
    }
    if (cands.empty()) {
      done(true, "Nothing to fetch: " + std::to_string(haveFile) + " already have a PDF, " + std::to_string(noDoi) + " have no DOI" + (skipped ? ", " + std::to_string(skipped) + " were tried before (retry true tries them again)" : string()) + ". " + fetchSummary());
      return;
    }
    change(false);
    agent.jobTool = t;
    agent.jobSeq = jobSeq;
    if (!fetchPdfs(cands)) { done(false, lastJobError.empty() ? string("The download could not start.") : lastJobError); return; }
    agent.waitJob = true;
    if (!agent.log.empty()) agent.log.back().detail = plural(long(cands.size()), "paper") + "\xE2\x80\xA6";
    if (agent.turn >= 0 && agent.turn < int(aiTurns.size())) aiTurns[size_t(agent.turn)].text = agentRender();
  } else if (t == "read_papers") {
    if (!needCorpus()) return;
    string s = agentReadPapers(g);
    done(!s.empty(), s.empty() ? string("No paper matches this query. Try fewer or other words.") : s);
  } else if (t == "add_chart") {
    ReportBlock b;
    string err = agentChart(g["chart"].str(), g, b);
    if (!err.empty()) { done(false, err); return; }
    string summary = b.figId;  // the data summary (set by agentChart)
    string chartId = normName(g["chart"].str());
    int asset = wdoc.addAsset(b.fig, chartId, b.title, summary);
    int blk = docAppendFigure(asset, b.text.empty() ? b.title : b.text);
    int nf = wdoc.figureNumber(blk);
    string s = "Figure " + std::to_string(nf) + " added to the document: " + b.title + ".";
    if (!b.text.empty()) s += " Caption: " + b.text;
    s += "\n" + summary;
    if (!agent.log.empty()) agent.log.back().detail = "Figure " + std::to_string(nf);
    done(true, s);
  } else if (t == "add_section") {
    string title = trim(g["title"].str()), text = g["text"].t == Json::Str ? g["text"].str() : string();
    if (trim(text).empty()) { done(false, "Give the section text."); return; }
    int words = 0, cites = 0, bad = 0;
    int idx = docAppendSection(truncate(title, 140), replaceAll(text, "\r", ""), &words, &cites, &bad);
    string s = "Section " + std::to_string(idx) + " added to the document (" + plural(words, "word") + ", " + plural(cites, "citation") + "). The document now has " + writerSummary() + ".";
    if (bad) s += " " + plural(bad, "citation") + " did not match a paper id and were left out.";
    if (!agent.log.empty()) agent.log.back().detail = plural(words, "word") + (cites ? ", " + plural(cites, "citation") : string());
    done(true, s);
  } else if (t == "get_report") {
    done(true, agentReportText(g["full"].boolean(false)));
  } else if (t == "write_report") {
    if (!needCorpus()) return;
    ai::Config cfg = aiConfig();
    if (!cfg.ready()) { done(false, "The text model is not configured (Settings \xE2\x86\x92 AI Assistant: provider, key and model). Write the sections yourself with add_section instead."); return; }
    if (agent.waitWrite) { done(false, "A draft is already being written."); return; }
    agentWriteStart(g, t);
  } else if (t == "edit_section") {
    int idx = g["index"].integer(1);
    vector<DocSection> secs;
    for (auto& sc : docSections(wdoc)) if (sc.heading >= 0) secs.push_back(sc);
    if (idx < 1 || idx > int(secs.size())) { done(false, secs.empty() ? string("The document has no sections yet (a section starts with a Heading 1).") : "There is no section " + std::to_string(idx) + "; the document has " + plural(long(secs.size()), "section") + " (see get_report)."); return; }
    DocSection sec = secs[size_t(idx) - 1];
    if (g["remove"].boolean(false)) {
      wed.begin("Assistant: remove section");
      docReplaceBlocks(wdoc, sec.first, sec.last, {});
      wed.externalChange();
      done(true, "Section " + std::to_string(idx) + " (" + sec.title + ") removed. " + agentReportText(false));
      return;
    }
    string title = trim(g["title"].str()), text = g["text"].t == Json::Str ? g["text"].str() : string();
    if (title.empty() && trim(text).empty()) { done(false, "Give a new title, a new text, or remove = true."); return; }
    wed.begin("Assistant: edit section");
    int words = 0, cites = 0, bad = 0;
    if (!title.empty()) { Para& hp = wdoc.blocks[size_t(sec.heading)].p; hp.spans = {Span{truncate(title, 140), 0, ""}}; }
    if (!trim(text).empty()) {
      string body = docCitedMarkdown(replaceAll(text, "\r", ""), &cites, &bad);
      vector<Block> blocks = blocksFromMarkdown(body, 2, title.empty() ? sec.title : title);
      if (sec.last >= sec.heading + 1) docReplaceBlocks(wdoc, sec.heading + 1, sec.last, blocks);
      else wdoc.blocks.insert(wdoc.blocks.begin() + sec.heading + 1, blocks.begin(), blocks.end());
      for (auto& w : splitAny(text, " \n\t")) words += !w.empty();
    }
    wed.externalChange();
    string s = "Section " + std::to_string(idx) + " updated" + (words ? " (" + plural(words, "word") + ", " + plural(cites, "citation") + ")" : string()) + ".";
    if (bad) s += " " + plural(bad, "citation") + " did not match a paper id and were left out.";
    done(true, s);
  } else if (t == "look_at_screen") {
    if (!agent.live) { done(false, "Only the live assistant can look at the screen. Use get_ui_state, get_overview and get_clusters for the same information as data."); return; }
    if (!live.s || live.phase != 2) { done(false, "The live session is not connected."); return; }
    bool canvas = lower(g["what"].str("canvas")) != "window";
    if (canvas && !hasMap()) { done(false, "There is no map on the canvas yet. Ask for the window instead, or build a map first."); return; }
    agent.waitShot = true;
    agent.jobTool = t;
    liveRequestPicture(canvas, false);
  } else if (t == "export_report") {
    if (wdoc.empty()) { done(false, "The document is empty. Add sections and figures first (add_section, write_report, add_chart)."); return; }
    string msg;
    bool ok = agentExportReport(g, msg);
    done(ok, msg);
  // ---------------------------------------------------------------- the whole application
  } else if (t == "get_ui_state") {
    done(true, uiStateSummary());
  } else if (t == "list_commands") {
    done(true, commandReference());
  } else if (t == "run_commands") {
    vector<string> cmds;
    const Json& c = g["commands"];
    if (c.t == Json::Arr) { for (size_t i = 0; i < c.size(); i++) cmds.push_back(trim(c[i].str())); }
    else if (c.t == Json::Str) { for (auto& l : splitAny(c.str(), "\n;")) cmds.push_back(trim(l)); }
    cmds.erase(std::remove_if(cmds.begin(), cmds.end(), [](const string& x) { return x.empty(); }), cmds.end());
    if (cmds.empty()) { done(false, "Give the commands as a list of strings, e.g. [\"view overlay\", \"page trends\"]."); return; }
    if (cmds.size() > 40) { done(false, "At most 40 commands per call."); return; }
    vector<string> bad;
    bool anyChange = false, corpusChange = false;
    for (auto& l : cmds) {
      string name = split(l, ' ')[0];
      int k = commandKind(name);
      if (k < 0) bad.push_back(name);
      else if (k == 1) anyChange = true;
      if (name == "sample" || name == "open" || name == "openproj" || name == "cleanmerge" || name == "cleanundo" || name == "oafetch" || name == "livingadd" || name == "records") corpusChange = true;
    }
    if (!bad.empty()) { done(false, "Not available to the assistant: " + join(bad, ", ") + ". See list_commands for the commands you can use."); return; }
    if (busy()) { done(false, "Another task is running (" + jobLabel + "). Try again in a moment."); return; }
    for (auto& l : cmds) {  // commands that would throw the loaded records away go through the typed tools, which carry the append/replace switches
      string name = lower(split(l, ' ')[0]);
      if (name == "openproj" && (hasCorpus() || hasMap()) && P->dirty) { refuseReplace("Use load_data with source project (and replace true once the user agreed, or save_project first)."); return; }
      if (name == "oafetch" && hasCorpus() && !oaAppend) { refuseReplace("Use search_openalex, which has append (add to the records) and replace (start over) switches, or new_project first."); return; }
    }
    if (anyChange) change(corpusChange);
    if (!agent.log.empty()) agent.log.back().detail = plural(long(cmds.size()), "command");
    agentRunCommands(t, cmds);
  } else if (t == "load_data") {
    string src = normName(g["source"].str("files"));
    vector<string> paths;
    const Json& pj = g["paths"];
    if (pj.t == Json::Arr) { for (size_t i = 0; i < pj.size(); i++) paths.push_back(trim(pj[i].str())); }
    else if (pj.t == Json::Str && !trim(pj.str()).empty()) paths.push_back(trim(pj.str()));
    if (g.has("path") && paths.empty()) paths.push_back(trim(g["path"].str()));
    paths.erase(std::remove_if(paths.begin(), paths.end(), [](const string& x) { return x.empty(); }), paths.end());
    if (busy()) { done(false, "Another task is running (" + jobLabel + "). Try again in a moment."); return; }
    vector<string> cmds;
    if (src == "sample" || src == "sample_wos" || src == "wos") cmds.push_back("sample");
    else if (src == "sample_scopus" || src == "scopus") cmds.push_back("sample scopus");
    else if (src == "project") {
      if (paths.empty()) { done(false, "Give the path of the .vosproj file."); return; }
      if ((hasCorpus() || hasMap()) && P->dirty && !g["replace"].boolean(false)) {
        refuseReplace("Opening a project closes the current one: save it first (save_project), or pass replace true when the user agreed to drop the unsaved work.");
        return;
      }
      cmds.push_back("openproj " + paths[0]);
    } else {
      if (paths.empty()) { done(false, "Give the file paths (full paths; separate files as list items)."); return; }
      for (auto& pth : paths) { bool ok = false; (void)readFileU(pth, &ok); if (!ok) { done(false, "Cannot read " + pth + ". Check the path; the user can also drop the file onto the window."); return; } }
      cmds.push_back("open " + join(paths, "|"));
    }
    change(true);
    if (!agent.log.empty()) agent.log.back().detail = src == "project" ? fileName(paths[0]) : src.rfind("sample", 0) == 0 ? "sample" : plural(long(paths.size()), "file");
    showStart = false;
    agentRunCommands(t, cmds);
  } else if (t == "save_project") {
    string pth = trim(g["path"].str());
    if (pth.empty()) pth = P->path.empty() ? agentExportPath("", "vosproj") : P->path;
    else pth = agentExportPath(pth, "vosproj");
    if (busy()) { done(false, "Another task is running. Try again in a moment."); return; }
    string err;
    mapsToProject();
    writerToProject();
    if (P->save(pth, &err)) {
      P->path = pth;
      P->dirty = false;
      settings.addRecent(pth);
      settings.save();
      if (!agent.log.empty()) agent.log.back().detail = fileName(pth);
      done(true, "Saved the project as " + pth + " (" + plural(long(P->corpus.recs.size()), "record") + ", " + plural(P->net.n(), "item") + ").");
    } else done(false, "Could not save " + pth + ": " + err);
  } else if (t == "set_look") {
    vector<string> cmds, what;
    if (g.has("theme")) { string th = lower(g["theme"].str()); if (th == "light" || th == "dark") { cmds.push_back("theme " + th); what.push_back(th + " theme"); } }
    if (g.has("look")) {
      string lk = lower(trim(g["look"].str()));
      if (lk == "clean" || lk == "publication" || lk == "clean_publication") lk = "paper";
      bool f = false;
      for (auto& l : lookPresets()) if (l.id == lk) f = true;
      if (!f) { done(false, "Unknown look \"" + g["look"].str() + "\": vosviewer, studio, paper or midnight."); return; }
      cmds.push_back("look " + lk); what.push_back("look " + lk);
    }
    auto flag = [&](const char* key, const char* cmd, const char* label) {
      if (!g.has(key)) return;
      bool v = g[key].boolean(true);
      cmds.push_back(string(cmd) + (v ? " on" : " off"));
      what.push_back(string(label) + (v ? " on" : " off"));
    };
    flag("hulls", "hulls", "hulls");
    flag("cluster_names", "names", "cluster names");
    flag("legend", "legend", "legend");
    flag("inspector", "inspector", "inspector");
    if (g.has("labels")) { showLabels = g["labels"].boolean(true); what.push_back(showLabels ? "labels on" : "labels off"); }
    if (g.has("label_halo")) { cmds.push_back(string("figopt halo ") + (g["label_halo"].boolean(true) ? "1" : "0")); what.push_back("label halo"); }
    if (g.has("link_geometry")) { string lg = lower(g["link_geometry"].str()); if (lg == "straight" || lg == "curved" || lg == "arc") { cmds.push_back("linkgeom " + lg); what.push_back(lg + " links"); } }
    if (g.has("max_lines")) { int ml = clampv(g["max_lines"].integer(1000), 0, 100000); cmds.push_back("maxlines " + std::to_string(ml)); what.push_back("max. " + std::to_string(ml) + " links"); }
    if (cmds.empty() && what.empty()) { done(false, "Nothing to change: give a look, a theme or one of the switches."); return; }
    if (!agent.log.empty()) agent.log.back().detail = join(what, ", ");
    if (cmds.empty()) { done(true, "Changed: " + join(what, ", ") + "."); return; }
    agentRunCommands(t, cmds);
  } else if (t == "export_figure") {
    if (!needMap()) return;
    string fmt = lower(trim(g["format"].str("png")));
    if (fmt != "png" && fmt != "svg" && fmt != "pdf") { done(false, "format must be png, svg or pdf."); return; }
    bool viewOnly = lower(g["what"].str("figure")) == "view";
    string pth = agentExportPath(trim(g["path"].str()), fmt);
    vector<string> cmds;
    if (!viewOnly && g.has("panels")) cmds.push_back("figpanels " + replaceAll(lower(g["panels"].str("all")), " ", ""));
    if (g.has("transparent")) cmds.push_back(string("figopt transparent ") + (g["transparent"].boolean(false) ? "1" : "0"));
    cmds.push_back((viewOnly ? "view" : "") + fmt + " " + pth);
    if (!agent.log.empty()) agent.log.back().detail = fileName(pth);
    agentRunCommands(t, cmds);
  } else if (t == "screenshot") {
    string pth = agentExportPath(trim(g["path"].str()), "png");
    if (!agent.log.empty()) agent.log.back().detail = fileName(pth);
    agentRunCommands(t, {string(g["canvas_only"].boolean(false) ? "viewshot " : "shot ") + pth});
  } else if (t == "reading_list") {
    PdfLibrary& lib = P->library;
    if (lib.empty()) { done(true, "The reading library is empty: no PDF is attached yet. The user attaches PDFs on the Read page (or drops them onto the window)."); return; }
    string f = lower(trim(g["filter"].str("all"))), q = lower(trim(g["query"].str()));
    string out;
    int n = 0;
    for (auto& it : lib.items) {
      string st = lower(statusName(it.status));
      if (f != "all" && !f.empty() && replaceAll(f, "_", " ") != st) continue;
      int rec = lib.recordIndex(P->corpus, it);
      string title = !it.title.empty() ? it.title : PdfLibrary::fileTitle(it.path);
      if (!q.empty() && !contains(lower(title + " " + it.author + " " + it.notes), q)) continue;
      n++;
      if (n > 60) break;
      out += "[" + it.id + "]" + (rec >= 0 ? " (R" + std::to_string(rec + 1) + ")" : " (not linked)") + " " + truncate(title, 110) + (it.year ? " (" + std::to_string(it.year) + ")" : string()) +
             " - " + statusName(it.status) + (it.rating ? ", rating " + std::to_string(it.rating) + "/5" : string()) + (it.tags.empty() ? string() : ", tags: " + join(it.tags, ", ")) +
             ", " + plural(it.annotCount(0x7), "highlight") + ", " + plural(it.annotCount(1 << 4), "area") + ", " + plural(it.annotCount(1 << 5), "drawing") + ", " + plural(it.annotCount(0x8), "note") + (it.notes.empty() ? string() : ", has reading notes") + "\n";
    }
    LibraryStats st = lib.stats();
    string head = plural(st.total, "paper") + " in the reading library: " + std::to_string(st.read) + " read, " + std::to_string(st.reading) + " reading, " + std::to_string(st.toRead) + " to read, " + std::to_string(st.excluded) + " excluded; " +
                  plural(st.highlights, "highlight") + ", " + plural(st.areas, "area") + ", " + plural(st.drawings, "drawing") + ", " + plural(st.notes, "note") + ".\n";
    string tail;
    if (hasCorpus()) {
      tail = "\nRecords and PDFs: " + fetchSummary() + " (get_pdfs downloads the open-access ones.)";
      vector<int> need = fetchNeedsFile();
      int shown = 0;
      for (int rec : need) {
        if (++shown > 15) { tail += "\n..."; break; }
        const Record& r = P->corpus.recs[size_t(rec)];
        auto f = lib.fetch.find(PdfLibrary::keyOf(r));
        tail += "\nR" + std::to_string(rec + 1) + " " + truncate(r.title, 80) + " - " + (f != lib.fetch.end() ? f->second.message(!openAlexKey().empty()) : string("no PDF"));
      }
    }
    done(true, head + (out.empty() ? "No paper matches the filter." : out) + tail);
  } else if (t == "open_pdf" || t == "set_reading" || t == "paper_notes") {
    string ref = trim(g["paper"].str());
    PdfItem* item = nullptr;
    string lref = lower(ref);
    if (P->library.find(ref)) item = P->library.find(ref);
    else if (lref.size() > 1 && lref[0] == 'r' && isdigit(uint8_t(lref[1]))) {
      int rec = toInt(lref.substr(1), 0) - 1;
      const PdfItem* c = readerItemForRecord(rec);
      if (c) item = P->library.find(c->id);
      else if (rec >= 0 && hasCorpus() && size_t(rec) < P->corpus.recs.size()) { done(false, "R" + std::to_string(rec + 1) + " has no PDF attached. Ask the user to attach it (Read page, or drop the file onto the window)."); return; }
    }
    if (!item && !lref.empty()) for (auto& it : P->library.items) if (contains(lower(it.title), lref) || contains(lower(fileName(it.path)), lref)) { item = &it; break; }
    if (!item) { done(false, P->library.empty() ? "The reading library is empty: no PDF is attached yet." : "No attached PDF matches \"" + ref + "\". Use reading_list to see the ids (p1, p2, ...) or the record ids (R1, R2, ...)."); return; }
    int rec = P->library.recordIndex(P->corpus, *item);
    string title = !item->title.empty() ? item->title : PdfLibrary::fileTitle(item->path);
    if (t == "open_pdf") {
      if (!fileExistsU(item->path)) { done(false, "The file is missing: " + item->path); return; }
      int pg = g["page"].integer(0);
      openReader(item->id, pg > 0 ? pg - 1 : -1, -1);
      done(true, "The reader shows \"" + truncate(title, 80) + "\"" + (pg > 0 ? " at page " + std::to_string(pg) : string()) + ". The user can highlight and code passages there; paper_notes returns what they marked.");
    } else if (t == "set_reading") {
      string st = lower(trim(g["status"].str())), tags = trim(g["tags"].str()), note = trim(g["note"].str());
      int rating = g["rating"].integer(-1);
      vector<string> changes;
      if (!st.empty()) {
        st = replaceAll(st, "_", " ");
        int idx = -1;
        for (int i = 0; i < int(ReadStatus::Count); i++) if (lower(statusName(ReadStatus(i))) == st) idx = i;
        if (idx < 0) { done(false, "status must be one of to_read, reading, read, excluded."); return; }
        ReadStatus next = ReadStatus(idx);
        if (item->status != next) { item->status = next; P->library.touchOrganization(); }
        changes.push_back("status " + string(statusName(item->status)));
      }
      if (rating >= 0) { item->rating = clampv(rating, 0, 5); changes.push_back("rating " + std::to_string(item->rating) + "/5"); }
      if (!tags.empty()) {
        vector<string> merged = item->recordKey.empty() ? item->tags : P->library.tagsForRecord(item->recordKey);
        for (auto& tg : split(tags, ',')) {
          string x = trim(tg); if (x.empty()) continue;
          bool exists = false; for (auto& old : merged) if (lower(trim(old)) == lower(x)) { exists = true; break; }
          if (!exists) merged.push_back(x);
        }
        if (item->recordKey.empty()) { if (merged != item->tags) { item->tags = merged; P->library.touchOrganization(); } }
        else P->library.setTagsForRecord(item->recordKey, merged);
        changes.push_back("tags " + join(merged, ", "));
      }
      if (!note.empty()) { item->notes += (item->notes.empty() ? "" : "\n\n") + note; changes.push_back("note added"); }
      if (changes.empty()) { done(false, "Nothing to change: give status, rating, tags or note."); return; }
      P->dirty = true;
      done(true, "\"" + truncate(title, 80) + "\": " + join(changes, "; ") + ".");
    } else {
      string md = P->library.exportMarkdown(*item, rec >= 0 ? &P->corpus.recs[size_t(rec)] : nullptr);
      done(true, md.size() > 12000 ? md.substr(0, 12000) + "\n[...]" : md);
    }
  } else {
    done(false, "This tool is not available.");
  }
}

// ------------------------------------------------------------------ application commands as a tool
// Which script commands the assistant may run. -1: not for the AI (synthetic input, settings, keys, other AI features,
// quitting), 0: reads or changes only what is shown, 1: changes the project or writes a file (asks for approval unless
// the user allows everything).
int App::commandKind(const string& name) {
  static const std::unordered_map<string, int> k = {
      {"sample", 1}, {"open", 1}, {"openproj", 1}, {"type", 1}, {"unit", 1}, {"min", 1}, {"build", 1}, {"relayout", 1}, {"bundle", 1}, {"save", 1}, {"records", 0},
      {"view", 0}, {"page", 0}, {"tab", 0}, {"theme", 0}, {"look", 0}, {"hulls", 0}, {"names", 0}, {"legend", 0}, {"inspector", 0}, {"maxlines", 0}, {"linkgeom", 0},
      {"zoom", 0}, {"orbit", 0}, {"start", 0}, {"search", 0}, {"select", 0}, {"clearsearch", 0}, {"scroll", 0}, {"preview", 0}, {"papers", 0}, {"writer", 0}, {"maps", 1}, {"docrank", 0}, {"cymode", 0},
      {"run", 0}, {"expand", 0}, {"closechart", 0}, {"sweep", 0}, {"useres", 1}, {"diffmap", 0}, {"diffclear", 0}, {"compare", 0}, {"scope", 0}, {"mainpathroutes", 0}, {"expandflow", 0}, {"flowsvg", 0},
      {"chartspdf", 0}, {"geolayer", 0}, {"geofill", 0}, {"geodenstyle", 0}, {"geodenmeasure", 0}, {"geosel", 0}, {"figpanels", 0}, {"figopt", 0}, {"figzoom", 0},
      {"svg", 0}, {"pdf", 0}, {"png", 0}, {"viewsvg", 0}, {"viewpdf", 0}, {"viewpng", 0}, {"shot", 0}, {"viewshot", 0}, {"oaquery", 0}, {"oamax", 0}, {"oafrom", 0},
      {"oakind", 0}, {"oasem", 0}, {"oasemq", 0}, {"oafetch", 1}, {"cleanunit", 0}, {"cleanscan", 0}, {"cleanai", 0}, {"cleanpick", 0}, {"cleanlabel", 0}, {"cleanmerge", 1},
      {"cleanundo", 1}, {"livingcheck", 0}, {"livingadd", 1}, {"livingauto", 1}, {"wait", 0}, {"canvascache", 0}, {"textcache", 0}, {"perf", 0}, {"themeanim", 0}, {"cite", 0}, {"split", 0}, {"pane", 0}, {"getpdf", 1}, {"rdview", 1}};
  auto it = k.find(lower(name));
  return it == k.end() ? -1 : it->second;
}

const char* App::commandReference() {
  return
      "VOSStudio commands for run_commands. One command per list item: name, a space, arguments. <required> [optional]. "
      "Commands that start work (open, build, relayout, bundle, oafetch, getpdf) are waited for before the next command runs.\n\n"
      "DATA\n"
      "sample [scopus] - load the built-in Web of Science sample (140 records) or the Scopus sample (90)\n"
      "open <file>[|<file2>...] - import bibliographic files into the loaded records (Web of Science, Scopus, RIS, PubMed, CSV, OpenAlex; a VOSviewer map|network pair opens as a map); full paths\n"
      "openproj <file.vosproj> - open a project (replaces everything)\n"
      "save <file.vosproj> - save the project\n"
      "records wos|ris|bib|csv <file> - export the loaded records (bib = BibTeX)\n"
      "start 0 - hide the start screen\n\n"
      "ANALYSIS AND MAP\n"
      "type cooc|coauth|citation|coupling|cocit - analysis type (co-occurrence, co-authorship, citation, bibliographic coupling, co-citation); resets the unit\n"
      "unit allKeywords|keywords|indexTerms|terms|authors|orgs|countries|docs|sources|refs|csources|cauthors - unit of analysis (must fit the type: cooc takes the keyword/term units, coauth authors|orgs|countries, citation docs|sources|authors|orgs|countries, coupling docs|sources|authors|orgs|countries, cocit refs|csources|cauthors)\n"
      "min <n> - minimum occurrences / documents of an item\n"
      "build - build the map with the current type, unit and minimum (layout + clusters)\n"
      "relayout - run the layout again with another seed\n"
      "bundle - compute edge bundles\n"
      "sweep - resolution sweep: clusters and reproducibility per resolution (result is returned)\n"
      "useres <r> - re-cluster with a resolution from the sweep, e.g. useres 1.5\n"
      "run bursts - detect keyword bursts (Trends > Bursts); run stability - cluster stability (Analyse > Stability)\n"
      "maps <k> - show map k of the map history (0 = first)\n\n"
      "VIEWS, PAGES, CAMERA\n"
      "view network|overlay|density|timeline|matrix|geo|3d - the main view\n"
      "page Data|Build|Look|Analyse|Trends|Actors|Publish - the left panel page\n"
      "tab <n> - tab of the open page. Analyse: 0 Clusters, 1 Items, 2 Network, 3 Stability. Trends: 0 Growth, 1 Bursts, 2 Themes, 3 Compare, 4 Three-field, 5 Topics, 6 RPYS, 7 Main path. Actors: 0 Authors, 1 Sources, 2 Countries, 3 Organisations, 4 Documents, 5 Laws\n"
      "inspector on|off - the right-hand inspector panel\n"
      "zoom <factor> - multiply the camera zoom (2 = closer, 0.5 = farther)\n"
      "orbit <radians> - rotate the 3D view\n"
      "expand strategic - show the strategic diagram in the main area; expandflow - the records-flow (PRISMA) diagram; closechart - close it\n"
      "split off|1|2|4 [ids...] - split the main area into 1, 2 (side by side), 2 (rows) or 4 panes; optional pane contents: map (the live map), a linked live view of the map (live:network, live:overlay, live:density, live:timeline, live:geo, live:3d), a map picture (map_network, map_overlay, map_density, map_timeline, map_geo, map_3d, map_matrix) or chart ids such as publications_per_year, top_sources, top_countries, most_cited, strategic_diagram (e.g. split 4 map live:density publications_per_year strategic_diagram)\n"
      "pane <1..4> <id|map> - change what one pane of the split view shows\n"
      "figzoom on|off - full-screen preview of the publication figure\n\n"
      "LOOK\n"
      "theme light|dark\n"
      "look vosviewer|studio|paper|midnight - a look preset (paper = clean publication style)\n"
      "hulls on|off - cluster hulls; names on|off - cluster names on the map; legend on|off\n"
      "linkgeom straight|curved|arc - link shape; maxlines <n> - maximum number of links drawn\n"
      "canvascache 0|1 - reuse the last render of the map while nothing on it changed (default 1; 0 renders every frame, only if the user reports a stale map)\n"
      "textcache 0|1 - reuse text layouts between frames (default 1; 0 only if the user reports garbled text)\n"
      "perf 0|1 - show or hide the performance overlay (frames per second, frame time, cache hit rates, CPU, memory)\n"
      "figopt halo 0|1 - label halos; figopt transparent 0|1 - transparent figure background; figopt pdfpages 0|1 - one panel per PDF page\n\n"
      "SEARCH AND SELECTION\n"
      "search <text> - highlight items whose label contains the text; select <text> - search and focus the first hit; clearsearch\n"
      "preview R<n>|top|-1 - open the preview of a document in the inspector (R<n> = the id from read_papers or the papers table; top = most cited; -1 closes it); papers [off|<filter words>] - the papers table in the main area (show_papers does the same with sorting); writer [on|off|pdf|docx|html|all [path]] - the document editor in the main area (the report tools write into it) or its exports; docrank 0|1|2 - document ranking (citations, recent, relevance); cymode 0|1 - citations per year\n"
      "scroll <y> - scroll the open page; scroll insp <y> - scroll the inspector\n\n"
      "COMPARISONS AND TIME\n"
      "diffmap [a0 a1 b0 b1] - difference map between two periods (years); without years the data is split in halves; diffclear - back to cluster colours\n"
      "compare periods a0 a1 b0 b1 | compare sources <a> <b> (1-based file numbers) | compare thresholds <tA> <tB> - choose what to compare; compare side - side-by-side figure in the main area; compare diff - difference map; compare off\n"
      "scope cluster <n> | scope off | scope link on|off - linked selection: the highlighted cluster (or selected items) scopes the Trends/Actors pages, the papers table and the geo view\n"
      "mainpathroutes <n> - number of key routes of the main path (0-20; citation map, Trends tab 7)\n\n"
      "GEOGRAPHY (view geo)\n"
      "geolayer network|overlay|density; geofill 0|1|2 - country fill; geodenstyle heat|clusters|countries; geodenmeasure docs|citations|percapita|collaboration\n"
      "geosel <country>|none - select a country and open its profile\n\n"
      "EXPORT\n"
      "figpanels all|network,overlay,density,timeline,geo,3d,matrix - panels of the publication figure (comma list, no spaces)\n"
      "svg|pdf|png <file> - export the publication figure; viewsvg|viewpdf|viewpng <file> - export the current view exactly as shown\n"
      "chartspdf <file> - all charts of the page and tab currently shown, one per page; flowsvg <file> - the records-flow diagram\n"
      "shot <file.png> - screenshot of the window; viewshot <file.png> - screenshot of the canvas\n\n"
      "OPENALEX (the search_openalex tool is simpler)\n"
      "oaquery <terms> (alternatives separated by ' | '); oamax <n>; oafrom <year>; oakind 0|1 (0 works, 1 ...); oasem 0|1 - semantic search; oasemq <q1|q2>; oafetch - run the search and load the works\n\n"
      "TERM CLEANING (Data > Clean terms)\n"
      "cleanunit <i> - unit index; cleanscan - find spelling variants by rules; cleanai - propose merges with the assistant's model; cleanpick <i> [0|1] - tick a group; cleanlabel <i> <label> - the merged label; cleanmerge - apply the ticked groups to the thesaurus; cleanundo\n\n"
      "READ\n"
      "getpdf [all|R3,R7] [retry] - download the open-access PDFs of the records with a DOI and no file (the get_pdfs tool reports the outcome)\n"
      "rdview one|two|cover|turn|turnback|upright|print - the reader's view: one page or two facing pages (cover = first page alone), turn the view a quarter, print the open PDF\n\n"
      "LIVING MAPS (a saved OpenAlex search)\n"
      "livingcheck - look for new works; livingadd [rebuild] - add them; livingauto 0|1\n\n"
      "OTHER\n"
      "wait <frames> - pause a few frames (60 = about a second) before the next command, e.g. before chartspdf after switching a tab\n";
}

string App::agentExportPath(const string& given, const string& ext) const {
  string dir = exportsDir();
  string pth = trim(given);
  auto hasDir = [](const string& x) { return x.find('\\') != string::npos || x.find('/') != string::npos; };
  if (pth.empty()) {
    string base = exportName(ext);
    if (ext == "vosproj") base = (P && !P->path.empty() ? fileName(P->path).substr(0, fileName(P->path).rfind('.')) : string("vosstudio-project")) + ".vosproj";
    pth = dir + "\\" + base;
  } else if (!hasDir(pth)) pth = dir + "\\" + pth;
  string e = fileExt(pth);
  if (e != ext && !(ext == "png" && (e == "jpg" || e == "jpeg"))) pth += "." + ext;
  return pth;
}

void App::agentRunCommands(const string& tool, const vector<string>& cmds) {
  agent.cmds.assign(cmds.begin(), cmds.end());
  agent.cmdNotes.clear();
  agent.cmdJobSeq = -1;
  agent.cmdCount = 0;
  agent.cmdT0 = nowSeconds();
  agent.cmdWaitUntil = 0;
  agent.jobTool = tool;
  agent.waitCmds = true;
  if (agent.turn >= 0 && agent.turn < int(aiTurns.size())) aiTurns[size_t(agent.turn)].text = agentRender();
  needFrame = true;
}

// One command per frame, each after the jobs and animations of the previous one (like the script runner). Finishes the
// tool with what every command did and the state of the interface.
void App::agentPumpCommands() {
  if (!agent.waitCmds) return;
  needFrame = true;
  auto finish = [&]() {
    agent.waitCmds = false;
    string tool = agent.jobTool;
    string res;
    int failed = 0;
    for (auto& n : agent.cmdNotes) { res += n + "\n"; if (n.find("?? ") == 0) failed++; }
    res += "\nInterface now:\n" + uiStateSummary();
    if (!agent.log.empty() && agent.log.back().detail.empty()) agent.log.back().detail = plural(agent.cmdCount, "command");
    agentFinishTool(tool, failed < agent.cmdCount || agent.cmdCount == 0, res);
  };
  if (busy() || (job && job->finished) || animT0 >= 0 || shotPending() || nowSeconds() < agent.cmdWaitUntil) {
    if (nowSeconds() - agent.cmdT0 > 900) { agent.cmdNotes.push_back("?? The task is taking too long; the assistant stopped waiting (it keeps running)."); agent.cmds.clear(); finish(); }
    return;
  }
  if (agent.cmdJobSeq >= 0) {  // the previous command's job has ended
    if (!lastJobError.empty() && !agent.cmdNotes.empty()) agent.cmdNotes.back() = "?? " + agent.cmdNotes.back() + " failed: " + lastJobError;
    else if (!agent.cmdNotes.empty()) {
      string& n = agent.cmdNotes.back();
      if (n.find("build") != string::npos || n.find("Building") != string::npos) {
        const auto& R = P->last;
        n += " done: " + plural(R.n, P->net.unitNoun) + ", " + plural(R.m, "link") + ", " + plural(R.clusters, "cluster") + ", Q " + fmtFixed(R.Q, 3) + ".";
        if (R.n == 0) n += " The map is empty: lower the minimum or pick another unit.";
      } else if (n.find("Importing") != string::npos || n.find("OpenAlex") != string::npos) n += " done: " + plural(long(P->corpus.recs.size()), "record") + " loaded.";
      else n += " done.";
    }
    agent.cmdJobSeq = -1;
  }
  if (agent.cmds.empty()) { finish(); return; }
  string line = agent.cmds.front();
  agent.cmds.pop_front();
  agent.cmdCount++;
  string name = split(line, ' ')[0];
  if (name == "wait") {  // frames, as in scripts (60 = about a second); the pause happens before the next command
    int fr = clampv(toInt(trim(line.substr(4)), 10), 1, 600);
    agent.cmdWaitUntil = nowSeconds() + fr / 60.0;
    agent.cmdNotes.push_back(line + " -> paused");
    return;
  }
  if (!P) { agent.cmdNotes.push_back("?? " + line + " -> no project is open"); return; }
  uint64_t before = jobSeq;
  string note;
  bool known = runCommand(line, &note);
  if (!known) { agent.cmdNotes.push_back("?? " + line + " -> unknown command"); return; }
  agent.cmdNotes.push_back(line + " -> " + (note.empty() ? string("ok") : note));
  if (busy()) agent.cmdJobSeq = int(before);  // a job started: report its end (or error) before the next command
}

// The interface as the user sees it: for get_ui_state and after run_commands
string App::uiStateSummary() {
  string s;
  if (!P) return "No project is open.";
  s += "- Project: " + (P->path.empty() ? string("unsaved") : P->path) + (P->dirty ? " (unsaved changes)" : "") + "\n";
  if (hasCorpus()) {
    int y0 = 0, y1 = 0;
    for (auto& r : P->corpus.recs) if (r.year) { y0 = y0 ? std::min(y0, r.year) : r.year; y1 = std::max(y1, r.year); }
    s += "- Records: " + plural(long(P->corpus.recs.size()), "record");
    if (y0 > 0) s += ", " + std::to_string(y0) + "\xE2\x80\x93" + std::to_string(y1);
    s += "\n";
  } else s += "- Records: none loaded\n";
  s += "- Analysis settings: " + string(typeInfo(P->spec.type).label) + " of " + lower(unitLabel(P->spec.type, P->spec.unit)) + ", minimum " + std::to_string(P->spec.min) + "\n";
  if (hasMap()) {
    s += "- Map: " + plural(P->net.n(), P->net.unitNoun) + ", " + plural(P->net.m(), "link") + ", " + plural(P->net.nClusters, "cluster") + (P->net.description.empty() ? "" : " (" + P->net.description + ")");
    if (P->mapSource != "analysis") s += ", imported";
    else if (P->builtSig != P->specSig()) s += "; the settings changed since the build";
    s += "\n";
    if (mapHistory.size() > 1) s += "- Map history: " + plural(long(mapHistory.size()), "map") + ", showing " + std::to_string(mapCur) + "\n";
  } else s += "- Map: none yet\n";
  s += "- View: " + viewName(view);
  if (view == ViewKind::Geo) s += string(" (layer ") + (geoLayerKind == 0 ? "network" : geoLayerKind == 1 ? "overlay" : "density") + ")";
  if (mainChartOpen) s += "; the main area shows the chart \"" + mainChart.title + "\"";
  if (figZoomOpen) s += "; the figure preview is open full-screen";
  s += "\n";
  string tab;
  if (page == PG_ANALYSE) { static const char* t[] = {"Clusters", "Items", "Network", "Stability"}; tab = anaTab >= 0 && anaTab < 4 ? t[anaTab] : ""; }
  else if (page == PG_TRENDS) { static const char* t[] = {"Growth", "Bursts", "Themes", "Compare", "Three-field", "Topics", "RPYS", "Main path"}; tab = trTab >= 0 && trTab < 8 ? t[trTab] : ""; }
  else if (page == PG_ACTORS) { static const char* t[] = {"Authors", "Sources", "Countries", "Organisations", "Documents", "Laws"}; tab = acTab >= 0 && acTab < 6 ? t[acTab] : ""; }
  s += "- Left panel: " + (page >= 0 && page < PG_COUNT ? string(pageTitle(page)) + (tab.empty() ? "" : " \xE2\x80\xBA " + tab) : string("closed")) + "; inspector " + (inspectorOpen ? "open" : "closed") + "\n";
  s += string("- Look: ") + (ui.dark ? "dark" : "light") + " theme, " + (P->style.hulls ? "hulls on" : "hulls off") + ", " + (P->style.clusterNames ? "cluster names on" : "cluster names off") + ", " +
       (showLegend ? "legend on" : "legend off") + ", " + (showLabels ? "labels on" : "labels off") + ", " +
       (P->style.linkGeom == LinkGeom::Straight ? "straight" : P->style.linkGeom == LinkGeom::Arc ? "arc" : "curved") + " links (max " + std::to_string(P->style.maxLines) + ")\n";
  if (!search.empty()) s += "- Search: \"" + search + "\" (" + plural(long(searchHits.size()), "hit") + ")\n";
  if (hasMap() && focusNode >= 0 && size_t(focusNode) < P->net.nodes.size()) s += "- Focused item: " + P->net.nodes[size_t(focusNode)].label + "\n";
  if (!selection.empty()) s += "- Selected items: " + std::to_string(selection.size()) + "\n";
  if (clusterFilter >= 0) s += "- Highlighted cluster: " + std::to_string(clusterFilter + 1) + "\n";
  if (scopeActive()) s += "- Linked selection: " + scopeLabel_ + " scopes the Trends/Actors pages, the papers table and the geo view to " + plural(long(scopeRecs_.size()), "record") + " of " + fmtInt(long(P->corpus.recs.size())) + "\n";
  else if (!linkScope) s += "- Linked selection is off (pages always show all records)\n";
  if (cmpSideOpen) s += "- Compare mode: side-by-side figure in the main area (" + compareSummary() + ")\n";
  if (diffRestore.active) s += "- Difference map colours are on the map\n";
  if (docPreview >= 0 && hasCorpus() && size_t(docPreview) < P->corpus.recs.size()) s += "- Document preview open: R" + std::to_string(docPreview + 1) + " " + truncate(P->corpus.recs[size_t(docPreview)].title, 80) + "\n";
  if (writerOpen) s += "- Main area: the writer (document editor); the map is hidden behind it\n";
  else if (papersOpen && hasCorpus()) s += "- Main area: the papers table (" + plural(long(papersRows.size()), "row") + (papersFilter.empty() ? string() : ", filter \"" + papersFilter + "\"") + "); the map is hidden behind it\n";
  else if (mainChartOpen && mainChart.valid()) s += "- Main area: the chart \"" + mainChart.title + "\" (the map is hidden behind it; show_chart none brings the map back)\n";
  else if (splitActive()) {
    s += "- Main area: split view with " + std::to_string(splitPanes()) + " panes: ";
    for (int i = 0; i < splitPanes(); i++) s += (i ? "; " : "") + std::to_string(i + 1) + " = " + (splitChart[i] == "map" ? "the live " + viewName(view) + " view of the map" : splitChart[i]);
    s += " (split off returns to the map alone; pane <n> <id> changes one)\n";
  }
  else if (page != PG_AI && hasMap()) s += "- Main area: the " + viewName(view) + " view of the map\n";
  if (showStart) s += "- The start screen is shown (start 0 hides it)\n";
  if (busy()) s += "- Running: " + jobLabel + "\n";
  if (!wdoc.empty()) s += "- Document (writer): " + writerSummary() + (writerOpen ? " - open in the main area" : " - not open (writer on shows it)") + "\n";
  if (live.open) s += string("- Live AI: ") + (live.talk ? "voice session, microphone on" : "text session") + "\n";
  return s;
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

// ------------------------------------------------------------------ reports (1.9.2)
string App::agentReportText(bool full) const {
  if (wdoc.empty()) return "The document is empty.";
  string s = docOutlineText(wdoc);
  vector<DocSection> secs = docSections(wdoc);
  int si = 0, total = wdoc.words();
  string body;
  for (auto& sec : secs) {
    if (sec.heading < 0) continue;
    string md = docSectionMarkdown(wdoc, sec);
    int words = 0, cites = 0;
    for (auto& w : splitAny(md, " \n\t")) words += !w.empty();
    for (size_t p = md.find('['); p != string::npos; p = md.find('[', p + 1)) { size_t e = md.find(']', p); if (e != string::npos && e > p + 1 && isdigit(uint8_t(md[p + 1]))) cites++; }
    body += "\nSection " + std::to_string(++si) + ": " + sec.title + " (" + plural(words, "word") + (cites ? ", " + plural(cites, "citation") : string()) + ")\n";
    if (full) body += md + "\n";
    else { string first = trim(md.substr(0, md.find('\n'))); if (!first.empty()) body += "  " + truncate(first, 220) + "\n"; }
  }
  s += body;
  s += "\n" + plural(si, "section") + ", " + plural(wdoc.figures(), "figure") + ", " + plural(wdoc.tables(), "table") + ", " + plural(total, "word") + " in total.";
  if (!full && si) s += " (get_report with full = true returns the complete text.)";
  if (!writerOpen) s += " The user can open the document in the writer (writer on).";
  return s;
}

void App::agentWriteStart(const Json& g, const string& tool) {
  ai::Config cfg = aiConfig();
  cfg.maxTokens = std::max(cfg.maxTokens, 8192);
  vector<string> titles;
  const Json& sec = g["sections"];
  if (sec.t == Json::Arr) for (size_t i = 0; i < sec.size(); i++) { string t = trim(sec[i].str()); if (!t.empty()) titles.push_back(truncate(t, 120)); }
  else if (sec.t == Json::Str) for (auto& t : splitAny(sec.str(), ";\n")) if (!trim(t).empty()) titles.push_back(truncate(trim(t), 120));
  if (titles.empty()) titles = {"Introduction", "Research themes", "Trends and emerging topics", "Research gaps and future directions", "Conclusion"};
  if (titles.size() > 12) titles.resize(12);
  int words = clampv(g["words_per_section"].integer(400), 150, 900);
  string instr = trim(g["instructions"].str()), title = trim(g["title"].str());
  // what the writer gets to work with: the corpus, the map, the trends, the most cited and the most recent papers with their ids
  ai::ContextOpts o;
  o.trends = true;
  o.maxItemsPerCluster = 15;
  string ctx = ai::buildContext(*P, o);
  Json q1 = Json::object(); q1.set("sort", "cited"); q1.set("n", 30);
  Json q2 = Json::object(); q2.set("sort", "recent"); q2.set("n", 20);
  string papers = "Most cited papers (cite them as [R<id>]):\n" + agentReadPapers(q1) + "\n\nMost recent papers:\n" + agentReadPapers(q2);
  if (papers.size() > 24000) papers.resize(24000);
  if (ctx.size() > 16000) ctx.resize(16000);
  string existing = wdoc.empty() ? string() : "The document so far (do not repeat it; the new sections continue it):\n" + agentReportText(false) + "\n\n";
  string sys =
      "You are a scientific writer inside VOSStudio, a bibliometric mapping application. You write literature-review sections in an academic register, "
      "specific and evidence-based: numbers, periods, themes and papers come only from the material given. Cite papers with their ids in square brackets, "
      "[R12] or [R12, R4], and never invent papers, authors or figures. Refer to the report's figures as \"Figure n\" when they exist. "
      "Light Markdown only: paragraphs, - bullets, **bold**, ### subheadings.\n"
      "Reply with exactly one JSON object and nothing else: {\"title\": \"report title\", \"sections\": [{\"title\": \"...\", \"text\": \"...\"}, ...]}. "
      "Write every requested section at the requested length (a section of " + std::to_string(words) + " words has " + std::to_string(std::max(2, words / 120)) + " or more full paragraphs). Escape newlines inside text as \\n.";
  string user = "Write these sections, about " + std::to_string(words) + " words each, in this order:\n";
  for (size_t i = 0; i < titles.size(); i++) user += std::to_string(i + 1) + ". " + titles[i] + "\n";
  if (!title.empty()) user += "\nReport title: " + title + "\n";
  if (!instr.empty()) user += "\nInstructions from the user: " + instr + "\n";
  user += "\n" + existing + "=== Material ===\n" + ctx + "\n\n" + papers;
  vector<ai::Msg> msgs = {{"user", user}};
  agent.writeReq = aiRequest(ai::buildChat(cfg, sys, msgs, true));
  agent.writeArgs = g;
  agent.waitWrite = true;
  agent.jobTool = tool;
  if (!agent.log.empty()) agent.log.back().detail = plural(long(titles.size()), "section") + " \xC2\xB7 " + cfg.effectiveModel();
  if (agent.live) live.log.toolStatus(live.toolId, 0, "Writing " + plural(long(titles.size()), "section") + " with " + cfg.effectiveModel() + "\xE2\x80\xA6");
}

void App::agentWriteFinish(const string& text, const string& err) {
  const string tool = agent.jobTool;
  if (!err.empty()) { agentFinishTool(tool, false, "The text model returned an error: " + err); return; }
  string js = ai::extractJsonBlock(text, '{', '}');
  Json j = Json::parse(js);
  vector<std::pair<string, string>> secs;
  string title;
  if (j.t == Json::Obj && j["sections"].t == Json::Arr) {
    for (size_t i = 0; i < j["sections"].size(); i++) {
      const Json& sj = j["sections"][i];
      string st = trim(sj["title"].str()), tx = sj["text"].t == Json::Str ? sj["text"].str() : string();
      if (!trim(tx).empty()) secs.push_back({st, tx});
    }
    title = truncate(trim(j["title"].str()), 160);
  }
  if (secs.empty()) {  // the model answered in prose: keep it as one section rather than losing it
    string body = trim(text);
    if (body.empty()) { agentFinishTool(tool, false, "The text model returned nothing usable."); return; }
    const Json& sec = agent.writeArgs["sections"];
    secs.push_back({sec.t == Json::Arr && sec.size() ? sec[size_t(0)].str() : string("Report"), body});
  }
  if (trim(agent.writeArgs["title"].str()).size()) title = truncate(trim(agent.writeArgs["title"].str()), 160);
  wed.begin("Assistant: write report");
  if (agent.writeArgs["replace"].boolean(false)) {  // drop the existing sections (title block, figures before the first section and the references stay)
    vector<DocSection> old = docSections(wdoc);
    for (int i = int(old.size()) - 1; i >= 0; i--) if (old[size_t(i)].heading >= 0) docReplaceBlocks(wdoc, old[size_t(i)].first, old[size_t(i)].last, {});
  }
  writerEnsureTitle(title, "");
  int words = 0, cites = 0, bad = 0;
  vector<string> titles;
  for (auto& sc : secs) {
    int w = 0, c = 0, b = 0;
    docAppendSection(truncate(sc.first, 140), replaceAll(sc.second, "\r", ""), &w, &c, &b);
    words += w; cites += c; bad += b;
    titles.push_back(sc.first.empty() ? string("(untitled)") : sc.first);
  }
  wed.externalChange();
  string res = "Wrote " + plural(long(secs.size()), "section") + " into the document (" + plural(words, "word") + ", " + plural(cites, "citation") + "): " + join(titles, "; ") + ". " + agentReportText(false);
  if (bad) res += " " + plural(bad, "citation") + " did not match a paper id and were left out.";
  if (!agent.log.empty()) agent.log.back().detail = plural(long(secs.size()), "section") + ", " + plural(words, "word");
  agentFinishTool(tool, true, res);
}

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
string App::agentChart(const string& id0, const Json& g, ReportBlock& out, double W, double H, const ChartTheme* theme) {
  string id = normName(id0);
  if (id.empty()) return "Name a chart.";
  out = ReportBlock();
  out.kind = ReportBlock::Figure;
  ChartTheme th = theme ? *theme : chartTheme(true);
  if (!theme) th.transparent = false;
  if (W <= 0) W = 500;
  auto hh = [&](double natural) { return H > 0 ? H : natural; };  // charts that stretch to any height take the caller's
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
      out.fig = chartStrategic(cinfo, clusterColors(), W, hh(380), th);
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
      out.fig = chartSweep(sweep, P->params.cluster.resolution, W, hh(250), th);
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
      } else if (id == "map_3d") { sp.panel3D = true; what = "3D map"; cap = "The layout in three dimensions, seen from the angle of the 3D view"; }
      else if (id == "map_matrix") { sp.panelMatrix = true; what = "Matrix"; cap = "Link strengths between the items, ordered by cluster"; }
      else return "Unknown map figure. Use map_network, map_overlay, map_density, map_timeline, map_geo, map_3d or map_matrix.";
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
      out.fig = chartGrowth(gr, W, hh(250), th);
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
      out.fig = chartTrendTopics(v, W, hh(std::min(640.0, std::max(160.0, double(v.size()) * 17 + 44))), th);
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
      out.fig = chartSankey(e, W, hh(320), th);
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
      out.fig = chartThreeField(tf, "Authors", corpusUnitName(mid) ? corpusUnitName(mid) : "Keywords", "Sources", W, hh(380), th);
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
      out.fig = chartBradford(b, W, hh(240), th);
      out.title = "Bradford's law: concentration of sources";
      out.text = "Core zone sources publish a third of the documents";
      summary = std::to_string(b.zone1) + " core sources: ";
      for (size_t k = 0; k < std::min<size_t>(size_t(b.zone1), std::min<size_t>(10, b.sources.size())); k++) summary += (k ? "; " : "") + b.sources[k].first;
    } else if (id == "lotka" || id == "lotkas_law") {
      const Lotka& lk = statLotka();
      out.fig = chartLotka(lk, W, hh(240), th);
      out.title = "Lotka's law: author productivity";
      out.text = "Share of authors by number of documents";
      summary = "Fitted exponent " + fmtNum(lk.exponent, 2) + " (classic value 2), R2 " + fmtNum(lk.r2, 2) + ".";
    } else if (id == "rpys") {
      bool anyRefs = false;
      for (auto& r : C.recs) if (!r.refs.empty()) { anyRefs = true; break; }
      if (!anyRefs) return "RPYS needs cited references in the records.";
      Rpys r = rpys(C, 0, 0);
      out.fig = chartRpys(r, W, hh(240), th);
      out.title = "Reference publication year spectroscopy";
      out.text = "Cited references by publication year; peaks mark the historical roots of the field";
      summary = "Peak years: ";
      for (size_t k = 0; k < r.peaks.size(); k++) summary += (k ? "; " : "") + std::to_string(r.peaks[k].year) + " (" + truncate(r.peaks[k].topRef, 80) + ")";
    } else if (id == "main_path") {
      const MainPath& M = mainPathNow();
      if (!M.ok) return M.error.empty() ? string("No main path was found (needs citations between the documents).") : M.error;
      out.fig = chartMainPath(C, M, W, hh(330), th);
      out.title = "Main path of the citation network";
      out.text = "Documents by publication year; the blue line is the global main path by search path count, orange documents lie on key routes";
      summary = "Global main path, oldest first: ";
      for (size_t k = 0; k < M.global.size(); k++) summary += (k ? "; " : "") + string("[R") + std::to_string(M.global[k] + 1) + "] " + shortCite(C.recs[size_t(M.global[k])]);
      summary += ". " + plural(M.dagNodes, "document") + " and " + plural(M.dagLinks, "citation") + " in the citation network.";
    } else if (id == "records_flow") {
      out.fig = chartFlow(recordsFlow(), H > 0 ? W : 520, hh(560), th);
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

// show_chart: the same charts as add_chart, drawn live into the main area at the size of the canvas
// A chart of the loaded data by id, as a ChartDef the main area and the split-view panes draw at any size.
bool App::makeChartDef(const string& id0, const Json& g, ChartDef& def, string& err, string* summary) {
  string id = normName(id0);
  // a probe at report size finds errors (no records, missing fields) before anything is shown
  ReportBlock probe;
  err = agentChart(id, g, probe);
  if (!err.empty()) return false;
  Json args = g;
  def = ChartDef();
  def.title = probe.title.empty() ? replaceAll(id, "_", " ") : probe.title;
  def.make = [this, id, args](double w, double h, const ChartTheme& t) {
    ReportBlock b;
    const double W0 = 720;  // designed width; the card scales the scene
    agentChart(id, args, b, W0, W0 * h / std::max(1.0, w), &t);
    return b.fig;
  };
  if (summary) *summary = probe.text + (probe.figId.empty() ? string() : ". " + probe.figId);
  return true;
}

string App::agentShowChart(const string& id0, const Json& g) {
  string id = normName(id0);
  static const char* views[][2] = {{"map_network", "network"}, {"map_overlay", "overlay"}, {"map_density", "density"}, {"map_timeline", "timeline"},
                                   {"map_geo", "geo"}, {"map_3d", "3d"}, {"map_matrix", "matrix"}};
  for (auto& v : views) {
    if (id != v[0]) continue;
    if (!hasMap()) return "There is no map yet. Build one with build_map first.";
    ViewKind vk;
    if (!parseView(v[1], vk)) return "Unknown view.";
    if (vk == ViewKind::Geo && !geoAvailable()) return "The Geo view needs a map of countries or organisations with locations.";
    setView(vk, true);
    mainChartOpen = false;
    papersOpen = false;
    mainChart.title = viewName(vk) + " view";
    mainChartSummary = "the live " + viewName(vk) + " view of the map";
    return "";
  }
  ChartDef def;
  string err, summary;
  if (!makeChartDef(id, g, def, err, &summary)) return err;
  mainChart = def;
  mainChartOpen = true;
  writerOpen = false;  // the chart takes the main area; the document stays
  mainChartSummary = summary;
  papersOpen = false;
  figZoomOpen = false;
  if (page == PG_AI) page = PG_TRENDS;
  chartCache.erase("#main|" + mainChart.title);
  needFrame = true;
  return "";
}

// Word export of a report: figures are rendered with the GPU at print resolution and embedded as PNG.
// ------------------------------------------------------------------ document helpers shared by the tools and the writer
// "[R12, R4]" citations -> citation fields ("[@key;@key]") with the entries added to the document's bibliography
// (1.12: real entries, formatted in the document's citation style; Word gets them as native citations); counts what matched
string App::docCitedMarkdown(const string& markdown, int* cites, int* bad) {
  int nC = 0, nB = 0;
  string out = docResolveCitationKeys(markdown, [&](int rid) -> string {
    nC++;
    if (rid < 1 || !hasCorpus() || rid > int(P->corpus.recs.size())) { nB++; return string(); }
    const Record& r = P->corpus.recs[size_t(rid) - 1];
    RefEntry e = refFromRecord(r);
    e.rec = rid - 1;
    docAddReference(wdoc, e);
    return e.key;
  });
  if (cites) *cites = nC;
  if (bad) *bad = nB;
  return out;
}

int App::docAppendSection(const string& title, const string& markdown, int* words, int* cites, int* bad) {
  wed.begin("Assistant: add section");
  if (words) { *words = 0; for (auto& w : splitAny(markdown, " \n\t")) *words += !w.empty(); }
  if (wdoc.blocks.size() == 1 && wdoc.blocks[0].kind == Block::Paragraph && wdoc.blocks[0].p.empty()) wdoc.blocks.clear();  // a fresh document
  string body = docCitedMarkdown(markdown, cites, bad);
  vector<Block> blocks = blocksFromMarkdown(body, 2, title);
  string t = trim(title);
  if (t.empty()) {  // no title: the blocks continue the last section
    int at = docBodyEnd(wdoc);
    wdoc.blocks.insert(wdoc.blocks.begin() + at, blocks.begin(), blocks.end());
  } else docInsertSection(wdoc, t, blocks);
  wed.externalChange();
  int n = 0;
  for (auto& sc : docSections(wdoc)) n += sc.heading >= 0;
  return n;
}

int App::docAppendFigure(int asset, const string& caption) {
  wed.begin("Assistant: add figure");
  Block f;
  f.kind = Block::FigureBlock;
  f.fig.asset = asset;
  f.fig.widthPct = 100;
  f.fig.caption.style = PStyle::Caption;
  f.fig.caption.align = PAlign::Center;
  if (!trim(caption).empty()) f.fig.caption.spans = spansFromMarkdown(trim(caption));
  if (wdoc.blocks.size() == 1 && wdoc.blocks[0].kind == Block::Paragraph && wdoc.blocks[0].p.empty()) wdoc.blocks.clear();  // a fresh document
  int at = docBodyEnd(wdoc);
  wdoc.blocks.insert(wdoc.blocks.begin() + at, f);
  wed.externalChange();
  return at;
}

void App::cmdSaveReport(const string& fmt) {
  if (wdoc.empty()) { ui.toast("Document", "The document is empty. Ask the AI to write a report, or open the writer and write one.", 2, 4); return; }
  writerExport(fmt);
}

bool App::agentExportReport(const Json& g, string& msg) {
  agentReportTitle = trim(g["title"].str());
  if (wdoc.empty()) { msg = "The document is empty."; return false; }
  wed.begin("Assistant: export");
  writerEnsureTitle(agentReportTitle, g["subtitle"].str());
  string format = lower(trim(g["format"].str()));
  if (format.empty()) format = "pdf";
  bool wantPdf = format == "pdf" || format == "both" || format == "all", wantDocx = format == "docx" || format == "word" || format == "both" || format == "all",
       wantHtml = format == "html" || format == "all";
  if (!wantPdf && !wantDocx && !wantHtml) { msg = "Unknown format \"" + format + "\" (pdf, docx, html, both or all)."; return false; }
  SYSTEMTIME st;
  GetLocalTime(&st);
  char stamp[32];
  snprintf(stamp, sizeof stamp, "%04d-%02d-%02d-%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
  string base = reportsDir() + "\\" + writerSlug() + "-" + stamp;
  vector<string> written;
  string pdfPath, docxPath, htmlPath;
  if (wantPdf) { pdfPath = writerExport("pdf", base + ".pdf"); if (pdfPath.empty()) { msg = "The PDF could not be written to " + base + ".pdf."; return false; } written.push_back(pdfPath); }
  if (wantDocx) { docxPath = writerExport("docx", base + ".docx"); if (docxPath.empty()) { msg = "The Word file could not be written to " + base + ".docx."; return false; } written.push_back(docxPath); }
  if (wantHtml) { htmlPath = writerExport("html", base + ".html"); if (htmlPath.empty()) { msg = "The web page could not be written to " + base + ".html."; return false; } written.push_back(htmlPath); }
  agentReportPath = !pdfPath.empty() ? pdfPath : !docxPath.empty() ? docxPath : htmlPath;
  agentReportDocx = docxPath;
  agentReportTurn = agent.turn;
  agentReportExported = true;
  int pages = wantPdf ? int(docToPages(wdoc).size()) : 0;
  int refs = 0;
  for (auto& b : wdoc.blocks) refs += b.kind == Block::Paragraph && b.p.style == PStyle::Reference;
  int secs = 0;
  for (auto& sc : docSections(wdoc)) secs += sc.heading >= 0;
  msg = "Saved the document as " + join(written, " and ") + ": " + (pages ? plural(pages, "page") + ", " : string()) + plural(secs, "section") + ", " + plural(wdoc.figures(), "figure") + ", " +
        plural(wdoc.tables(), "table") + ", " + plural(refs, "reference") + (wantDocx ? ". The Word file has real headings, tables, embedded figures and references and can be edited" : "") +
        ". The user can open it from the conversation, and keep editing the document in the writer (writer on).";
  if (!agent.log.empty()) agent.log.back().detail = (pages ? plural(pages, "page") + ", " : string()) + plural(refs, "reference");
  return true;
}

}  // namespace win
}  // namespace vs
