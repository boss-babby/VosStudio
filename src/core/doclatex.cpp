// VOSStudio Native: standalone LaTeX bundle generation from the visual document model.
// The editor remains canonical; this exporter never requires users to author TeX.
#include "doc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace vs {
namespace {

string texEscape(const string& text, bool cell = false) {
  string out;
  out.reserve(text.size() + text.size() / 8);
  for (unsigned char c : text) {
    switch (c) {
      case '\\': out += "\\textbackslash{}"; break;
      case '{': out += "\\{"; break;
      case '}': out += "\\}"; break;
      case '#': out += "\\#"; break;
      case '$': out += "\\$"; break;
      case '%': out += "\\%"; break;
      case '&': out += "\\&"; break;
      case '_': out += "\\_"; break;
      case '~': out += "\\textasciitilde{}"; break;
      case '^': out += "\\textasciicircum{}"; break;
      case '\r': break;
      case '\n': out += cell ? "\\newline{}" : "\\\\\n"; break;
      default: out += char(c); break;
    }
  }
  return out;
}

string familyCommand(const string& font) {
  if (PageSetup::monoFamily(font)) return "\\texttt";
  return PageSetup::serifFamily(font) ? "\\textrm" : "\\textsf";
}

string familySwitch(const string& font) {
  if (PageSetup::monoFamily(font)) return "\\ttfamily";
  return PageSetup::serifFamily(font) ? "\\rmfamily" : "\\sffamily";
}

string spanTex(const Span& span, bool cell = false) {
  if (span.fmt & F_MATH) return "\\(" + span.text + "\\)";
  string text = texEscape(span.text, cell);
  if (span.fmt & F_CODE) text = "\\texttt{" + text + "}";
  if (span.fmt & F_BOLD) text = "\\textbf{" + text + "}";
  if (span.fmt & F_ITALIC) text = "\\emph{" + text + "}";
  if (span.fmt & F_UNDER) text = "\\uline{" + text + "}";
  if (span.fmt & F_STRIKE) text = "\\sout{" + text + "}";
  if (span.fmt & F_SUB) text = "\\textsubscript{" + text + "}";
  if (span.fmt & F_SUP) text = "\\textsuperscript{" + text + "}";
  if (span.fmt & F_MARK) text = "\\colorbox{yellow!40}{" + text + "}";
  if (!span.link.empty()) text = "\\href{" + texEscape(span.link) + "}{" + text + "}";
  if (!span.font.empty()) text = familyCommand(span.font) + "{" + text + "}";
  if (span.size > 0 && std::isfinite(span.size)) {
    const double lead = std::max(1.0, double(span.size) * 1.15);
    text = "{\\fontsize{" + fmtNum(span.size, 1) + "pt}{" + fmtNum(lead, 1) + "pt}\\selectfont " + text + "}";
  }
  if (span.color) {
    char rgb[7];
    snprintf(rgb, sizeof rgb, "%02x%02x%02x", (span.color >> 16) & 255, (span.color >> 8) & 255, span.color & 255);
    text = "\\textcolor[HTML]{" + string(rgb) + "}{" + text + "}";
  }
  return text;
}

string spansTex(const vector<Span>& spans, bool cell = false) {
  string out;
  for (const Span& span : spans) out += spanTex(span, cell);
  return out;
}

bool captionHasLabel(const string& text) {
  string t = lower(trim(text));
  for (const char* prefix : {"figure ", "fig. ", "fig ", "table ", "tbl. "}) {
    size_t n = strlen(prefix);
    if (t.size() > n && startsWith(t, prefix) && isdigit(uint8_t(t[n]))) return true;
  }
  return false;
}

int tableForCaption(const Document& d, int k) {
  if (k + 1 < int(d.blocks.size()) && d.blocks[size_t(k) + 1].kind == Block::TableBlock) return k + 1;
  if (k > 0 && d.blocks[size_t(k) - 1].kind == Block::TableBlock && !(k + 1 < int(d.blocks.size()) && d.blocks[size_t(k) + 1].kind == Block::TableBlock)) return k - 1;
  return -1;
}

string headingCommand(PStyle style) {
  switch (style) {
    case PStyle::H1: return "section";
    case PStyle::H2: return "subsection";
    case PStyle::H3: return "subsubsection";
    default: return "paragraph";
  }
}

string aligned(const string& body, PAlign alignment) {
  if (alignment == PAlign::Center) return "{\\centering " + body + "\\par}\n";
  if (alignment == PAlign::Right) return "{\\raggedleft " + body + "\\par}\n";
  if (alignment == PAlign::Justify) return "{\\justifying " + body + "\\par}\n";
  return body + "\\par\n";
}

string bibKey(const RefEntry& ref, size_t index, std::set<string>& used) {
  string base = ref.key;
  if (base.empty()) base = ref.structured() ? refKeyFor(ref.doi, ref.title, ref.year) : "raw:" + sha256Hex(ref.raw).substr(0, 12);
  string key;
  for (unsigned char c : lower(base)) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) key += char(c);
    else if (c == '-' || c == '_') key += char(c);
    else if (!key.empty() && key.back() != '-') key += '-';
  }
  while (!key.empty() && (key.back() == '-' || key.back() == '_')) key.pop_back();
  if (key.empty()) key = "reference-" + std::to_string(index + 1);
  string candidate = key;
  for (int suffix = 2; used.count(candidate); suffix++) candidate = key + "-" + std::to_string(suffix);
  used.insert(candidate);
  return candidate;
}

