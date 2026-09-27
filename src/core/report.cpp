// VOSStudio Native: report layout (see report.h)
#include "report.h"

#include <map>
#include <regex>

namespace vs {

int ReportDoc::figures() const { int n = 0; for (auto& b : blocks) n += b.kind == ReportBlock::Figure; return n; }
int ReportDoc::sections() const { int n = 0; for (auto& b : blocks) n += b.kind == ReportBlock::Section; return n; }

vector<int> numberCitations(ReportDoc& d, int maxId) {
  vector<int> order;
  std::map<int, int> num;
  std::regex group(R"(\[(R\d+(?:\s*[,;]\s*R\d+)*)\])");
  std::regex one(R"(R(\d+))");
  for (auto& b : d.blocks) {
    if (b.kind != ReportBlock::Section) continue;
    string out;
    string s = b.text;
    std::smatch m;
    while (std::regex_search(s, m, group)) {
      out += m.prefix().str();
      string inner = m[1].str();
      vector<int> ns;
      for (std::sregex_iterator it(inner.begin(), inner.end(), one), end; it != end; ++it) {
        int r = std::atoi((*it)[1].str().c_str());
        if (r < 1 || r > maxId) continue;
        if (!num.count(r)) { num[r] = int(order.size()) + 1; order.push_back(r); }
        ns.push_back(num[r]);
      }
      std::sort(ns.begin(), ns.end());
      ns.erase(std::unique(ns.begin(), ns.end()), ns.end());
      string lab;
      for (size_t k = 0; k < ns.size(); k++) {
        // ranges: 1, 2, 3 -> 1-3
        size_t e = k;
        while (e + 1 < ns.size() && ns[e + 1] == ns[e] + 1) e++;
        lab += (lab.empty() ? "" : ", ") + std::to_string(ns[k]) + (e >= k + 2 ? "\xE2\x80\x93" + std::to_string(ns[e]) : e == k + 1 ? ", " + std::to_string(ns[e]) : "");
        k = e;
      }
      if (!lab.empty()) out += "[" + lab + "]";
      else while (!out.empty() && out.back() == ' ') out.pop_back();
      s = m.suffix().str();
    }
    b.text = out + s;
  }
  return order;
}

void appendScene(Scene& dst, const Scene& src, double ox, double oy, double k) {
  float K = float(k);
  auto X = [&](float x) { return float(x * k + ox); };
  auto Y = [&](float y) { return float(y * k + oy); };
  for (const Prim& p0 : src.items) {
    Prim p = p0;
    p.x = X(p0.x);
    p.y = Y(p0.y);
    p.w = p0.w * K;
    p.h = p0.h * K;
    p.r = p0.r * K;
    p.sw = p0.sw * K;
    p.size = p0.size * K;
    p.haloW = p0.haloW * K;
    for (auto& dsh : p.dash) dsh *= K;
    for (auto& c : p.d) { c.x1 = X(c.x1); c.y1 = Y(c.y1); c.x2 = X(c.x2); c.y2 = Y(c.y2); }
    dst.items.push_back(std::move(p));
  }
}

namespace {

const double PW = 595.28, PH = 841.89, ML = 62, MR = 62, MT = 64, MB = 64;
const double CW = PW - ML - MR;
const Color INK(0.1f, 0.11f, 0.13f), MUTED(0.38f, 0.4f, 0.45f), RULE(0.82f, 0.84f, 0.87f), ACCENT(0.1f, 0.4f, 0.8f);

Prim textPrim(double x, double y, const string& t, double size, Color c, bool bold, int anchor = 0) {
  Prim p;
  p.type = Prim::Text;
  p.x = float(x);
  p.y = float(y);
  p.text = t;
  p.size = float(size);
  p.fillC = c;
  p.fill = true;
  p.bold = bold;
  p.anchor = anchor;
  p.group = "text";
  return p;
}

Prim rulePrim(double x0, double y, double x1, Color c, double w) {
  Prim p;
  p.type = Prim::Path;
  p.stroke = true;
  p.strokeC = c;
  p.sw = float(w);
  p.d.push_back({'M', float(x0), float(y), 0, 0});
  p.d.push_back({'L', float(x1), float(y), 0, 0});
  p.group = "text";
  return p;
}

string stripInline(string s) {
  // light Markdown: **bold**, *italic*, `code` and [text](url) become plain text
  s = std::regex_replace(s, std::regex(R"(\[([^\]]+)\]\((https?://[^)]+)\))"), "$1");
  string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '*' || s[i] == '`') continue;
    if (s[i] == '_' && (i + 1 < s.size() && s[i + 1] == '_')) { i++; continue; }
    o += s[i];
  }
  return o;
}

