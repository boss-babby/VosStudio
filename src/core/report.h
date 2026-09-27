// VOSStudio Native: literature reports written by the agent. A report is an ordered list of text sections and figures
// (charts and maps as vector Scenes), laid out on A4 pages and written with the multi-page PDF writer.
#pragma once
#include "figure.h"

namespace vs {

struct ReportBlock {
  enum Kind { Section, Figure } kind = Section;
  string title;    // section heading or figure title
  string text;     // section body (light Markdown: ## headings, - bullets, **bold** lines, | tables |) or figure caption
  Scene fig;       // figure content, any size (scaled to the text width)
  string figId;    // chart or map id the figure was made from
};

struct ReportDoc {
  string title, subtitle, date;
  vector<string> meta;        // lines under the title: data source, query, records, years
  vector<ReportBlock> blocks;
  vector<string> references;  // formatted references, numbered in order
  bool empty() const { return blocks.empty(); }
  int figures() const;
  int sections() const;
};

// Replaces citation keys [R12] / [R12, R4] in the section texts with numbers [1] / [1, 2] in order of first use and
// returns the record numbers (1-based, as given to the model) in that order.
// Keys outside 1..maxId are dropped.
vector<int> numberCitations(ReportDoc& d, int maxId);

// A4 portrait pages (595 x 842 pt): title block, sections and figures in order (figures scaled to the text width and
// kept whole), references, page numbers.
vector<Scene> layoutReport(const ReportDoc& d, bool serif = false);

// draws src into dst: every primitive scaled by k and moved by (ox, oy)
void appendScene(Scene& dst, const Scene& src, double ox, double oy, double k);

}  // namespace vs