string bibEntryType(const string& kind) {
  if (kind == "article") return "article";
  if (kind == "conference") return "inproceedings";
  if (kind == "book") return "book";
  if (kind == "chapter") return "incollection";
  if (kind == "thesis") return "phdthesis";
  if (kind == "report") return "techreport";
  return "misc";
}

void bibField(string& out, const char* name, const string& value) {
  if (trim(value).empty()) return;
  string clean = replaceAll(replaceAll(value, "\r", " "), "\n", " ");
  out += "  " + string(name) + " = {" + texEscape(clean) + "},\n";
}

string safeAssetDirectory(const string& input) {
  string out;
  for (unsigned char c : lower(input)) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out += char(c);
    else if (!out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && (out.back() == '-' || out.back() == '_')) out.pop_back();
  return out.empty() || out == "." || out == ".." ? string("figures") : out;
}

string columnSpec(const Table& table) {
  string out;
  for (int c = 0; c < table.cols(); c++) {
    char align = c < int(table.align.size()) ? table.align[size_t(c)] : 'a';
    double width = c < int(table.widths.size()) ? table.widths[size_t(c)] : 0;
    if (std::isfinite(width) && width > 0) {
      string cmd = align == 'c' ? "\\centering" : align == 'r' ? "\\raggedleft" : "\\raggedright";
      out += ">{" + cmd + "\\arraybackslash}p{" + fmtNum(clampv(width, 0.02, 1.0), 3) + "\\linewidth}";
    } else out += align == 'c' ? "c" : align == 'r' ? "r" : "l";
  }
  return out;
}

}  // namespace

