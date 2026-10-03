// AI assistant: provider-neutral chat requests, streaming parsers, research context and task prompts.
// Transport-free (the Windows layer sends the HttpReq), so everything here is testable on any host.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "json.h"

namespace vs {
class Project;
struct Network;

namespace ai {
using std::string;
using std::vector;

enum class Provider { OpenAI = 0, Gemini = 1, Anthropic = 2, Compatible = 3 };
constexpr int kProviders = 4;

struct Config {
  Provider provider = Provider::OpenAI;
  string key, model, baseUrl;  // baseUrl: Compatible only (e.g. https://openrouter.ai/api/v1, http://localhost:11434/v1)
  int maxTokens = 4096;
  bool ready() const { return !effectiveModel().empty() && (!key.empty() || provider == Provider::Compatible); }
  string effectiveModel() const;
};

const char* providerName(Provider p);
const char* providerHint(Provider p);  // where to get a key
string defaultModel(Provider p);
vector<string> suggestedModels(Provider p);

struct Msg { string role, text; };  // role: "user" or "assistant"

struct HttpReq {
  string method = "POST", url;
  vector<std::pair<string, string>> headers;
  string body;
};

HttpReq buildChat(const Config& c, const string& system, const vector<Msg>& msgs, bool stream);
HttpReq buildModelList(const Config& c);
vector<string> parseModelList(const Config& c, const string& body);
string parseChat(const Config& c, const string& body, string* err);  // non-streamed reply
string describeError(const Config& c, int status, const string& body, const string& transportErr);

// Incremental Server-Sent-Events parser for all providers. feed() returns false once the stream reports an error.
class StreamParser {
 public:
  explicit StreamParser(Provider p) : p_(p) {}
  bool feed(const char* data, size_t n);
  void finish();
  string text;       // accumulated reply
  string error;      // provider error reported inside the stream
  bool done = false;
  string stopReason;
 private:
  void line(const string& l);
  Provider p_;
  string buf_, event_, raw_;
};

// ---------------------------------------------------------------- research context
struct ContextOpts {
  bool corpus = true, map = true, topDocs = true, trends = true;
  int selectedItem = -1, selectedCluster = -1, document = -1;
  int maxItemsPerCluster = 12;
};
string corpusContext(const Project& P);
string mapContext(const Project& P, int maxItemsPerCluster);
string topDocsContext(const Project& P, int n);
string trendContext(const Project& P);
string itemContext(const Project& P, int item);
string clusterContext(const Project& P, int cluster);
string documentContext(const Project& P, int rec);
string buildContext(const Project& P, const ContextOpts& o);

// ---------------------------------------------------------------- tasks
enum class Task {
  Chat = 0, Overview, Review, Gaps, Emerging, NameClusters, Explain, Document, Search, Caption, Methods, Clean, Agent, Count
};
struct TaskInfo { Task task; const char* id; const char* title; const char* sub; const char* icon; };
const vector<TaskInfo>& tasks();
const TaskInfo& taskInfo(Task t);
string systemPrompt();
string taskPrompt(Task t, const string& extra);  // the user turn for a one-click task

// Structured replies
struct ClusterName { int cluster = -1; string name, summary; };
vector<ClusterName> parseClusterNames(const string& reply);  // JSON array, tolerant of code fences / prose
struct SearchPlan { string keywords; vector<string> semantic; int fromYear = 0, toYear = 0; };
bool parseSearchPlan(const string& reply, SearchPlan& out);

// Term cleaning (Data → Clean terms → AI): the model proposes merges and generic terms from the term list
struct CleanProposal {
  string target;            // preferred label (merge) or the term itself (ignore)
  vector<string> members;   // labels merged into target
  string kind, reason;      // "synonym", "abbreviation", "variant", "generic"
  bool ignore = false;      // remove the term from maps
  double confidence = 0;    // 0..1 as given by the model (0 = not given)
};
string cleanTermList(const vector<std::pair<string, int>>& terms, size_t maxTerms);
bool parseCleanPlan(const string& reply, vector<CleanProposal>& out);

// ---------------------------------------------------------------- agent
// The agent works in steps: every model reply is one JSON action (a tool call or the final answer); the app runs
// the tool and sends back the result. Provider-neutral: plain JSON in the text, no vendor tool-calling API.
enum class ToolKind { Read = 0, View = 1, Change = 2 };  // Change needs the user's approval
struct ToolSpec { const char* name; const char* args; const char* desc; ToolKind kind; };
const vector<ToolSpec>& agentTools();
const ToolSpec* findTool(const string& name);
string agentSystemPrompt();
struct AgentAction { string thought, tool, final; Json args; };
bool parseAgentAction(const string& reply, AgentAction& out, string* err);
string extractJsonBlock(const string& reply, char open, char close);  // first balanced {...} / [...] in a model reply (code fences tolerated)
string agentGoalMessage(const string& goal, const string& context);
string agentResultMessage(const string& tool, bool ok, const string& result);
constexpr int kAgentMaxSteps = 30;

// Conversation stored in the project file
struct Turn { string role, text, task, when; };
Json turnsToJson(const vector<Turn>& t);
vector<Turn> turnsFromJson(const Json& j);

}  // namespace ai
}  // namespace vs