struct Pager {
  vector<Scene> pages;
  double y = MT;
  Scene& cur() { return pages.back(); }
  void newPage() {
    Scene s;
    s.W = PW;
    s.H = PH;
    Prim bg;
    bg.type = Prim::Rect;
    bg.x = 0; bg.y = 0; bg.w = float(PW); bg.h = float(PH);
    bg.fill = true;
    bg.fillC = Color(1, 1, 1);
    bg.group = "background";
    s.items.push_back(bg);
    pages.push_back(std::move(s));
    y = MT;
  }
  void need(double h) { if (y + h > PH - MB) newPage(); }
  // wrapped paragraph; indent for bullets
  void para(const string& text, double size, Color c, bool bold, double indent = 0, double after = 6, const string& bullet = "") {
    double lh = size * 1.42;
    auto lines = wrapText(text, size, CW - indent, bold);
    if (lines.empty()) lines.push_back("");
    for (size_t i = 0; i < lines.size(); i++) {
      need(lh);
      if (i == 0 && !bullet.empty()) cur().items.push_back(textPrim(ML + indent - size * 0.45, y + size, bullet, size, MUTED, false, 2));
      cur().items.push_back(textPrim(ML + indent, y + size, lines[i], size, c, bold));
      y += lh;
    }
    y += after;
  }
};

}  // namespace