LatexBundle docToLatex(const Document& d, const string& assetDirectory0) {
  LatexBundle out;
  const string assetDirectory = safeAssetDirectory(assetDirectory0);
  vector<string> figurePath(d.blocks.size());
  int figureAsset = 0;
  bool hasAltText = false;
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& block = d.blocks[k];
    if (block.kind != Block::FigureBlock) continue;
    if (block.fig.asset < 0 || block.fig.asset >= int(d.assets.size())) {
      out.warnings.push_back("Figure " + std::to_string(d.figureNumber(int(k))) + " has no available vector asset; a placeholder will be exported.");
      continue;
    }
    const FigAsset& asset = d.assets[size_t(block.fig.asset)];
    string bytes = toPDF(asset.scene, asset.title.empty() ? "VOSStudio figure" : asset.title);
    if (bytes.empty()) {
      out.warnings.push_back("Figure " + std::to_string(d.figureNumber(int(k))) + " could not be converted to a vector PDF; a placeholder will be exported.");
      continue;
    }
    string name = assetDirectory + "/figure-" + std::to_string(++figureAsset) + ".pdf";
    figurePath[k] = name;
    out.assets.emplace_back(name, std::move(bytes));
    if (!block.fig.alt.empty()) hasAltText = true;
  }
  if (hasAltText) out.warnings.push_back("Figure alternative text is preserved in TeX comments; the generated PDF is not tagged for accessibility.");

  std::set<string> usedBibKeys;
  vector<string> bibKeys(d.refs.size());
  out.bibliography = "% Companion BibTeX file generated from the shared VOSStudio reference model.\n%\n";
  for (size_t i = 0; i < d.refs.size(); i++) {
    const RefEntry& ref = d.refs[i];
    string key = bibKey(ref, i, usedBibKeys);
    out.bibliography += "@" + bibEntryType(ref.kind) + "{" + key + ",\n";
    if (!ref.authors.empty()) bibField(out.bibliography, "author", join(ref.authors, " and "));
    bibField(out.bibliography, "title", ref.title);
    bibField(out.bibliography, "year", ref.year);
    bibField(out.bibliography, "journal", ref.container);
    bibField(out.bibliography, "volume", ref.volume);
    bibField(out.bibliography, "number", ref.issue);
    bibField(out.bibliography, "pages", ref.pages);
    bibField(out.bibliography, "publisher", ref.publisher);
    bibField(out.bibliography, "address", ref.place);
    bibField(out.bibliography, "doi", ref.doi);
    bibField(out.bibliography, "url", ref.url);
    if (!ref.raw.empty()) bibField(out.bibliography, "note", ref.raw);
    out.bibliography += "}\n\n";
  }
  if (d.refs.empty()) out.bibliography.clear();

  const PageSetup& setup = d.setup;
  bool hasExplicitToc = std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::Toc; });
  bool hasReferenceParagraph = std::any_of(d.blocks.begin(), d.blocks.end(), [](const Block& b) { return b.kind == Block::Paragraph && b.p.style == PStyle::Reference; });
  int classSize = setup.baseSize >= 11.5f ? 12 : setup.baseSize < 10.5f ? 10 : 11;
  string paper = setup.paper == "Letter" ? "letterpaper" : "a4paper";
  out.source = "% Generated from the VOSStudio visual document model. Edit the document in VOSStudio for source-of-truth changes.\n";
  out.source += "\\documentclass[" + std::to_string(classSize) + "pt," + paper + "]{article}\n";
  out.source += "\\usepackage[paper=" + paper + ",margin=" + fmtNum(setup.margin() / 72.0, 3) + "in]{geometry}\n";
  out.source += "\\usepackage{amsmath}\n\\usepackage{graphicx}\n\\usepackage{array}\n\\usepackage{longtable}\n\\usepackage{booktabs}\n";
  out.source += "\\usepackage{caption}\n\\usepackage[normalem]{ulem}\n\\usepackage[table]{xcolor}\n\\usepackage{ragged2e}\n\\usepackage{hyperref}\n";
  if (!setup.header.empty()) out.source += "\\usepackage{fancyhdr}\n";
  out.source += "\\setlength{\\parindent}{0pt}\n\\setlength{\\parskip}{0.45em}\n";
  out.source += "\\begin{document}\n";
  out.source += "\\fontsize{" + fmtNum(std::max(6.f, setup.baseSize), 1) + "pt}{" + fmtNum(std::max(7.f, setup.baseSize * setup.lineSpacing), 1) + "pt}\\selectfont\n";
  out.source += familySwitch(setup.font) + "\n";
  if (!setup.font.empty()) out.warnings.push_back("The body font '" + setup.font + "' is mapped to a generic TeX family; exact font matching requires a verified Tectonic font setup.");
  if (!setup.headingFont.empty() && (PageSetup::serifFamily(setup.headingFont) != PageSetup::serifFamily(setup.font) || PageSetup::monoFamily(setup.headingFont) != PageSetup::monoFamily(setup.font)))
    out.warnings.push_back("The heading font family is reduced to LaTeX's generic heading family; exact installed-font matching is not available in source generation.");
  if (!setup.header.empty()) {
    out.source += "\\pagestyle{fancy}\n\\fancyhf{}\n\\setlength{\\headheight}{14pt}\n\\fancyhead[R]{" + texEscape(setup.header) + "}\n";
    if (setup.pageNumbers) out.source += "\\fancyfoot[C]{\\thepage}\n";
  } else if (setup.pageNumbers) out.source += "\\pagestyle{plain}\n";
  else out.source += "\\pagestyle{empty}\n";

  vector<string> listStack;
  auto closeLists = [&]() {
    while (!listStack.empty()) { out.source += "\\end{" + listStack.back() + "}\n"; listStack.pop_back(); }
  };
  auto toc = [&]() { out.source += "\\clearpage\n\\tableofcontents\n\\clearpage\n"; };
  bool frontMatterDone = false, titleWritten = false, tocWritten = false;
  for (size_t k = 0; k < d.blocks.size(); k++) {
    const Block& block = d.blocks[k];
    bool front = block.kind == Block::Paragraph && (block.p.style == PStyle::Title || block.p.style == PStyle::Subtitle || block.p.style == PStyle::Meta);
    if (!front && !frontMatterDone) {
      frontMatterDone = true;
      if (setup.toc && !hasExplicitToc && !tocWritten) { closeLists(); toc(); tocWritten = true; }
    }
    if (block.kind == Block::Paragraph) {
      const Para& p = block.p;
      string body = spansTex(p.spans);
      if (isList(p.style)) {
        string want = p.style == PStyle::Bullet ? "itemize" : "enumerate";
        int depth = clampv(p.level, 0, 2) + 1;
        if (!listStack.empty() && listStack[0] != want) closeLists();
        while (int(listStack.size()) > depth) { out.source += "\\end{" + listStack.back() + "}\n"; listStack.pop_back(); }
        while (int(listStack.size()) < depth) {
          out.source += "\\begin{" + want + "}\n";
          listStack.push_back(want);
        }
        if (want == "enumerate" && p.numStart > 0) {
          const char* counter[] = {"enumi", "enumii", "enumiii"};
          out.source += "\\setcounter{" + string(counter[depth - 1]) + "}{" + std::to_string(p.numStart - 1) + "}\n";
        }
        out.source += "\\item " + body + "\n";
        continue;
      }
      closeLists();
      switch (p.style) {
        case PStyle::Title:
          out.source += "\\begingroup\\Huge\\bfseries\n" + aligned(body, p.align) + "\\endgroup\n";
          if (!titleWritten && !d.author.empty()) out.source += "{\\large " + texEscape(d.author) + "\\par}\n";
          if (!titleWritten && !d.keywords.empty()) out.source += "{\\small\\textbf{Keywords:} " + texEscape(d.keywords) + "\\par}\n";
          titleWritten = true;
          break;
        case PStyle::Subtitle: out.source += "\\begingroup\\Large\n" + aligned(body, p.align) + "\\endgroup\n"; break;
        case PStyle::Meta: out.source += "\\begingroup\\small\\color{gray}\n" + aligned(body, p.align) + "\\endgroup\n"; break;
        case PStyle::H1: case PStyle::H2: case PStyle::H3: case PStyle::H4: {
          string cmd = headingCommand(p.style);
          if (setup.numberedHeadings) out.source += "\\" + cmd + "{" + body + "}\n";
          else out.source += "\\phantomsection\\" + cmd + "*{" + body + "}\\addcontentsline{toc}{" + cmd + "}{" + body + "}\n";
          break;
        }
        case PStyle::Quote: out.source += "\\begin{quote}\n" + body + "\n\\end{quote}\n"; break;
        case PStyle::Code: out.source += "\\begin{quote}\\ttfamily\\small\n" + body + "\n\\end{quote}\n"; break;
        case PStyle::Caption: {
          int table = tableForCaption(d, int(k));
          if (table >= 0) {
            if (captionHasLabel(p.text())) out.source += "\\refstepcounter{table}\\caption*{" + body + "}\\par\n";
            else out.source += "\\captionof{table}{" + body + "}\n";
          } else out.source += aligned(body, p.align);
          break;
        }
        case PStyle::Reference: out.source += "\\noindent\\hangindent=2em\\hangafter=1 " + body + "\\par\n"; break;
        default: out.source += aligned(body, p.align); break;
      }
    } else if (block.kind == Block::FigureBlock) {
      closeLists();
      string align = block.fig.align == PAlign::Left ? "\\raggedright" : block.fig.align == PAlign::Right ? "\\raggedleft" : "\\centering";
      out.source += "\\begin{figure}[htbp]\n" + align + "\n";
      auto caption = [&]() {
        string cap = spansTex(block.fig.caption.spans);
        if (cap.empty()) return;
        if (block.fig.label == "-") out.source += "\\caption*{" + cap + "}\n";
        else if (captionHasLabel(block.fig.caption.text())) out.source += "\\refstepcounter{figure}\\caption*{" + cap + "}\n";
        else if (!block.fig.label.empty() && block.fig.label != "Figure")
          out.source += "{\\captionsetup{name={" + texEscape(block.fig.label) + "}}\n\\caption{" + cap + "}}\n";
        else out.source += "\\caption{" + cap + "}\n";
      };
      if (block.fig.captionAbove) caption();
      if (!figurePath[k].empty()) {
        string img = "\\includegraphics[width=" + fmtNum(clampv(block.fig.widthPct, 5, 100) / 100.0, 2) + "\\linewidth]{" + figurePath[k] + "}";
        out.source += block.fig.border ? "\\fbox{" + img + "}\n" : img + "\n";
      } else out.source += "\\fbox{\\parbox{0.8\\linewidth}{\\centering [Figure asset unavailable]}}\n";
      if (!block.fig.alt.empty()) out.source += "% Alternative text: " + replaceAll(replaceAll(block.fig.alt, "\r", " "), "\n", " ") + "\n";
      if (!block.fig.captionAbove) caption();
      out.source += "\\end{figure}\n";
    } else if (block.kind == Block::TableBlock) {
      closeLists();
      const Table& table = block.tbl;
      if (table.rows() <= 0 || table.cols() <= 0) { out.warnings.push_back("An empty table block was omitted from the TeX source."); continue; }
      out.source += "\\begin{longtable}{" + columnSpec(table) + "}\n\\toprule\n";
      auto row = [&](int r, bool bold) {
        for (int c = 0; c < table.cols(); c++) {
          if (c) out.source += " & ";
          string cell = spansTex(table.cells[size_t(r)][size_t(c)].spans, true);
          out.source += bold ? "\\textbf{" + cell + "}" : cell;
        }
        out.source += " \\\\\n";
      };
      if (table.header) {
        row(0, true);
        out.source += "\\midrule\\endfirsthead\n\\toprule\n";
        row(0, true);
        out.source += "\\midrule\\endhead\n";
      }
      for (int r = table.header ? 1 : 0; r < table.rows(); r++) row(r, false);
      out.source += "\\bottomrule\n\\end{longtable}\n";
    } else if (block.kind == Block::Rule) { closeLists(); out.source += "\\noindent\\rule{\\linewidth}{0.4pt}\\par\n"; }
    else if (block.kind == Block::PageBreak) { closeLists(); out.source += "\\clearpage\n"; }
    else if (block.kind == Block::Toc) { closeLists(); toc(); tocWritten = true; }
  }
  closeLists();
  if (!hasReferenceParagraph && !d.refs.empty()) {
    out.source += "\\section*{References}\\addcontentsline{toc}{section}{References}\n";
    vector<int> order;
    vector<string> cited = d.citedKeys();
    if (citeStyleInfo(d.citeStyle).numeric) {
      for (const string& key : cited) for (size_t i = 0; i < d.refs.size(); i++) if (d.refs[i].key == key) { order.push_back(int(i)); break; }
      for (size_t i = 0; i < d.refs.size(); i++) if (std::find(order.begin(), order.end(), int(i)) == order.end()) order.push_back(int(i));
    } else {
      for (size_t i = 0; i < d.refs.size(); i++) order.push_back(int(i));
      std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return refSortKey(d.refs[size_t(a)]) < refSortKey(d.refs[size_t(b)]); });
    }
    for (size_t n = 0; n < order.size(); n++) {
      vector<Span> ref = formatReference(d.refs[size_t(order[n])], d.citeStyle, int(n) + 1);
      out.source += "\\noindent\\hangindent=2em\\hangafter=1 " + spansTex(ref) + "\\par\n";
    }
  }
  out.source += "\\end{document}\n";
  return out;
}

}  // namespace vs
