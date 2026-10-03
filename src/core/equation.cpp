// VOSStudio Native — a bounded LaTeX-math reader used for Writer preview and Word's native OMML.
// This is deliberately a math-expression parser, not a TeX macro/program runner.
#include "equation.h"

#include <cctype>
#include <limits>

namespace vs {
namespace {

const char* kMathMlNs = "http://www.w3.org/1998/Math/MathML";

string xmlEsc(const string& s) {
  string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&apos;";
    else if (uint8_t(c) >= 0x20 || c == '\t' || c == '\n' || c == '\r') o += c;
  }
  return o;
}

struct Node {
  enum Kind { Row, Ident, Number, Operator, Fraction, Root, Script, Upright, Accent, Space } kind = Row;
  string text;
  vector<Node> children;
  bool hasSub = false, hasSup = false, hasDegree = false, noBar = false;
};

Node atom(Node::Kind kind, const string& text) {
  Node n;
  n.kind = kind;
  n.text = text;
  return n;
}

string symbolFor(const string& name) {
  static const std::map<string, string> symbols = {
      {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"varepsilon", "ϵ"}, {"zeta", "ζ"}, {"eta", "η"},
      {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"},
      {"pi", "π"}, {"varpi", "ϖ"}, {"rho", "ρ"}, {"varrho", "ϱ"}, {"sigma", "σ"}, {"varsigma", "ς"}, {"tau", "τ"}, {"upsilon", "υ"},
      {"phi", "ϕ"}, {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"}, {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"},
      {"Lambda", "Λ"}, {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Upsilon", "Υ"}, {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
      {"times", "×"}, {"cdot", "·"}, {"div", "÷"}, {"pm", "±"}, {"mp", "∓"}, {"le", "≤"}, {"leq", "≤"}, {"ge", "≥"}, {"geq", "≥"},
      {"ne", "≠"}, {"neq", "≠"}, {"approx", "≈"}, {"equiv", "≡"}, {"sim", "∼"}, {"simeq", "≃"}, {"propto", "∝"}, {"in", "∈"},
      {"notin", "∉"}, {"subset", "⊂"}, {"subseteq", "⊆"}, {"supset", "⊃"}, {"supseteq", "⊇"}, {"cup", "∪"}, {"cap", "∩"}, {"setminus", "∖"},
      {"forall", "∀"}, {"exists", "∃"}, {"nexists", "∄"}, {"partial", "∂"}, {"nabla", "∇"}, {"infty", "∞"}, {"emptyset", "∅"},
      {"to", "→"}, {"rightarrow", "→"}, {"leftarrow", "←"}, {"leftrightarrow", "↔"}, {"Rightarrow", "⇒"}, {"Leftarrow", "⇐"},
      {"Leftrightarrow", "⇔"}, {"mapsto", "↦"}, {"longrightarrow", "⟶"}, {"longleftarrow", "⟵"}, {"ldots", "…"}, {"cdots", "⋯"}, {"vdots", "⋮"},
      {"ddots", "⋱"}, {"sum", "∑"}, {"prod", "∏"}, {"int", "∫"}, {"iint", "∬"}, {"iiint", "∭"}, {"oint", "∮"}, {"sqrt", "√"},
      {"degree", "°"}, {"prime", "′"}, {"dagger", "†"}, {"ddagger", "‡"}, {"ell", "ℓ"}, {"Re", "ℜ"}, {"Im", "ℑ"}, {"aleph", "ℵ"},
      {"{", "{"}, {"}", "}"}, {"%", "%"}, {"$", "$"}, {"#", "#"}, {"&", "&"}, {"_", "_"}, {"{,}", ","}};
  auto it = symbols.find(name);
  return it == symbols.end() ? string() : it->second;
}

bool isFunctionName(const string& s) {
  static const std::set<string> names = {"sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh", "tanh", "log",
                                         "ln", "exp", "lim", "max", "min", "sup", "inf", "det", "dim", "ker", "Pr", "arg", "gcd", "deg", "hom", "sgn"};
  return names.count(s) != 0;
}

class Parser {
 public:
  explicit Parser(const string& source) : s_(source) {}

  Node parse() {
    Node root = sequence(0, 0);
    skipSpace();
    if (error_.empty() && at_ < s_.size()) error_ = "Unexpected closing delimiter in equation.";
    return root;
  }
  const string& error() const { return error_; }
  bool ok() const { return error_.empty(); }

 private:
  const string& s_;
  size_t at_ = 0;
  size_t nodes_ = 0;
  string error_;

  void fail(const string& message) { if (error_.empty()) error_ = message; }
  void skipSpace() { while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\r' || s_[at_] == '\n')) at_++; }
  bool newNode(Node& n) {
    if (++nodes_ > 12000) { fail("Equation is too complex to export (12,000 math tokens maximum)."); return false; }
    n = Node();
    return true;
  }

  Node sequence(char closing, int depth) {
    Node out;
    out.kind = Node::Row;
    if (depth > 128) { fail("Equation nesting exceeds 128 levels."); return out; }
    while (at_ < s_.size() && error_.empty()) {
      skipSpace();
      if (at_ >= s_.size()) break;
      char c = s_[at_];
      if (closing && c == closing) { at_++; return out; }
      if (c == '}') { fail("Unmatched closing brace in equation."); at_++; break; }
      if (c == '{') { at_++; out.children.push_back(sequence('}', depth + 1)); continue; }
      if (c == '^' || c == '_') {
        bool sup = c == '^';
        at_++;
        if (out.children.empty()) { fail("A script marker has no base expression."); break; }
        Node arg = argument(depth + 1);
        if (!error_.empty()) break;
        Node base = std::move(out.children.back());
        out.children.pop_back();
        if (base.kind != Node::Script) {
          Node script;
          if (!newNode(script)) break;
          script.kind = Node::Script;
          script.hasSub = script.hasSup = false;
          script.children.push_back(std::move(base));
          script.children.emplace_back();  // subscript slot
          script.children.emplace_back();  // superscript slot
          base = std::move(script);
        }
        bool& present = sup ? base.hasSup : base.hasSub;
        if (present) { fail("An equation has more than one superscript or subscript on the same base."); break; }
        present = true;
        base.children[sup ? 2 : 1] = std::move(arg);
        out.children.push_back(std::move(base));
        continue;
      }
      if (c == '\\') { at_++; out.children.push_back(command(depth + 1)); continue; }
      if (c == '&') { at_++; out.children.push_back(atom(Node::Operator, "&")); continue; }
      if (c == '~') { at_++; out.children.push_back(atom(Node::Space, "0.333em")); continue; }
      if (c == '$') { at_++; continue; }  // math delimiters are optional in the editor
      if (std::isdigit(uint8_t(c))) {
        size_t begin = at_++;
        while (at_ < s_.size() && (std::isdigit(uint8_t(s_[at_])) || s_[at_] == '.' || s_[at_] == ',')) at_++;
        out.children.push_back(atom(Node::Number, s_.substr(begin, at_ - begin)));
        continue;
      }
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        size_t begin = at_++;
        while (at_ < s_.size() && ((s_[at_] >= 'A' && s_[at_] <= 'Z') || (s_[at_] >= 'a' && s_[at_] <= 'z'))) at_++;
        string word = s_.substr(begin, at_ - begin);
        out.children.push_back(atom(isFunctionName(word) ? Node::Upright : Node::Ident, word));
        continue;
      }
      // one UTF-8 code point or punctuation operator
      size_t end = at_ + 1;
      unsigned char lead = uint8_t(s_[at_]);
      if ((lead & 0xE0) == 0xC0) end = std::min(s_.size(), at_ + 2);
      else if ((lead & 0xF0) == 0xE0) end = std::min(s_.size(), at_ + 3);
      else if ((lead & 0xF8) == 0xF0) end = std::min(s_.size(), at_ + 4);
      string token = s_.substr(at_, end - at_);
      at_ = end;
      out.children.push_back(atom(strchr("+-=*/<>|:,;.!()[]", char(lead)) ? Node::Operator : Node::Ident, token));
    }
    if (closing && error_.empty()) fail("Unclosed group in equation.");
    return out;
  }

  Node argument(int depth) {
    skipSpace();
    if (at_ >= s_.size()) { fail("A script or command is missing its argument."); return Node(); }
    if (s_[at_] == '{') { at_++; return sequence('}', depth); }
    if (s_[at_] == '\\') { at_++; return command(depth); }
    char c = s_[at_++];
    if (std::isdigit(uint8_t(c))) return atom(Node::Number, string(1, c));
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return atom(Node::Ident, string(1, c));
    return atom(Node::Operator, string(1, c));
  }

  string rawGroup() {
    skipSpace();
    if (at_ >= s_.size() || s_[at_] != '{') { fail("A text command needs a braced argument."); return string(); }
    at_++;
    string out;
    int depth = 1;
    while (at_ < s_.size() && depth > 0) {
      char c = s_[at_++];
      if (c == '\\' && at_ < s_.size()) { out += s_[at_++]; continue; }
      if (c == '{') { depth++; if (depth > 1) out += c; }
      else if (c == '}') { depth--; if (depth > 0) out += c; }
      else out += c;
    }
    if (depth != 0) fail("Unclosed text group in equation.");
    return collapseWs(trim(out));
  }

  string controlWord() {
    if (at_ >= s_.size()) return string();
    if ((s_[at_] >= 'A' && s_[at_] <= 'Z') || (s_[at_] >= 'a' && s_[at_] <= 'z')) {
      size_t begin = at_++;
      while (at_ < s_.size() && ((s_[at_] >= 'A' && s_[at_] <= 'Z') || (s_[at_] >= 'a' && s_[at_] <= 'z'))) at_++;
      return s_.substr(begin, at_ - begin);
    }
    return s_.substr(at_++, 1);
  }

  Node command(int depth) {
    if (depth > 128) { fail("Equation nesting exceeds 128 levels."); return Node(); }
    string name = controlWord();
    if (name.empty()) { fail("Invalid command at the end of the equation."); return Node(); }
    if (name == "left" || name == "right" || name == "middle" || name == "limits" || name == "nolimits" || name == "displaystyle" || name == "textstyle" ||
        name == "scriptstyle" || name == "scriptscriptstyle" || name == "quad" || name == "qquad") {
      if (name == "quad") return atom(Node::Space, "1em");
      if (name == "qquad") return atom(Node::Space, "2em");
      return atom(Node::Row, "");
    }
    if (name == "," || name == ":" || name == ";" || name == "!") return atom(Node::Space, name == "," ? "0.167em" : name == ";" ? "0.278em" : name == ":" ? "0.222em" : "-0.167em");
    if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "binom") {
      Node n;
      if (!newNode(n)) return n;
      n.kind = Node::Fraction;
      n.noBar = name == "binom";
      n.children.push_back(argument(depth + 1));
      n.children.push_back(argument(depth + 1));
      if (name == "binom") {
        Node wrapped;
        wrapped.kind = Node::Row;
        wrapped.children.push_back(atom(Node::Operator, "("));
        wrapped.children.push_back(std::move(n));
        wrapped.children.push_back(atom(Node::Operator, ")"));
        return wrapped;
      }
      return n;
    }
    if (name == "sqrt") {
      Node n;
      if (!newNode(n)) return n;
      n.kind = Node::Root;
      skipSpace();
      if (at_ < s_.size() && s_[at_] == '[') {
        at_++;
        Node degree = sequence(']', depth + 1);
        n.children.push_back(std::move(degree));
        n.hasDegree = true;
      }
      n.children.push_back(argument(depth + 1));
      return n;
    }
    if (name == "overline" || name == "bar" || name == "underline" || name == "hat" || name == "widehat" || name == "tilde" || name == "widetilde" ||
        name == "vec" || name == "dot" || name == "ddot") {
      Node n;
      if (!newNode(n)) return n;
      n.kind = Node::Accent;
      n.text = name == "overline" || name == "bar" ? "¯" : name == "underline" ? "_" : name == "hat" || name == "widehat" ? "ˆ" :
               name == "tilde" || name == "widetilde" ? "˜" : name == "vec" ? "→" : name == "dot" ? "˙" : "¨";
      n.children.push_back(argument(depth + 1));
      return n;
    }
    if (name == "mathrm" || name == "textrm" || name == "mathbf" || name == "mathit" || name == "mathbb" || name == "mathcal" || name == "textsf" || name == "texttt") {
      Node n;
      if (!newNode(n)) return n;
      n.kind = Node::Upright;
      n.text = name;
      n.children.push_back(argument(depth + 1));
      return n;
    }
    if (name == "text" || name == "operatorname") {
      string raw = rawGroup();
      return atom(Node::Upright, raw);
    }
    if (name == "operatorname*" ) return atom(Node::Upright, rawGroup());
    if (name == "\\") return atom(Node::Space, "0.5em");
    string symbol = symbolFor(name);
    if (!symbol.empty()) return atom(Node::Operator, symbol);
    if (isFunctionName(name)) return atom(Node::Upright, name);
    // Do not pretend an unsupported macro rendered as math. The Writer keeps the source visible and the UI warns
    // that Word will receive a text fallback; only Tectonic compiles the unrestricted LaTeX source.
    fail("Unsupported LaTeX command \\" + name + "; use PDF preview for full LaTeX rendering.");
    return atom(Node::Ident, "\\" + name);
  }
};

string omml(const Node& n) {
  switch (n.kind) {
    case Node::Row: {
      string o;
      for (const Node& c : n.children) o += omml(c);
      return o;
    }
    case Node::Ident: case Node::Number: case Node::Operator: {
      if (n.text.empty()) return string();
      return "<m:r><m:t xml:space=\"preserve\">" + xmlEsc(n.text) + "</m:t></m:r>";
    }
    case Node::Upright: {
      if (n.children.empty()) return n.text.empty() ? string() : "<m:r><m:rPr><m:sty m:val=\"p\"/></m:rPr><m:t xml:space=\"preserve\">" + xmlEsc(n.text) + "</m:t></m:r>";
      string child = omml(n.children[0]);
      if (n.text.empty()) return child;
      // Explicitly wrap text-style expressions in a math run property; nested structures remain editable OMML.
      return "<m:box><m:boxPr><m:opEmu m:val=\"0\"/></m:boxPr><m:e>" + child + "</m:e></m:box>";
    }
    case Node::Space:
      return n.text.empty() ? string() : "<m:r><m:rPr><m:spacing m:val=\"0\"/></m:rPr><m:t xml:space=\"preserve\"> </m:t></m:r>";
    case Node::Fraction:
      if (n.children.size() < 2) return string();
      return string("<m:f><m:fPr>") + (n.noBar ? "<m:type m:val=\"skw\"/>" : "") + "</m:fPr><m:num>" + omml(n.children[0]) + "</m:num><m:den>" + omml(n.children[1]) + "</m:den></m:f>";
    case Node::Root: {
      if (n.children.empty()) return string();
      if (n.hasDegree && n.children.size() >= 2)
        return "<m:rad><m:radPr><m:degHide m:val=\"0\"/></m:radPr><m:deg>" + omml(n.children[0]) + "</m:deg><m:e>" + omml(n.children[1]) + "</m:e></m:rad>";
      return "<m:rad><m:radPr><m:degHide m:val=\"1\"/></m:radPr><m:deg/><m:e>" + omml(n.children[0]) + "</m:e></m:rad>";
    }
    case Node::Script: {
      if (n.children.empty()) return string();
      string base = omml(n.children[0]);
      if (n.hasSub && n.hasSup) return "<m:sSubSup><m:sSubSupPr/><m:e>" + base + "</m:e><m:sub>" + omml(n.children[1]) + "</m:sub><m:sup>" + omml(n.children[2]) + "</m:sup></m:sSubSup>";
      if (n.hasSub) return "<m:sSub><m:sSubPr/><m:e>" + base + "</m:e><m:sub>" + omml(n.children[1]) + "</m:sub></m:sSub>";
      if (n.hasSup) return "<m:sSup><m:sSupPr/><m:e>" + base + "</m:e><m:sup>" + omml(n.children[2]) + "</m:sup></m:sSup>";
      return base;
    }
    case Node::Accent:
      if (n.children.empty()) return string();
      return "<m:acc><m:accPr><m:chr m:val=\"" + xmlEsc(n.text) + "\"/></m:accPr><m:e>" + omml(n.children[0]) + "</m:e></m:acc>";
  }
  return string();
}

string mathml(const Node& n) {
  switch (n.kind) {
    case Node::Row: {
      string o = "<mrow>";
      for (const Node& c : n.children) o += mathml(c);
      return o + "</mrow>";
    }
    case Node::Ident: return "<mi>" + xmlEsc(n.text) + "</mi>";
    case Node::Number: return "<mn>" + xmlEsc(n.text) + "</mn>";
    case Node::Operator: return "<mo>" + xmlEsc(n.text) + "</mo>";
    case Node::Upright:
      if (n.children.empty()) return "<mi mathvariant=\"normal\">" + xmlEsc(n.text) + "</mi>";
      return "<mstyle mathvariant=\"normal\">" + mathml(n.children[0]) + "</mstyle>";
    case Node::Space: return "<mspace width=\"" + (n.text.empty() ? string("0.2em") : xmlEsc(n.text)) + "\"/>";
    case Node::Fraction:
      return n.children.size() < 2 ? string() : "<mfrac" + string(n.noBar ? " linethickness=\"0\"" : "") + ">" + mathml(n.children[0]) + mathml(n.children[1]) + "</mfrac>";
    case Node::Root:
      if (n.children.empty()) return string();
      return n.hasDegree && n.children.size() >= 2 ? "<mroot>" + mathml(n.children[1]) + mathml(n.children[0]) + "</mroot>" : "<msqrt>" + mathml(n.children[0]) + "</msqrt>";
    case Node::Script:
      if (n.children.empty()) return string();
      if (n.hasSub && n.hasSup) return "<msubsup>" + mathml(n.children[0]) + mathml(n.children[1]) + mathml(n.children[2]) + "</msubsup>";
      if (n.hasSub) return "<msub>" + mathml(n.children[0]) + mathml(n.children[1]) + "</msub>";
      if (n.hasSup) return "<msup>" + mathml(n.children[0]) + mathml(n.children[2]) + "</msup>";
      return mathml(n.children[0]);
    case Node::Accent:
      if (n.children.empty()) return string();
      return "<mover accent=\"true\">" + mathml(n.children[0]) + "<mo>" + xmlEsc(n.text) + "</mo></mover>";
  }
  return string();
}

string superscript(const string& s) {
  static const std::map<char, string> map = {{'0', "⁰"}, {'1', "¹"}, {'2', "²"}, {'3', "³"}, {'4', "⁴"}, {'5', "⁵"}, {'6', "⁶"}, {'7', "⁷"}, {'8', "⁸"}, {'9', "⁹"},
                                             {'+', "⁺"}, {'-', "⁻"}, {'=', "⁼"}, {'(', "⁽"}, {')', "⁾"}};
  string o;
  for (char c : s) { auto it = map.find(c); if (it == map.end()) return "^(" + s + ")"; o += it->second; }
  return o;
}
string subscript(const string& s) {
  static const std::map<char, string> map = {{'0', "₀"}, {'1', "₁"}, {'2', "₂"}, {'3', "₃"}, {'4', "₄"}, {'5', "₅"}, {'6', "₆"}, {'7', "₇"}, {'8', "₈"}, {'9', "₉"},
                                             {'+', "₊"}, {'-', "₋"}, {'=', "₌"}, {'(', "₍"}, {')', "₎"}};
  string o;
  for (char c : s) { auto it = map.find(c); if (it == map.end()) return "_(" + s + ")"; o += it->second; }
  return o;
}
string linear(const Node& n) {
  switch (n.kind) {
    case Node::Row: { string o; for (auto& c : n.children) o += linear(c); return o; }
    case Node::Ident: case Node::Number: case Node::Operator: case Node::Upright: return n.children.empty() ? n.text : linear(n.children[0]);
    case Node::Space: return " ";
    case Node::Fraction: return n.children.size() < 2 ? string() : "(" + linear(n.children[0]) + ")⁄(" + linear(n.children[1]) + ")";
    case Node::Root: return n.children.empty() ? string() : (n.hasDegree ? "root(" + linear(n.children[0]) + ", " + linear(n.children[1]) + ")" : "√(" + linear(n.children[0]) + ")");
    case Node::Script: {
      if (n.children.empty()) return string();
      string o = linear(n.children[0]);
      if (n.hasSub) o += subscript(linear(n.children[1]));
      if (n.hasSup) o += superscript(linear(n.children[2]));
      return o;
    }
    case Node::Accent: return n.children.empty() ? string() : linear(n.children[0]) + n.text;
  }
  return string();
}

bool parseForExport(const string& latex, Node& root, string* error) {
  if (latex.size() > 32768) { if (error) *error = "Equation source is longer than 32,768 bytes."; return false; }
  Parser p(latex);
  root = p.parse();
  if (!p.ok()) { if (error) *error = p.error(); return false; }
  if (error) error->clear();
  return true;
}

}  // namespace

string mathToOmml(const string& latex, string* error) {
  Node root;
  if (!parseForExport(latex, root, error)) return string();
  return "<m:oMath xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\">" + omml(root) + "</m:oMath>";
}

string mathToMathML(const string& latex, string* error) {
  Node root;
  if (!parseForExport(latex, root, error)) return string();
  return "<math xmlns=\"" + string(kMathMlNs) + "\" display=\"inline\">" + mathml(root) + "</math>";
}

string mathPreviewText(const string& latex) {
  Node root;
  if (!parseForExport(latex, root, nullptr)) return latex;
  return linear(root);
}

}  // namespace vs