vector<Scene> layoutReport(const ReportDoc& d, bool serif) {
  (void)serif;
  Pager pg;
  pg.newPage();
  // title block
  {
    auto tl = wrapText(d.title.empty() ? string("Literature report") : d.title, 21, CW, true);
    for (auto& l : tl) { pg.cur().items.push_back(textPrim(ML, pg.y + 21, l, 21, INK, true)); pg.y += 27; }
    if (!d.subtitle.empty()) { pg.y += 2; pg.para(d.subtitle, 12, MUTED, false, 0, 2); }
    pg.y += 6;
    pg.cur().items.push_back(rulePrim(ML, pg.y, PW - MR, ACCENT, 1.2));
    pg.y += 12;
    for (auto& m : d.meta) pg.para(m, 8.8, MUTED, false, 0, 0);
    if (!d.date.empty()) pg.para(d.date, 8.8, MUTED, false, 0, 0);
    pg.y += 16;
  }
  int figNo = 0;
  for (auto& b : d.blocks) {
    if (b.kind == ReportBlock::Section) {
      if (!b.title.empty()) {
        pg.need(14 * 1.4 + 3 * 10 * 1.42);  // keep the heading with its first lines
        pg.y += 4;
        pg.para(stripInline(b.title), 14, INK, true, 0, 4);
      }
      vector<string> lines = split(replaceAll(b.text, "\r", ""), '\n', true);
      string paraBuf;
      auto flush = [&]() { if (!trim(paraBuf).empty()) pg.para(stripInline(trim(paraBuf)), 10, INK, false, 0, 7); paraBuf.clear(); };
      for (size_t i = 0; i < lines.size(); i++) {
        string t = trim(lines[i]);
        if (t.empty()) { flush(); continue; }
        if (t.rfind("#", 0) == 0) {
          flush();
          size_t h = t.find_first_not_of('#');
          string ht = trim(t.substr(h == string::npos ? t.size() : h));
          pg.need(11.5 * 1.4 + 2 * 10 * 1.42);
          pg.y += 3;
          pg.para(stripInline(ht), 11.5, INK, true, 0, 3);
          continue;
        }
        bool bul = (t.size() > 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ');
        size_t dot = t.find_first_not_of("0123456789");
        bool num = dot != string::npos && dot > 0 && dot + 1 < t.size() && (t[dot] == '.' || t[dot] == ')') && t[dot + 1] == ' ';
        if (bul || num) {
          flush();
          string body = bul ? t.substr(2) : t.substr(dot + 2);
          // continuation lines of the item
          while (i + 1 < lines.size() && !trim(lines[i + 1]).empty() && lines[i + 1].size() > 1 && lines[i + 1][0] == ' ') {
            string nx = trim(lines[i + 1]);
            size_t d2 = nx.find_first_not_of("0123456789");
            if ((nx.size() > 2 && (nx[0] == '-' || nx[0] == '*') && nx[1] == ' ') || (d2 != string::npos && d2 > 0 && d2 < nx.size() && nx[d2] == '.')) break;
            body += " " + nx;
            i++;
          }
          pg.para(stripInline(body), 10, INK, false, 14, 3, bul ? "\xE2\x80\xA2" : t.substr(0, dot + 1));
          continue;
        }
        if (t[0] == '|') {
          flush();
          if (t.find("---") != string::npos && t.find_first_not_of("|-: ") == string::npos) continue;
          vector<string> cells;
          for (auto& c : split(t.substr(1, t.size() > 1 && t.back() == '|' ? t.size() - 2 : t.size() - 1), '|', true)) cells.push_back(trim(stripInline(c)));
          pg.para(join(cells, "  \xC2\xB7  "), 9.5, INK, i > 0 && trim(lines[i - 1]).empty(), 0, 1);
          continue;
        }
        if (t[0] == '>') { flush(); pg.para(stripInline(trim(t.substr(1))), 10, MUTED, false, 12, 6); continue; }
        paraBuf += (paraBuf.empty() ? "" : " ") + t;
      }
      flush();
      pg.y += 6;
    } else {
      figNo++;
      const Scene& f = b.fig;
      if (f.W <= 0 || f.H <= 0) continue;
      double capH = 0;
      string cap = "Figure " + std::to_string(figNo) + ". " + stripInline(b.title) + (b.text.empty() ? string() : ". " + stripInline(b.text));
      auto capLines = wrapText(cap, 8.8, CW, false);
      capH = double(capLines.size()) * 8.8 * 1.4 + 4;
      double k = CW / f.W;
      double maxH = PH - MT - MB - capH - 8;
      if (f.H * k > maxH) k = maxH / f.H;
      double h = f.H * k, w = f.W * k;
      pg.need(h + capH + 10);
      pg.y += 4;
      appendScene(pg.cur(), f, ML + (CW - w) / 2, pg.y, k);
      pg.y += h + 6;
      for (auto& l : capLines) { pg.cur().items.push_back(textPrim(ML, pg.y + 8.8, l, 8.8, MUTED, false)); pg.y += 8.8 * 1.4; }
      pg.y += 14;
    }
  }
  if (!d.references.empty()) {
    pg.need(14 * 1.4 + 40);
    pg.y += 6;
    pg.para("References", 14, INK, true, 0, 6);
    for (size_t i = 0; i < d.references.size(); i++) pg.para(d.references[i], 8.8, INK, false, 26, 4, "[" + std::to_string(i + 1) + "]");
  }
  // footer: page numbers and the report title
  int n = int(pg.pages.size());
  for (int i = 0; i < n; i++) {
    Scene& s = pg.pages[size_t(i)];
    s.items.push_back(rulePrim(ML, PH - MB + 22, PW - MR, RULE, 0.5));
    s.items.push_back(textPrim(ML, PH - MB + 36, truncate(d.title, 90), 7.5, MUTED, false));
    s.items.push_back(textPrim(PW - MR, PH - MB + 36, std::to_string(i + 1) + " / " + std::to_string(n), 7.5, MUTED, false, 2));
  }
  return pg.pages;
}

}  // namespace vs
