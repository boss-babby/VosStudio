// VOSStudio Native — the reading library: the Read page (left panel), the reader's side pane and the file flows
// (attach, identify, relocate, detach, save annotations into the PDF, exports). The data lives in P->library.
#include <algorithm>
#include <cmath>

#include "../core/doc.h"
#include "../core/oa.h"
#include "reader.h"

namespace vs {
namespace win {

namespace {
const char* kStatusNames[] = {"To read", "Reading", "Read", "Excluded"};
Color statusColor(const Ui& ui, ReadStatus st) {
  switch (st) {
    case ReadStatus::Reading: return ui.c.accent;
    case ReadStatus::Read: return ui.c.ok;
    case ReadStatus::Excluded: return ui.c.textFaint;
    default: return ui.c.textDim;
  }
}
string itemTitle(const PdfItem& it) { return !it.title.empty() ? it.title : PdfLibrary::fileTitle(it.path); }
string itemSub(const PdfItem& it, const Record* rec) {
  string sub;
  if (rec) {
    if (!rec->authors.empty()) sub = rec->authors[0] + (rec->authors.size() > 1 ? " et al." : "");
    if (rec->year) sub += (sub.empty() ? "" : " \xC2\xB7 ") + std::to_string(rec->year);
    if (!rec->source.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + rec->source;
  } else {
    if (!it.author.empty()) sub = it.author;
    if (it.year) sub += (sub.empty() ? "" : " \xC2\xB7 ") + std::to_string(it.year);
  }
  return sub;
}
string fmtBytes(long long b) {
  if (b < 0) return "";
  if (b < 1024 * 1024) return fmtNum(double(b) / 1024, 0) + " KB";
  return fmtNum(double(b) / (1024.0 * 1024), 1) + " MB";
}
// A list row with buttons in a strip at its right end. Ui::behave gives a press to the first widget that claims it,
// so the row's own hit area stops before the strip; the hover fill still covers the whole row.
bool rowWithStrip(Ui& ui, const Rect& r, const string& key, bool selected, float stripW, bool* over) {
  const float s = ui.s;
  uint64_t idv = ui.id("row:" + key);
  bool hov = false;
  bool click = ui.behave(idv, {r.x, r.y, std::max(24 * s, r.w - stripW), r.h}, &hov);
  bool on = hov || ui.mouseIn(r);
  if (selected) ui.fill(r, ui.c.accent.withA(0.16f), 4 * s);
  else if (on) ui.fill(r, ui.c.hover, 4 * s);
  if (over) *over = on;
  return click;
}

// signatures of the long lists (FNV-1a over what decides the row heights)
struct RowSig {
  uint64_t h = 1469598103934665603ULL;
  RowSig& add(const void* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= static_cast<const unsigned char*>(p)[i]; h *= 1099511628211ULL; } return *this; }
  RowSig& v(double x) { return add(&x, sizeof x); }
  RowSig& v(const string& x) { return add(x.data(), x.size()).v(double(x.size())); }
};
// the first row of a measured list that reaches into the view: rel = viewTop - listTop
size_t firstRow(const vector<float>& top, float rel) {
  if (top.size() < 2) return 0;
  auto it = std::upper_bound(top.begin(), top.end() - 1, rel);
  return it == top.begin() ? 0 : size_t(it - top.begin()) - 1;
}
// file existence for the lists: a stat per row per frame is too much for a few hundred PDFs on a network share
bool rdExists(ReaderState& R, double now, const string& path) {
  auto it = R.exists.find(path);
  if (it != R.exists.end() && now - it->second.second < 4.0) return it->second.first;
  bool ok = fileExistsU(path);
  if (R.exists.size() > 4096) R.exists.clear();
  R.exists[path] = {ok, now};
  return ok;
}
}  // namespace

static ReaderState& RS(App& a) {
  if (!a.rd) a.rd = std::make_shared<ReaderState>();
  return *a.rd;
}

const PdfItem* App::readerItemForRecord(int rec) const {
  if (rec < 0 || !hasCorpus() || size_t(rec) >= P->corpus.recs.size()) return nullptr;
  string key = PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]);
  for (auto& it : P->library.items) if (!it.recordKey.empty() && it.recordKey == key) return &it;
  return nullptr;
}

// ------------------------------------------------------------------ attach
vector<string> App::readerAttach(const vector<string>& paths, int linkRec, bool open) {
  vector<string> ids;
  int added = 0, linked = 0;
  for (auto& p : paths) {
    if (lower(fileExt(p)) != "pdf") continue;
    PdfItem& it = P->library.add(p);  // the existing item when the path is already attached
    if (it.added.empty()) it.added = nowIso();
    if (it.size <= 0) it.size = fileSizeU(p);
    if (it.title.empty()) it.title = PdfLibrary::fileTitle(p);
    added++;
    if (linkRec >= 0 && hasCorpus() && size_t(linkRec) < P->corpus.recs.size()) { P->library.link(it, P->corpus.recs[size_t(linkRec)]); linked++; }
    ids.push_back(it.id);
    P->dirty = true;
  }
  if (ids.empty()) return ids;
  // identify the unlinked ones from the file itself (DOI or title on the first page) — on the engine thread
  for (auto& id : ids) { const PdfItem* it = P->library.find(id); if (it && it->recordKey.empty()) readerIdentify(id); }
  if (!scriptMode) ui.toast(plural(added, "PDF") + " attached", linked ? "Linked to the selected record." : hasCorpus() ? "Matching to the records by DOI and title\xE2\x80\xA6" : "Import the bibliographic records to link papers to them.", 1, 4);
  if (workspace != WS_BIBLIOGRAPHY) setWorkspace(WS_BIBLIOGRAPHY);
  writerOpen = false;
  lastBibliographyRoute = BR_REVIEW;
  page = PG_READ;
  papersOpen = !readerOpen;
  settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  if (open) openReader(ids[0]);
  return ids;
}

void App::readerAttachDialog(int linkRec) {
  auto v = openFileDialog(hwnd, linkRec >= 0 ? "Attach the PDF of this paper" : "Attach PDFs", {{"PDF files", "*.pdf"}, {"All files", "*.*"}}, linkRec < 0);
  if (v.empty()) return;
  readerAttach(v, linkRec, v.size() == 1);
}

// reads the first pages' text on the engine thread and links the item to a record when the DOI or the title matches
void App::readerIdentify(const string& itemId) {
  PdfItem* it = P->library.find(itemId);
  if (!it) return;
  ReaderState& R = RS(*this);
  readerEnsureEngine();
  string path = it->path;
  R.worker.submit(30, [this, itemId, path](pdf::Worker& w) {
    pdf::Document d;
    string err;
    auto out = std::make_shared<std::tuple<string, string, string, int, int, string>>();  // doi, title, author, year, pages, error
    if (d.open(path, &err)) {
      pdf::TextPage tp;
      string text;
      for (int pg = 0; pg < std::min(2, d.info().pages); pg++) { pdf::TextPage t2; if (d.text(pg, t2)) { text += t2.text + "\n"; if (pg == 0) tp = std::move(t2); } }
      string doi = PdfLibrary::guessDoi(text);
      string title = !d.info().title.empty() && d.info().title.size() > 8 && !contains(lower(d.info().title), ".doc") && !contains(lower(d.info().title), "untitled") ? d.info().title : PdfLibrary::guessTitle(tp);
      *out = {doi, title, d.info().author, 0, d.info().pages, ""};
    } else *out = {"", "", "", 0, 0, err};
    post([this, itemId, out]() {
      PdfItem* item = P->library.find(itemId);
      if (!item) return;
      const auto& [doi, title, author, year, pages, e] = *out;
      (void)year;
      if (pages > 0) item->pages = pages;
      if (!e.empty()) return;
      bool changed = false;
      if (item->recordKey.empty() && hasCorpus()) {
        int rec = PdfLibrary::matchRecord(P->corpus, doi, title, 0);
        if (rec < 0 && !item->title.empty()) rec = PdfLibrary::matchRecord(P->corpus, "", item->title, 0);
        if (rec >= 0) { P->library.link(*item, P->corpus.recs[size_t(rec)]); changed = true; if (!scriptMode && !readerOpen) ui.toast("PDF linked", truncate(item->title, 70), 1, 3); }
      }
      if (item->recordKey.empty()) {
        if (!doi.empty() && item->doi.empty()) { item->doi = doi; changed = true; }
        if (!title.empty() && (item->title.empty() || item->title == PdfLibrary::fileTitle(item->path))) { item->title = title; changed = true; }
        if (!author.empty() && item->author.empty()) { item->author = author; changed = true; }
      }
      if (changed) P->dirty = true;
      needFrame = true;
    });
  });
}

void App::readerRelocate(const string& itemId) {
  PdfItem* it = P->library.find(itemId);
  if (!it) return;
  auto v = openFileDialog(hwnd, "Locate " + fileName(it->path), {{"PDF files", "*.pdf"}, {"All files", "*.*"}}, false);
  if (v.empty()) return;
  it = P->library.find(itemId);
  if (!it) return;
  it->path = v[0];
  it->size = fileSizeU(v[0]);
  P->dirty = true;
  if (rd && rd->doc.itemId == itemId) { bool wasOpen = readerOpen; readerResetDoc(); if (wasOpen) openReader(itemId); }
}

void App::readerDetach(const string& itemId) {
  PdfItem* it = P->library.find(itemId);
  if (!it) return;
  int n = it->annotCount();
  if (n > 0 || !it->notes.empty()) {
    if (!scriptMode) {
      std::wstring msg = widen("Detach \"" + truncate(itemTitle(*it), 60) + "\" from the project?\n\nIts " + plural(n, "highlight") + " and notes are removed from the project. The PDF file itself is not touched.");
      if (MessageBoxW(hwnd, msg.c_str(), L"VOSStudio", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
  }
  if (readerOpen && rd && rd->doc.itemId == itemId) closeReader();
  if (rd && rd->doc.itemId == itemId) readerResetDoc();
  P->library.remove(itemId);
  P->dirty = true;
}

// ------------------------------------------------------------------ save annotations into the PDF (engine thread)
void App::readerSaveIntoPdf(const string& itemId) {
  PdfItem* it = P->library.find(itemId);
  if (!it || it->annots.empty()) return;
  ReaderState& R = RS(*this);
  readerEnsureEngine();
  vector<pdf::FileAnnot> fa;
  for (auto& a : it->annots) {
    pdf::FileAnnot f;
    f.subtype = a.kind == 0 ? 9 : a.kind == 1 ? 10 : a.kind == 2 ? 12 : a.kind == 3 ? 1 : a.kind == 5 ? 15 : 5;
    f.quads = a.quads;
    f.strokes = a.strokes;
    f.strokeWidth = a.strokeWidth;
    f.rect = a.kind >= 3 ? a.rect : pdf::Box();
    if (f.rect.empty() && !a.quads.empty()) { f.rect = a.quads[0]; for (auto& q : a.quads) { f.rect.x0 = std::min(f.rect.x0, q.x0); f.rect.y0 = std::min(f.rect.y0, q.y0); f.rect.x1 = std::max(f.rect.x1, q.x1); f.rect.y1 = std::max(f.rect.y1, q.y1); } }
    f.color = a.color;
    string c = a.code != Code::None ? string("[") + codeName(a.code) + "] " : !a.tag.empty() ? "[" + a.tag + "] " : "";
    f.contents = c + a.note;
    f.author = "VOSStudio";
    f.name = "vs:" + a.id;
    fa.push_back(f);
  }
  // pages of each annotation, in the same order
  vector<int> pages;
  for (auto& a : it->annots) pages.push_back(a.page);
  string path = it->path;
  const bool wasOpen = rd && rd->doc.itemId == itemId && rd->doc.id >= 0;
  const int docId = wasOpen ? rd->doc.id : -1;
  ui.toast("Saving into the PDF\xE2\x80\xA6", fileName(path), 0, 3);
  R.worker.submit(-10, [this, itemId, path, fa, pages, docId](pdf::Worker& w) {
    string err;
    bool ok = false;
    string tmp = path + ".vs-tmp.pdf";
    {
      // the open document (if any) is closed first: the file is replaced underneath it
      if (docId >= 0 && w.hasDoc(docId)) w.doc(docId).close();
      pdf::Document d;
      if (!d.open(path, &err)) { ok = false; }
      else {
        std::map<int, vector<pdf::FileAnnot>> perPage;
        for (size_t i = 0; i < fa.size(); i++) perPage[pages[i]].push_back(fa[i]);
        // ours from an earlier save go first (same /NM prefix), then the current set
        for (int pg = 0; pg < d.info().pages; pg++) d.removeAnnots(pg, [](const pdf::FileAnnot& f) { return startsWith(f.name, "vs:"); });
        ok = true;
        for (auto& kv : perPage) if (!d.writeAnnots(kv.first, kv.second, &err)) { ok = false; break; }
        if (ok) ok = d.saveAs(tmp, &err);
        d.close();
        if (ok) { ok = renameFileU(tmp, path); if (!ok) err = "The file could not be replaced (is it open in another program?)"; }
        if (!ok) removeFileU(tmp);
      }
      if (docId >= 0 && w.hasDoc(docId)) { string e2; w.doc(docId).open(path, &e2); }
    }
    post([this, itemId, ok, err, docId]() {
      PdfItem* item = P->library.find(itemId);
      if (ok && item) { for (auto& a : item->annots) a.inFile = true; P->dirty = true; }
      if (rd && rd->doc.id == docId && docId >= 0) {  // the tiles show the file's own rendering of the annotations now
        rd->tiles.clear(); rd->tileBytes = 0; rd->tileReq.clear(); rd->tileK = 0; rd->tileKPrev = 0;
        for (auto& p : rd->doc.pages) { p.preview.reset(); p.previewReq = false; }
      }
      if (ok) ui.toast("Saved into the PDF", "Highlights, notes, areas, and freehand drawings are in the file now (any PDF viewer shows them).", 1, 4);
      else ui.toast("Could not save into the PDF", err, 3, 7);
      needFrame = true;
    });
  });
}

// ------------------------------------------------------------------ exports
void App::readerExportNotes(const string& itemId) {
  string md, name;
  if (!itemId.empty()) {
    const PdfItem* it = P->library.find(itemId);
    if (!it) return;
    int rec = P->library.recordIndex(P->corpus, *it);
    md = P->library.exportMarkdown(*it, rec >= 0 ? &P->corpus.recs[size_t(rec)] : nullptr);
    name = truncate(itemTitle(*it), 60);
    for (auto& ch : name) if (strchr("\\/:*?\"<>|", ch)) ch = ' ';
    name = trim(name) + " - notes.md";
  } else {
    md = P->library.exportMarkdownAll(hasCorpus() ? &P->corpus : nullptr);
    name = "reading-notes.md";
  }
  string path = saveFileDialog(hwnd, "Export notes (Markdown)", {{"Markdown", "*.md"}, {"Text", "*.txt"}}, name, "md");
  if (path.empty()) return;
  if (writeFileU(path, md)) { ui.toast("Notes exported", fileName(path), 1); revealInExplorer(path); }
  else ui.toast("Could not write the file", path, 3);
}

void App::readerExportCoding() {
  string csv = P->library.codingCsv(hasCorpus() ? &P->corpus : nullptr);
  string path = saveFileDialog(hwnd, "Export the coding matrix (CSV)", {{"CSV", "*.csv"}}, "coding-matrix.csv", "csv");
  if (path.empty()) return;
  if (writeFileU(path, csv)) { ui.toast("Coding matrix exported", "One row per paper: reading status, coded text highlights, area selections, freehand drawings, and notes.", 1, 5); revealInExplorer(path); }
  else ui.toast("Could not write the file", path, 3);
}

// ------------------------------------------------------------------ the Read page (left panel)
void App::pageRead(Lay& L) {
  const float s = ui.s;
  ReaderState& R = RS(*this);
  PdfLibrary& lib = P->library;
  const BibliographyRoute libraryRoute = readerOpen && bibliographyReaderReturnToPapers ? bibliographyReaderReturnRoute : lastBibliographyRoute;
  LibraryStats st = lib.stats([&](const string& p) { return rdExists(R, ui.time, p); });

  // ---- persistent bibliography navigation: the same record filters remain available beside the table and reader
  sectionTitle(L, "Library", "Browse records and attached PDFs together. Collections, tags and saved views are stored in this project.");
  {
    Rect r = L.row(28 * s);
    auto c2 = cols(r, 2, 6 * s);
    if (ui.button(c2[0], "All papers", papersStatusFilter < 0 && papersPdfFilter < 0 && papersCollectionFilter.empty() && papersTagFilter.empty() && papersFilter.empty() ? BTN_PRIMARY : BTN_NORMAL, "table"))
      applyPaperView(-1, -1, "", "", "", BR_PAPERS);
    if (ui.button(c2[1], "Needs PDF", papersPdfFilter == 0 ? BTN_PRIMARY : BTN_NORMAL, "file"))
      applyPaperView(-1, 0, "", "", "", BR_PAPERS);
    r = L.row(28 * s);
    if (ui.button(r, "Review attached PDFs", papersPdfFilter == 1 ? BTN_PRIMARY : BTN_NORMAL, "book"))
      applyPaperView(-1, 1, papersCollectionFilter, papersTagFilter, papersFilter, BR_REVIEW);
    ui.tip("Show the attached-PDF reading queue, including unlinked files. Collection filters show linked records; Reader and notes stay in this library panel.");
    r = L.row(24 * s);
    int statusChoice = papersStatusFilter + 1;
    if (ui.segmented(r, {"Any", "To read", "Reading", "Read", "Excl."}, statusChoice, "papers:status"))
      applyPaperView(statusChoice - 1, statusChoice > 0 ? 1 : papersPdfFilter, papersCollectionFilter, papersTagFilter, papersFilter, statusChoice > 0 ? BR_REVIEW : libraryRoute);
    r = L.row(24 * s);
    int pdfChoice = papersPdfFilter + 1;
    if (ui.segmented(r, {"Any PDF", "Needs PDF", "Has PDF"}, pdfChoice, "papers:pdf"))
      applyPaperView(papersStatusFilter, pdfChoice - 1, papersCollectionFilter, papersTagFilter, papersFilter, pdfChoice == 1 ? BR_PAPERS : libraryRoute);
    if (papersStatusFilter >= 0 || papersPdfFilter >= 0 || !papersCollectionFilter.empty() || !papersTagFilter.empty() || !papersFilter.empty()) {
      r = L.row(22 * s);
      if (ui.button(r, "Clear library filters", BTN_GHOST, "x")) applyPaperView(-1, -1, "", "", "", libraryRoute);
    }
  }

  sectionTitle(L, "Collections", "Group records without moving or copying PDFs. Use the folder action above the table to add or remove selected papers.");
  const vector<PaperCollection> collectionRows = lib.collections;
  for (const auto& c : collectionRows) {
    Rect r = L.row(28 * s);
    Rect nameR{r.x, r.y, std::max(40 * s, r.w - 28 * s), r.h};
    bool active = papersCollectionFilter == c.id;
    if (ui.button(nameR, c.name, active ? BTN_PRIMARY : BTN_NORMAL, "folder"))
      applyPaperView(papersStatusFilter, papersPdfFilter, active ? "" : c.id, papersTagFilter, papersFilter, libraryRoute);
    if (ui.iconButton({r.r() - 25 * s, r.y + 3 * s, 22 * s, 22 * s}, "x", "Delete collection only; its papers and PDFs stay in the project.")) {
      if (lib.removeCollection(c.id)) {
        P->dirty = true;
        if (papersCollectionFilter == c.id) papersCollectionFilter.clear();
        papersBuiltKey.clear();
        needFrame = true;
      } else ui.toast("Collection is in use", "Remove saved views that use it before deleting the collection.", 2, 5);
    }
  }
  if (papersNewCollectionOpen) {
    Rect r = L.row(28 * s);
    bool submitted = false;
    ui.textInput(r, "papers:newcollection", papersNewCollectionName, "Collection name", &submitted, "folder");
    r = L.row(27 * s);
    auto c2 = cols(r, 2, 6 * s);
    bool create = ui.button(c2[0], "Create", BTN_PRIMARY, "plus");
    bool cancel = ui.button(c2[1], "Cancel", BTN_NORMAL);
    if ((create || submitted) && !trim(papersNewCollectionName).empty()) {
      string id = lib.addCollection(papersNewCollectionName);
      if (id.empty()) ui.toast("Collection not added", "A collection with that name already exists.", 2, 4);
      else { P->dirty = true; papersCollectionFilter = id; papersBuiltKey.clear(); }
      papersNewCollectionOpen = false; papersNewCollectionName.clear(); needFrame = true;
    } else if (cancel) { papersNewCollectionOpen = false; papersNewCollectionName.clear(); }
  } else {
    Rect r = L.row(28 * s);
    if (ui.button(r, "New collection", BTN_NORMAL, "plus")) { papersNewCollectionOpen = true; papersNewCollectionName.clear(); }
  }

  const auto& allTags = lib.allTags();
  if (!allTags.empty()) {
    sectionTitle(L, "Tags and codes", "Tags on linked papers filter records and the queue; unlinked-PDF tags and annotation codes filter the queue.");
    Rect row = L.row(25 * s);
    float cx = row.x;
    for (const string& tag : allTags) {
      float w = std::min(row.w, ui.textW("#" + tag, 10.8f * s, 550) + 20 * s);
      if (cx + w > row.r() && cx > row.x) { row = L.row(25 * s); cx = row.x; }
      bool active = lower(trim(papersTagFilter)) == lower(trim(tag));
      if (ui.button({cx, row.y, w, row.h}, "#" + tag, active ? BTN_PRIMARY : BTN_NORMAL))
        applyPaperView(papersStatusFilter, papersPdfFilter, papersCollectionFilter, active ? "" : tag, papersFilter, libraryRoute);
      cx += w + 4 * s;
    }
  }

  sectionTitle(L, "Saved views", "Save a search, status, PDF, collection and tag combination for reuse.");
  const vector<SavedPaperView> savedViewRows = lib.savedViews;
  for (const auto& view : savedViewRows) {
    Rect r = L.row(28 * s);
    Rect nameR{r.x, r.y, std::max(40 * s, r.w - 28 * s), r.h};
    if (ui.button(nameR, view.name, BTN_NORMAL, "bookmark")) applyPaperView(view.status, view.pdf, view.collectionId, view.tag, view.query, libraryRoute);
    if (ui.iconButton({r.r() - 25 * s, r.y + 3 * s, 22 * s, 22 * s}, "x", "Delete this saved view")) {
      if (lib.removeSavedView(view.id)) { P->dirty = true; papersBuiltKey.clear(); needFrame = true; }
    }
  }
  if (papersNewViewOpen) {
    Rect r = L.row(28 * s);
    bool submitted = false;
    ui.textInput(r, "papers:newview", papersNewViewName, "Saved view name", &submitted, "bookmark");
    r = L.row(27 * s);
    auto c2 = cols(r, 2, 6 * s);
    bool save = ui.button(c2[0], "Save current view", BTN_PRIMARY, "check");
    bool cancel = ui.button(c2[1], "Cancel", BTN_NORMAL);
    if ((save || submitted) && !trim(papersNewViewName).empty()) { saveCurrentPaperView(papersNewViewName); papersNewViewOpen = false; papersNewViewName.clear(); }
    else if (cancel) { papersNewViewOpen = false; papersNewViewName.clear(); }
  } else {
    Rect r = L.row(28 * s);
    if (ui.button(r, "Save current view\xE2\x80\xA6", BTN_NORMAL, "plus")) { papersNewViewOpen = true; papersNewViewName.clear(); }
  }
  L.space(12 * s);

  // ---- header: where the reading stands
  {
    Rect r = L.row(40 * s);
    ui.text({r.x, r.y, r.w, 22 * s}, st.total == 0 ? "PDF reading queue" : plural(st.total, "attached paper"), 14 * s, ui.c.text, AL_LEFT, 600);
    string sub = st.total == 0 ? "Attach the PDFs of the papers you will read" : plural(st.read, "read") + " \xC2\xB7 " + std::to_string(st.reading) + " reading \xC2\xB7 " + std::to_string(st.toRead) + " to read" + (st.excluded ? " \xC2\xB7 " + std::to_string(st.excluded) + " excluded" : string());
    ui.text({r.x, r.y + 22 * s, r.w, 16 * s}, sub, 11.5f * s, ui.c.textDim);
  }
  if (st.total > 0) {
    Rect r = L.row(8 * s);
    int done = st.read + st.excluded;
    ui.fill(r, ui.c.hover, 4 * s);
    ui.fill({r.x, r.y, r.w * float(done) / float(std::max(1, st.total)), r.h}, ui.c.ok, 4 * s);
    ui.fill({r.x + r.w * float(done) / float(std::max(1, st.total)), r.y, r.w * float(st.reading) / float(std::max(1, st.total)), r.h}, ui.c.accent.withA(0.7f));
    L.space(2 * s);
  }
  {
    Rect r = L.row(30 * s);
    auto c2 = cols(r, 2, 6 * s);
    if (ui.button(c2[0], "Attach PDFs\xE2\x80\xA6", BTN_PRIMARY, "plus")) readerAttachDialog(-1);
    ui.tip("Add the PDF files of the papers (or drop them anywhere on the window). They are linked to your records by DOI or title.");
    if (ui.button(c2[1], "Get PDFs\xE2\x80\xA6", BTN_NORMAL, "download", hasCorpus() && !busy())) {
      vector<int> all;
      for (size_t i = 0; i < P->corpus.recs.size(); i++) all.push_back(int(i));
      fetchAsk(all, true);
    }
    ui.tip("Download the open-access copies of the records that have a DOI and no PDF yet (OpenAlex, Unpaywall, arXiv). Papers that are not open, or whose site refuses, are listed below with the reason so you can attach them by hand.");
    if (hasCorpus() && st.total > 0) {
      r = L.row(28 * s);
      if (ui.button(r, "Match to records", BTN_NORMAL, "link")) {
        int n = lib.relinkAll(P->corpus);
        for (auto& it : lib.items) if (it.recordKey.empty()) readerIdentify(it.id);
        ui.toast(plural(n, "paper") + " linked", "By DOI, else by title. The unlinked ones are being read for a DOI\xE2\x80\xA6", 1, 4);
        if (n) P->dirty = true;
      }
      ui.tip("Link every attached PDF to the matching bibliographic record (DOI, else normalised title).");
    }
  }
  drawFetchSection(L);
  if (st.total == 0) {
    L.space(6 * s);
    ui.textWrap(L.row(120 * s), "The Read stage turns the map into a reading list: attach the PDFs of the papers in the clusters that matter, read them here, highlight and code passages (aim, method, finding, theory, gap), keep a status per paper and export the notes or the coding matrix. Quotes and citations go straight into the report in Write.", 12 * s, ui.c.textDim);
    if (hasCorpus()) {
      sectionTitle(L, "From the records", "The papers table (Data \xE2\x80\xBA Papers) has a PDF column: attach a file to one record there, or drop PDFs onto the window.", "");
      Rect r = L.row(30 * s);
      if (ui.button(r, "Open the papers table", BTN_NORMAL, "table")) { openPapers(); }
    }
    return;
  }
  // ---- filters
  {
    Rect r = L.row(28 * s);
    ui.textInput(r, "rdlibq", R.libQuery, "Search title, author, tag, note\xE2\x80\xA6", nullptr, "search");
    r = L.row(26 * s);
    vector<string> opts = {"All", "To read", "Reading", "Read", "Excl."};
    ui.segmented(r, opts, R.libFilter, "rdlibf");
  }
  // ---- the list
  string q = lower(trim(R.libQuery));
  vector<string> paperQueryTokens;
  for (auto& token : splitAny(lower(papersFilter), " \t")) if (!token.empty()) paperQueryTokens.push_back(token);
  const PaperCollection* activeCollection = papersCollectionFilter.empty() ? nullptr : lib.collection(papersCollectionFilter);
  const string activeTag = lower(trim(papersTagFilter));
  vector<const PdfItem*> rows;
  for (auto& it : lib.items) {
    if (R.libFilter > 0 && int(it.status) != R.libFilter - 1) continue;
    if (papersStatusFilter >= 0 && int(it.status) != papersStatusFilter) continue;
    if (papersPdfFilter == 0) continue;  // this view is corpus records without a linked PDF
    if (!papersCollectionFilter.empty() && (!activeCollection || !activeCollection->contains(it.recordKey))) continue;
    string hay = lower(it.title + " " + it.author + " " + it.doi + " " + it.notes + " " + fileName(it.path));
    vector<string> itemTags = it.recordKey.empty() ? it.tags : lib.tagsForRecord(it.recordKey);
    for (auto& t : itemTags) hay += " " + lower(t);
    for (auto& a : it.annots) hay += " " + lower(a.text) + " " + lower(a.note) + " " + lower(a.tag);
    if (!q.empty() && !contains(hay, q)) continue;
    bool queryMatches = true;
    for (auto& token : paperQueryTokens) if (!contains(hay, token)) { queryMatches = false; break; }
    if (!queryMatches) continue;
    if (!activeTag.empty()) {
      bool found = false;
      for (auto& t : itemTags) if (lower(trim(t)) == activeTag) { found = true; break; }
      if (!found) for (auto& a : it.annots) if (lower(trim(a.tag)) == activeTag) { found = true; break; }
      if (!found) continue;
    }
    rows.push_back(&it);
  }
  std::stable_sort(rows.begin(), rows.end(), [&](const PdfItem* a, const PdfItem* b) {
    if (R.libSort == 1) return lower(itemTitle(*a)) < lower(itemTitle(*b));
    if (R.libSort == 2) return a->year > b->year;
    if (R.libSort == 3) return int(a->status) < int(b->status);
    return a->added > b->added;  // newest first
  });
  {
    Rect r = L.row(22 * s);
    ui.text({r.x, r.y, r.w * 0.5f, r.h}, plural(long(rows.size()), "paper") + (q.empty() && R.libFilter == 0 ? "" : " shown"), 11.5f * s, ui.c.textDim, AL_LEFT, 600);
    vector<string> sorts = {"Recent", "Title", "Year", "Status"};
    Rect sr{r.r() - 96 * s, r.y - 3 * s, 96 * s, 26 * s};
    ui.combo(sr, "rdlibsort", sorts, R.libSort);
  }
  const PdfItem* nextUp = nullptr;
  for (auto* it : rows) if (it->status == ReadStatus::Reading) { nextUp = it; break; }
  if (!nextUp) for (auto* it : rows) if (it->status == ReadStatus::ToRead) { nextUp = it; break; }
  const float rh = 58 * s;
  const Rect vis = ui.clipRect();  // only the rows in view are drawn (the list can hold hundreds of papers)
  for (const PdfItem* it : rows) {
    Rect r = L.row(rh);
    if (r.b() < vis.y - 2 * s || r.y > vis.b() + 2 * s) continue;
    const bool cur = readerOpen && rd && rd->doc.itemId == it->id;
    bool click = rowWithStrip(ui, r, "rdlib:" + it->id, cur, 32 * s, nullptr);
    bool exists = rdExists(R, ui.time, it->path);
    int rec = lib.recordIndex(P->corpus, *it);
    const Record* record = rec >= 0 ? &P->corpus.recs[size_t(rec)] : nullptr;
    Color sc = statusColor(ui, it->status);
    ui.circle(r.x + 10 * s, r.y + 15 * s, 4 * s, sc, it->status != ReadStatus::ToRead, 1.5f);
    float tx = r.x + 22 * s;
    string title = itemTitle(*it);
    ui.text({tx, r.y + 5 * s, r.w - 22 * s - 30 * s, 18 * s}, title, 12.5f * s, it->status == ReadStatus::Excluded ? ui.c.textFaint : ui.c.text, AL_LEFT, it == nextUp ? 600 : 500);
    string sub = itemSub(*it, record);
    if (sub.empty()) sub = fileName(it->path);
    ui.text({tx, r.y + 23 * s, r.w - 22 * s - 30 * s, 16 * s}, sub, 11 * s, ui.c.textDim);
    // third line: counts and flags
    string info;
    int nh = it->annotCount(0x7), nn = it->annotCount(0x8);
    if (nh) info += plural(nh, "highlight");
    if (nn) info += (info.empty() ? "" : " \xC2\xB7 ") + plural(nn, "note");
    if (!it->notes.empty()) info += (info.empty() ? "" : " \xC2\xB7 ") + string("notes");
    vector<string> shownTags = it->recordKey.empty() ? it->tags : lib.tagsForRecord(it->recordKey);
    for (auto& t : shownTags) info += (info.empty() ? "" : " \xC2\xB7 ") + string("#") + t;
    if (it->rating) { string stars; for (int i = 0; i < it->rating; i++) stars += "\xE2\x98\x85"; info = stars + (info.empty() ? "" : "  " + info); }
    if (!record && hasCorpus()) info = (info.empty() ? "" : info + " \xC2\xB7 ") + "not linked";
    if (it == nextUp && it->status != ReadStatus::Reading) info = (info.empty() ? "" : info + " \xC2\xB7 ") + "next up";
    if (!exists) info = "file missing" + (info.empty() ? "" : " \xC2\xB7 " + info);
    ui.text({tx, r.y + 39 * s, r.w - 22 * s - 30 * s, 14 * s}, info, 10.5f * s, !exists ? ui.c.danger : it == nextUp ? ui.c.accent : ui.c.textFaint);
    // the row's menu
    Rect mb{r.r() - 26 * s, r.y + 4 * s, 22 * s, 22 * s};
    if (ui.iconButton(mb, "menu", "Status, link, export, detach\xE2\x80\xA6")) { R.libMenuItem = it->id; R.libMenuX = mb.x; R.libMenuY = mb.b(); ui.openPopup("rdlibmenu"); }
    if (click) { if (exists) openReader(it->id); else { R.libMenuItem = it->id; R.libMenuX = r.x + 40 * s; R.libMenuY = r.y + 20 * s; ui.openPopup("rdlibmenu"); } }
    if (ui.in.pressed[1] && r.has(ui.in.mx, ui.in.my) && !ui.anyPopup()) { R.libMenuItem = it->id; R.libMenuX = ui.in.mx; R.libMenuY = ui.in.my; ui.openPopup("rdlibmenu"); }  // right-click
  }
  if (ui.isPopupOpen("rdlibmenu")) drawReaderLibraryMenu();
  // ---- exports
  sectionTitle(L, "Synthesis", "What the reading produced: the notes of every paper as one Markdown file, and the coding matrix (one row per paper, one column per code) for the analysis.", "");
  {
    Rect r = L.row(30 * s);
    auto c2 = cols(r, 2, 6 * s);
    if (ui.button(c2[0], "Notes (Markdown)", BTN_NORMAL, "download", st.highlights + st.areas + st.drawings + st.notes + st.withNotes > 0)) readerExportNotes("");
    if (ui.button(c2[1], "Coding matrix (CSV)", BTN_NORMAL, "table", st.total > 0)) readerExportCoding();
  }
  if (st.linked < st.total && hasCorpus()) {
    Rect r = L.row(18 * s);
    ui.text(r, std::to_string(st.total - st.linked) + " not linked to a record: open the paper \xE2\x80\xBA Info to link it by hand.", 11 * s, ui.c.textFaint);
  }
  if (st.missing > 0) {
    Rect r = L.row(18 * s);
    ui.text(r, plural(st.missing, "file") + " missing: right-click the paper \xE2\x80\xBA Locate the file.", 11 * s, ui.c.danger);
  }
}

void App::drawReaderLibraryMenu() {
  const float s = ui.s;
  ui.overlay([this, s]() {
    ReaderState& r = RS(*this);
    PdfItem* it = P->library.find(r.libMenuItem);
    if (!it) { ui.closePopup(); return; }
    struct It { string label, key; bool enabled = true, danger = false; };
    vector<It> items;
    auto add = [&](const string& l, const string& k, bool en = true, bool danger = false) { items.push_back({l, k, en, danger}); };
    auto sep = [&]() { items.push_back({"-", "", false, false}); };
    bool exists = rdExists(r, ui.time, it->path);
    add("Open", "open", exists);
    sep();
    for (int i = 0; i < 4; i++) add(string(int(it->status) == i ? "\xE2\x97\x8F " : "\xE2\x97\x8B ") + kStatusNames[i], "st" + std::to_string(i));
    sep();
    add("Export notes (Markdown)", "exportmd", it->annotCount() > 0 || !it->notes.empty());
    add("Save annotations into the PDF file", "savepdf", exists && it->annotCount() > 0);
    add("Show in Explorer", "reveal", exists);
    add("Locate the file\xE2\x80\xA6", "relocate");
    sep();
    add("Detach from the project", "detach", true, true);
    float hgt = 6 * s;
    for (auto& i : items) hgt += i.label[0] == '-' ? 7 * s : 27 * s;
    Rect pr{r.libMenuX, r.libMenuY, 250 * s, hgt};
    if (pr.r() > float(g.W) - 8 * s) pr.x = float(g.W) - 8 * s - pr.w;
    if (pr.b() > float(g.H) - 8 * s) pr.y = std::max(8 * s, float(g.H) - 8 * s - pr.h);
    ui.shadow(pr, 8 * s);
    ui.fill(pr, ui.c.panel2, 8 * s);
    ui.stroke(pr, ui.c.border, 8 * s);
    ui.popupRect(pr);
    float yy = pr.y + 3 * s;
    for (auto& i : items) {
      if (i.label[0] == '-') { ui.line(pr.x + 8 * s, yy + 3 * s, pr.r() - 8 * s, yy + 3 * s, ui.c.border, 1); yy += 7 * s; continue; }
      Rect rr{pr.x + 4 * s, yy, pr.w - 8 * s, 26 * s};
      bool click = i.enabled && ui.listRow(rr, "rdlibmenu:" + i.key, false);
      ui.text({rr.x + 10 * s, rr.y, rr.w - 14 * s, rr.h}, i.label, 12.5f * s, !i.enabled ? ui.c.textFaint : i.danger ? ui.c.danger : ui.c.text);
      yy += 27 * s;
      if (!click) continue;
      ui.closePopup();
      const string key = i.key, id = it->id;
      if (key == "open") openReader(id);
      else if (startsWith(key, "st")) { it->status = ReadStatus(atoi(key.c_str() + 2)); P->library.touchOrganization(); P->dirty = true; }
      else if (key == "exportmd") readerExportNotes(id);
      else if (key == "savepdf") readerSaveIntoPdf(id);
      else if (key == "reveal") revealInExplorer(it->path);
      else if (key == "relocate") readerRelocate(id);
      else if (key == "detach") readerDetach(id);
      return;
    }
  });
}

// ------------------------------------------------------------------ Get PDF on the Read page: the run, the papers without a file
void App::drawFetchSection(Lay& L) {
  const float s = ui.s;
  // the run in progress
  if (fetchRun && busy()) {
    int total, done, attached;
    string current;
    bool finished;
    { std::lock_guard<std::mutex> lk(fetchRun->mu); total = fetchRun->total; done = fetchRun->done; attached = fetchRun->attached; current = fetchRun->current; finished = fetchRun->finished; }
    if (!finished) {
      Rect box = L.row(64 * s);
      ui.fill(box, ui.c.accent.withA(0.08f), 8 * s);
      ui.stroke(box, ui.c.accent.withA(0.25f), 8 * s);
      Rect in = inset(box, 10 * s, 8 * s);
      ui.text({in.x, in.y, in.w - 60 * s, 18 * s}, (total == 1 ? "Getting the PDF" : "Getting PDFs") + string(" \xC2\xB7 ") + std::to_string(done) + "/" + std::to_string(total) + (attached ? " \xC2\xB7 " + std::to_string(attached) + " attached" : ""), 12.5f * s, ui.c.text, AL_LEFT, 600);
      if (ui.button({in.r() - 56 * s, in.y - 2 * s, 56 * s, 22 * s}, "Stop") && job) job->cancel = true;
      ui.progress({in.x, in.y + 22 * s, in.w, 5 * s}, job ? job->progress.load() : 0.0, ui.c.accent);
      ui.text({in.x, in.y + 30 * s, in.w, 16 * s}, current, 11 * s, ui.c.textDim);
      L.space(2 * s);
    }
  }
  if (!hasCorpus()) return;
  // the papers whose fetch ended without a file: the reason, and the ways to get the file by hand
  vector<int> need = fetchNeedsFile();
  if (need.empty()) return;
  const bool haveKey = !openAlexKey().empty();
  Rect hr = L.row(26 * s);
  ui.header(hr, "Without a PDF", fetchListOpen, std::to_string(need.size()));
  if (!fetchListOpen) return;
  ui.textWrap(L.row(30 * s), "Click a paper for the reason and the links (DOI page, open-access link); attach the file by hand or drop it on the window.", 11 * s, ui.c.textDim);
  int cachedNoKey = 0;
  const float rh = 46 * s;
  const Rect vis = ui.clipRect();
  for (int rec : need) {
    const Record& r = P->corpus.recs[size_t(rec)];
    auto fit = P->library.fetch.find(PdfLibrary::keyOf(r));
    if (fit == P->library.fetch.end()) continue;
    const oa::FetchStatus& fs = fit->second;
    if (fs.state == oa::FetchState::Refused && fs.cachedAvailable) cachedNoKey++;
    Rect row = L.row(rh);
    if (row.b() < vis.y - 2 * s || row.y > vis.b() + 2 * s) continue;
    bool click = rowWithStrip(ui, row, "fetchneed:" + std::to_string(rec), false, 32 * s, nullptr);
    Color sc = fs.state == oa::FetchState::Refused ? ui.c.accent : fs.state == oa::FetchState::Failed ? ui.c.danger : ui.c.textFaint;
    ui.icon(fs.state == oa::FetchState::Closed ? "shield" : fs.state == oa::FetchState::NoDoi ? "x" : "warn", row.x + 12 * s, row.y + 14 * s, 13 * s, sc);
    float tx = row.x + 26 * s;
    ui.text({tx, row.y + 4 * s, row.w - 30 * s, 18 * s}, r.title.empty() ? r.doi : r.title, 12.5f * s, ui.c.text, AL_LEFT, 500);
    string label = fs.state == oa::FetchState::Closed ? "Not open access" : fs.state == oa::FetchState::Refused ? (fs.cachedAvailable && !haveKey ? "Open, site refused \xE2\x80\x94 a free OpenAlex key would fetch it" : "Open, but the site refused") : fs.state == oa::FetchState::Budget ? "Waiting for OpenAlex's daily budget" : fs.state == oa::FetchState::NoDoi ? "No DOI" : "Lookup failed";
    if (fs.state == oa::FetchState::Refused && !fs.detail.empty()) label += " \xC2\xB7 " + fs.detail;
    ui.text({tx, row.y + 23 * s, row.w - 30 * s, 16 * s}, label, 10.5f * s, sc);
    if (click) fetchRecordMenu(rec, row.x + 40 * s, row.y + 24 * s);
    if (ui.in.pressed[1] && row.has(ui.in.mx, ui.in.my) && !ui.anyPopup()) fetchRecordMenu(rec, ui.in.mx, ui.in.my);
  }
  Rect br = L.row(28 * s);
  auto c2 = cols(br, 2, 6 * s);
  if (ui.button(c2[0], "Try again", BTN_NORMAL, "refresh", !busy())) fetchAsk(need, true);
  ui.tip("Fetch these again (after adding an OpenAlex key, or later in the day when the budget or a site was the problem).");
  if (!haveKey && cachedNoKey) {
    if (ui.button(c2[1], "Add a free key\xE2\x80\xA6", BTN_NORMAL, "settings")) { openSettings(); setTab = 1; }
    ui.tip("OpenAlex has a copy of " + plural(cachedNoKey, "paper") + " of these. With a free personal API key they are fetched from OpenAlex directly.");
  } else if (ui.button(c2[1], "Clear the list", BTN_NORMAL, "x")) {
    for (int rec : need) P->library.fetch.erase(PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]));
    P->dirty = true;
    fetchGen++;
  }
  L.space(4 * s);
}

// ------------------------------------------------------------------ the reader's side pane
void App::drawReaderPane(const Rect& r) {
  ReaderState& R = RS(*this);
  const float s = ui.s;
  PdfItem* item = readerItem();
  if (!item) return;
  ui.fill(r, ui.c.panel);
  ui.line(r.x + 0.5f, r.y, r.x + 0.5f, r.b(), ui.c.border);
  const int rec = readerRecord();
  const Record* record = rec >= 0 ? &P->corpus.recs[size_t(rec)] : nullptr;
  Rect head{r.x + 1, r.y, r.w - 1, 34 * s};
  const char* titles[] = {"", "Notes", "Highlights", "Find", "Outline", "Paper"};
  ui.text({head.x + 14 * s, head.y, head.w - 60 * s, head.h}, titles[clampv(R.pane, 0, 5)], 12.5f * s, ui.c.text, AL_LEFT, 650);
  if (ui.iconButton({head.r() - 30 * s, head.y + 4 * s, 26 * s, 26 * s}, "x", "Hide the pane")) { R.pane = 0; return; }
  Rect content{r.x + 1, head.b(), r.w - 1, r.h - head.h};
  const float pad = 12 * s;
  Rect inner{content.x + pad, content.y, content.w - 2 * pad, content.h};
  auto paperTagsForItem = [&]() {
    if (item->recordKey.empty()) return item->tags;
    if (R.paperTagCacheKey != item->recordKey || R.paperTagCacheVersion != P->library.organizationVersion) {
      R.paperTagCacheKey = item->recordKey;
      R.paperTagCacheVersion = P->library.organizationVersion;
      R.paperTagCache = P->library.tagsForRecord(item->recordKey);
    }
    return R.paperTagCache;
  };

  if (R.pane == 1) {  // ---- Notes: status, rating, tags, free notes
    ui.beginScroll("rdnotes", content);
    float yy = content.y + 6 * s - ui.scrollY();
    const float y0 = yy;
    auto row = [&](float h) { Rect rr{inner.x, yy, inner.w, h}; yy += h + 6 * s; return rr; };
    auto lbl = [&](const string& t) { ui.text(row(18 * s), t, 11.5f * s, ui.c.textDim, AL_LEFT, 600); };
    lbl("Status");
    { int sel = int(item->status); if (ui.segmented(row(26 * s), {"To read", "Reading", "Read", "Excl."}, sel, "rdst")) { item->status = ReadStatus(sel); P->library.touchOrganization(); P->dirty = true; } }
    lbl("Relevance");
    {
      Rect rr = row(24 * s);
      for (int i = 1; i <= 5; i++) {
        Rect b{rr.x + (i - 1) * 24 * s, rr.y, 24 * s, 24 * s};
        uint64_t idv = ui.id("rdstar" + std::to_string(i));
        bool hov = false;
        if (ui.behave(idv, b, &hov)) { item->rating = item->rating == i ? 0 : i; P->dirty = true; }
        bool on = i <= item->rating;
        ui.text(b, on ? "\xE2\x98\x85" : "\xE2\x98\x86", 16 * s, on ? Color::hex(0xF59E0B) : hov ? ui.c.text : ui.c.textFaint, AL_CENTER);
      }
      ui.text({rr.x + 126 * s, rr.y, rr.w - 126 * s, rr.h}, item->rating == 0 ? "not rated" : item->rating >= 5 ? "core paper" : item->rating >= 4 ? "important" : item->rating >= 3 ? "relevant" : item->rating >= 2 ? "marginal" : "not relevant", 11 * s, ui.c.textFaint);
    }
    lbl("Tags (comma-separated)");
    {
      vector<string> tagsNow = paperTagsForItem();
      string tags;
      for (size_t i = 0; i < tagsNow.size(); i++) tags += (i ? ", " : "") + tagsNow[i];
      if (!ui.editing() || ui.focus != ui.id("ti:rdtags")) R.tagBuf = tags;
      bool sub = false;
      if (ui.textInput(row(28 * s), "rdtags", R.tagBuf, "e.g. core, method, review", &sub)) {
        vector<string> nt;
        for (auto& t : split(R.tagBuf, ',')) { string x = trim(t); if (!x.empty()) nt.push_back(x); }
        vector<string> before = paperTagsForItem();
        if (nt != before) {
          if (item->recordKey.empty()) { item->tags = nt; P->library.touchOrganization(); }
          else P->library.setTagsForRecord(item->recordKey, nt);  // legacy item tags are merged into one shared paper tag set
          P->dirty = true;
        }
      }
      const auto& all = P->library.allTags();
      if (!all.empty()) {
        float cx = inner.x, cy = yy;
        vector<string> currentTags = paperTagsForItem();
        auto hasCurrent = [&](const string& tag) { for (auto& old : currentTags) if (lower(trim(old)) == lower(trim(tag))) return true; return false; };
        for (auto& t : all) {
          if (hasCurrent(t)) continue;
          float w = ui.textW(t, 11 * s) + 16 * s;
          if (cx + w > inner.r()) { cx = inner.x; cy += 24 * s; }
          Rect ch{cx, cy, w, 20 * s};
          uint64_t idv = ui.id("rdtagadd:" + t);
          bool hov = false;
          if (ui.behave(idv, ch, &hov)) {
            if (item->recordKey.empty()) { item->tags.push_back(t); P->library.touchOrganization(); }
            else { currentTags.push_back(t); P->library.setTagsForRecord(item->recordKey, currentTags); }
            P->dirty = true;
          }
          ui.fill(ch, hov ? ui.c.hover : ui.c.panel2, 10 * s);
          ui.stroke(ch, ui.c.border, 10 * s);
          ui.text(ch, "+ " + t, 11 * s, ui.c.textDim, AL_CENTER);
          cx += w + 6 * s;
        }
        if (cx > inner.x) yy = cy + 26 * s;
      }
    }
    lbl("Reading notes");
    {
      float need = 0;
      Rect ta{inner.x, yy, inner.w, std::max(160 * s, std::min(content.h * 0.6f, R.noteBuf.empty() ? 160 * s : 0))};
      static float lastNeed = 0;
      ta.h = std::max(160 * s, lastNeed + 24 * s);
      bool changed = ui.textArea(ta, "rdnotes", item->notes, "Summary, argument, how it relates to your question, what to cite it for\xE2\x80\xA6", nullptr, &need, 13 * s);
      lastNeed = need;
      if (changed) P->dirty = true;
      yy = ta.b() + 8 * s;
    }
    {
      Rect rr = row(30 * s);
      auto c2 = cols(rr, 2, 6 * s);
      if (ui.button(c2[0], "Export notes", BTN_GHOST, "download", item->annotCount() > 0 || !item->notes.empty())) readerExportNotes(item->id);
      if (ui.button(c2[1], "Cite in Write", BTN_GHOST, "writer", rec >= 0)) readerCite();
    }
    ui.endScroll(yy - y0 + 20 * s);
  } else if (R.pane == 2) {  // ---- Highlights: the annotations of this paper
    vector<string> opts = {"All codes"};
    for (int c = 1; c < int(Code::Count); c++) opts.push_back(codeName(Code(c)));
    opts.push_back("Uncoded");
    Rect fr{inner.x, content.y + 4 * s, inner.w, 26 * s};
    ui.combo(fr, "rdmarkf", opts, R.markFilter);
    Rect list{content.x, fr.b() + 6 * s, content.w, content.b() - fr.b() - 6 * s};
    ui.beginScroll("rdmarks", list);
    float yy = list.y + 2 * s - ui.scrollY();
    const float y0 = yy;
    // the rows shown, and their heights: measured only when the annotations, the filter, the width or the scale
    // changed (the wrapped passage and comment decide the height; measuring every row every frame is what made
    // long lists slow). Then only the rows in view are drawn.
    vector<size_t> shownIdx;
    for (size_t ai = 0; ai < item->annots.size(); ai++) {
      const PdfAnnot& a = item->annots[ai];
      if (R.markFilter > 0 && R.markFilter < int(Code::Count) && a.code != Code(R.markFilter)) continue;
      if (R.markFilter == int(Code::Count) && a.code != Code::None) continue;
      shownIdx.push_back(ai);
    }
    auto quoteOf = [&](const PdfAnnot& a) { return a.kind == 3 ? string() : a.text; };
    auto noteOf = [&](const PdfAnnot& a) { return a.id == R.cardAnnot ? trim(R.noteBuf) : a.note; };
    auto qHeight = [&](const string& quote) { return quote.empty() ? 0.f : std::min(60 * s, ui.textWrap({inner.x + 14 * s, 0, inner.w - 14 * s, 1000}, quote, 12 * s, ui.c.text, 400, false)); };
    auto nHeight = [&](const string& note) { return note.empty() ? 0.f : std::min(80 * s, ui.textWrap({inner.x + 14 * s, 0, inner.w - 14 * s, 1000}, note, 11.5f * s, ui.c.textDim, 400, false)); };
    {
      RowSig sg;
      sg.v(item->id).v(double(shownIdx.size())).v(double(R.markFilter)).v(double(inner.w)).v(double(s));
      for (size_t ai : shownIdx) { const PdfAnnot& a = item->annots[ai]; sg.v(a.id).v(double(a.kind)).v(a.text).v(noteOf(a)); }
      if (R.markRows.sig != sg.h || R.markRows.top.size() != shownIdx.size() + 1) {
        R.markRows.sig = sg.h;
        R.markRows.top.assign(shownIdx.size() + 1, 0);
        float acc = 0;
        for (size_t k = 0; k < shownIdx.size(); k++) {
          const PdfAnnot& a = item->annots[shownIdx[k]];
          float qh = qHeight(quoteOf(a)), nh = nHeight(noteOf(a));
          R.markRows.top[k] = acc;
          acc += 24 * s + (qh ? qh + 4 * s : 0) + (nh ? nh + 4 * s : 0) + 8 * s + 4 * s;
        }
        R.markRows.top[shownIdx.size()] = acc;
      }
    }
    const int shown = int(shownIdx.size());
    // one row per annotation: label, the passage, the comment; comment / quote / delete in the strip on the right.
    // A click goes to the annotation on the page; the comment button opens its card there.
    for (size_t k = firstRow(R.markRows.top, list.y - yy); k < shownIdx.size(); k++) {
      const float ry = yy + R.markRows.top[k];
      if (ry > list.b()) break;
      const PdfAnnot& a = item->annots[shownIdx[k]];
      const bool cur = a.id == R.curAnnot;
      const bool editing = a.id == R.cardAnnot;
      const string note = noteOf(a);
      string quote = quoteOf(a);
      float qh = qHeight(quote);
      float nh = nHeight(note);
      float h = R.markRows.top[k + 1] - R.markRows.top[k] - 4 * s;
      Rect rr{content.x + 4 * s, ry, content.w - 8 * s, h};
      const float stripW = a.kind == 3 ? 50 * s : 72 * s;
      bool over = false;
      bool click = rowWithStrip(ui, rr, "rdmark:" + a.id, cur || editing, stripW, &over);
      if (over) R.hoverAnnot = a.id;
      Color col = rdCodeColor(a.color);
      ui.fill({rr.x + 4 * s, rr.y + 6 * s, 4 * s, rr.h - 12 * s}, col, 2 * s);
      string label = a.kind == 3 ? "Note" : a.code != Code::None ? codeName(a.code) : !a.tag.empty() ? a.tag : a.kind == 1 ? "Underline" : a.kind == 2 ? "Strikeout" : a.kind == 4 ? "Area" : a.kind == 5 ? "Drawing" : "Highlight";
      label += "  \xC2\xB7  p. " + std::to_string(a.page + 1);
      if (a.inFile) label += "  \xC2\xB7  in file";
      ui.text({inner.x + 14 * s, rr.y + 4 * s, inner.w - 14 * s - stripW, 16 * s}, label, 11 * s, a.code != Code::None ? col.mix(ui.c.text, 0.35f) : ui.c.textDim, AL_LEFT, 600);
      float ty = rr.y + 22 * s;
      if (qh) { ui.pushClip({inner.x, ty, inner.w, qh}); ui.textWrap({inner.x + 14 * s, ty, inner.w - 14 * s, qh}, quote, 12 * s, ui.c.text); ui.popClip(); ty += qh + 4 * s; }
      if (nh) { ui.pushClip({inner.x, ty, inner.w, nh}); ui.textWrap({inner.x + 14 * s, ty, inner.w - 14 * s, nh}, note, 11.5f * s, ui.c.textDim); ui.popClip(); ty += nh + 4 * s; }
      // the strip (always there, so that the buttons never move under the pointer)
      {
        float bx = rr.r() - 4 * s;
        bx -= 22 * s;
        if (ui.iconButton({bx, rr.y + 2 * s, 20 * s, 20 * s}, "trash", "Delete")) { readerDeleteAnnot(a.id); ui.endScroll(R.markRows.top.back() + 12 * s); return; }
        bx -= 22 * s;
        if (ui.iconButton({bx, rr.y + 2 * s, 20 * s, 20 * s}, "chat", note.empty() ? "Add a comment" : "Edit the comment", editing)) {
          float y = a.kind >= 3 ? a.rect.y0 : a.quads.empty() ? 0 : a.quads[0].y0;
          readerGoTo(a.page, y, true);
          readerOpenCard(a.id, true);
        }
        if (a.kind != 3) {
          bx -= 22 * s;
          if (ui.iconButton({bx, rr.y + 2 * s, 20 * s, 20 * s}, "quote", "Quote into the writer")) readerQuote(a.text, rec);
        }
      }
      if (click) {
        R.curAnnot = a.id;
        float y = a.kind >= 3 ? a.rect.y0 : a.quads.empty() ? 0 : a.quads[0].y0;
        readerGoTo(a.page, y, true);
      }
    }
    yy += R.markRows.top.back();
    if (shown == 0) {
      ui.textWrap({inner.x, yy + 8 * s, inner.w, 140 * s}, item->annots.empty() ? "Select text on the page: the toolbar that appears codes it (the coloured dots, keys 1\xE2\x80\x93" "7), highlights it (H), underlines it (U) or highlights it with a comment (N). Right-click a page for a free note. Click a highlight to comment on it or change its code. Everything is kept in the project; \xE2\x8B\xAF \xE2\x80\xBA Save into the PDF writes it into the file." : "No highlight with this code.", 12 * s, ui.c.textDim);
      yy += 150 * s;
    }
    ui.endScroll(yy - y0 + 12 * s);
  } else if (R.pane == 3) {  // ---- Find
    float yy = content.y + 6 * s;
    if (R.findFocus) { ui.focusText(ui.id("ti:rdfind"), R.findQ); R.findFocus = false; }
    bool sub = false;
    bool changed = ui.textInput({inner.x, yy, inner.w, 28 * s}, "rdfind", R.findQ, "Find in this PDF", &sub, "search");
    yy += 34 * s;
    {
      Rect rr{inner.x, yy, inner.w, 22 * s};
      auto c2 = cols(rr, 2, 6 * s);
      bool a = R.findCase, b = R.findWord;
      if (ui.checkbox(c2[0], "Match case", a)) { R.findCase = a; changed = true; }
      if (ui.checkbox(c2[1], "Whole words", b)) { R.findWord = b; changed = true; }
      yy += 28 * s;
    }
    static double lastEdit = 0;
    if (changed) lastEdit = ui.time;
    if (sub) { if (R.findQ != R.findLast) readerFindStart(); else readerFindNext(1, false); ui.focus = ui.id("ti:rdfind"); }
    else if (changed && trim(R.findQ).empty()) readerFindStart();
    else if (R.findQ != R.findLast && ui.time - lastEdit > 0.35 && !trim(R.findQ).empty()) readerFindStart();
    else if (R.findQ != R.findLast) ui.animating = true;
    {
      Rect rr{inner.x, yy, inner.w, 24 * s};
      string st = R.findLast.empty() ? "" : R.findRunning ? "Searching\xE2\x80\xA6 " + plural(long(R.hits.size()), "match") : R.hits.empty() ? "No matches" : plural(long(R.hits.size()), "match") + (R.hitCur >= 0 ? "  \xC2\xB7  " + std::to_string(R.hitCur + 1) : "");
      ui.text({rr.x, rr.y, rr.w - 60 * s, rr.h}, st, 11.5f * s, ui.c.textDim);
      if (ui.iconButton({rr.r() - 52 * s, rr.y, 24 * s, 24 * s}, "chev-up", "Previous  (Shift+F3)", false, !R.hits.empty())) readerFindNext(-1, false);
      if (ui.iconButton({rr.r() - 26 * s, rr.y, 24 * s, 24 * s}, "chev-down", "Next  (F3 / Enter)", false, !R.hits.empty())) readerFindNext(1, false);
      yy += 30 * s;
    }
    Rect list{content.x, yy, content.w, content.b() - yy};
    ui.beginScroll("rdhits", list);
    float ly = list.y - ui.scrollY();
    const float rh = 46 * s;
    int lastPage = -1;
    float used = 0;
    for (size_t i = 0; i < R.hits.size() && i < 500; i++) {  // only the rows in view are laid out
      const pdf::Hit& h = R.hits[i];
      if (h.page != lastPage) {
        if (ly + 20 * s >= list.y && ly <= list.b()) ui.text({inner.x, ly + 2 * s, inner.w, 18 * s}, "Page " + std::to_string(h.page + 1), 11 * s, ui.c.textFaint, AL_LEFT, 600);
        ly += 20 * s; used += 20 * s; lastPage = h.page;
      }
      if (ly + rh >= list.y && ly <= list.b()) {
        Rect rr{content.x + 4 * s, ly, content.w - 8 * s, rh - 4 * s};
        bool click = ui.listRow(rr, "rdhit:" + std::to_string(i), int(i) == R.hitCur);
        string md = h.before + "**" + h.match + "**" + h.after;
        ui.pushClip(rr);
        ui.richText({rr.x + 8 * s, rr.y + 3 * s, rr.w - 16 * s, rr.h - 6 * s}, md, 11.5f * s, ui.c.text, 400, true, 1.25f);
        ui.popClip();
        if (click) { R.hitCur = int(i); readerFindNext(0, false); }
      }
      ly += rh;
      used += rh;
    }
    ui.endScroll(used + 12 * s);
  } else if (R.pane == 4) {  // ---- Outline
    ui.beginScroll("rdoutline", content);
    float yy = content.y + 4 * s - ui.scrollY();
    const float y0 = yy;
    const auto& ol = R.doc.info.outline;
    if (ol.empty()) { ui.textWrap({inner.x, yy + 8 * s, inner.w, 80 * s}, R.doc.ready ? "This PDF has no bookmarks." : "", 12 * s, ui.c.textDim); yy += 60 * s; }
    int cur = readerCurrentPage();
    // row heights once per document / width / scale (a book's outline has thousands of entries; measuring each
    // title every frame is what dragged the whole app down), then only the rows in view
    {
      RowSig sg;
      sg.v(double(R.docGen)).v(double(ol.size())).v(double(inner.w)).v(double(s));
      if (R.olRows.sig != sg.h || R.olRows.top.size() != ol.size() + 1) {
        R.olRows.sig = sg.h;
        R.olRows.top.assign(ol.size() + 1, 0);
        float acc = 0;
        for (size_t i = 0; i < ol.size(); i++) {
          const pdf::OutlineItem& o = ol[i];
          float ind = std::min(4, o.depth) * 14 * s;
          R.olRows.top[i] = acc;
          acc += std::min(3, 1 + int(ui.textW(o.title, 12 * s) / std::max(60.f, inner.w - ind))) * 17 * s + 8 * s + 2 * s;
        }
        R.olRows.top[ol.size()] = acc;
      }
    }
    for (size_t i = firstRow(R.olRows.top, content.y - yy); i < ol.size(); i++) {
      const float ry = yy + R.olRows.top[i];
      if (ry > content.b()) break;
      const pdf::OutlineItem& o = ol[i];
      float ind = std::min(4, o.depth) * 14 * s;
      float th = R.olRows.top[i + 1] - R.olRows.top[i] - 2 * s;
      Rect rr{content.x + 4 * s, ry, content.w - 8 * s, th};
      bool next = i + 1 < ol.size() ? ol[i + 1].page > cur : true;
      bool here = o.page >= 0 && o.page <= cur && next && (i + 1 >= ol.size() || ol[i + 1].page > cur);
      bool click = ui.listRow(rr, "rdol:" + std::to_string(i), here);
      ui.textWrap({inner.x + ind, rr.y + 4 * s, inner.w - ind - 30 * s, th - 8 * s}, o.title, 12 * s, o.depth == 0 ? ui.c.text : ui.c.textDim, o.depth == 0 ? 600 : 400);
      if (o.page >= 0) ui.text({rr.r() - 34 * s, rr.y + 4 * s, 28 * s, 16 * s}, std::to_string(o.page + 1), 10.5f * s, ui.c.textFaint, AL_RIGHT);
      if (click && o.page >= 0) readerGoTo(o.page, o.y, true);
    }
    yy += R.olRows.top.back();
    ui.endScroll(yy - y0 + 12 * s);
  } else if (R.pane == 5) {  // ---- Paper: record link, metadata, file
    ui.beginScroll("rdinfo", content);
    float yy = content.y + 6 * s - ui.scrollY();
    const float y0 = yy;
    auto row = [&](float h) { Rect rr{inner.x, yy, inner.w, h}; yy += h + 6 * s; return rr; };
    auto lbl = [&](const string& t) { ui.text(row(18 * s), t, 11.5f * s, ui.c.textDim, AL_LEFT, 600); };
    auto val = [&](const string& t, bool strong = false) { if (t.empty()) return; float h = ui.textWrap({inner.x, 0, inner.w, 1000}, t, 12.5f * s, ui.c.text, strong ? 600 : 400, false); ui.textWrap(row(h), t, 12.5f * s, ui.c.text, strong ? 600 : 400); };
    lbl("Record");
    if (record) {
      val(record->title, true);
      string by;
      for (size_t i = 0; i < record->authors.size() && i < 6; i++) by += (i ? ", " : "") + record->authors[i];
      if (record->authors.size() > 6) by += " et al.";
      val(by);
      string src = record->source;
      if (record->year) src += (src.empty() ? "" : ", ") + std::to_string(record->year);
      val(src);
      if (!record->doi.empty()) {
        Rect rr = row(20 * s);
        uint64_t idv = ui.id("rddoi");
        bool hov = false;
        if (ui.behave(idv, rr, &hov)) openUrl("https://doi.org/" + record->doi);
        ui.text(rr, "doi:" + record->doi, 11.5f * s, hov ? ui.c.accent : ui.c.textDim);
        ui.tipFor(idv, "Open at doi.org");
      }
      {
        Rect rr = row(28 * s);
        auto c3 = cols(rr, 3, 6 * s);
        if (ui.button(c3[0], "Details", BTN_GHOST, "file")) { openDoc(rec); }
        ui.tip("The record in the inspector: abstract, keywords, references");
        if (ui.button(c3[1], "Cite", BTN_GHOST, "writer")) readerCite();
        if (ui.button(c3[2], "Change\xE2\x80\xA6", BTN_GHOST, "link", hasCorpus())) { R.linkPick = !R.linkPick; R.linkQuery.clear(); }
      }
    } else {
      val(hasCorpus() ? "Not linked to a record. Citations and the coding matrix need the link; pick the record below (the list is searched by title, author and year)." : "Import the bibliographic records (Data) to link this PDF to one of them.");
      if (!item->title.empty()) { lbl("From the PDF"); val(item->title, true); if (!item->author.empty()) val(item->author); if (!item->doi.empty()) val("doi:" + item->doi); }
      if (hasCorpus()) R.linkPick = true;
    }
    if (R.linkPick && hasCorpus()) {
      lbl("Link to a record");
      ui.textInput(row(28 * s), "rdlinkq", R.linkQuery, "Search title, author, year\xE2\x80\xA6", nullptr, "search");
      string q = lower(trim(R.linkQuery));
      if (q.empty() && !item->title.empty()) q = lower(truncate(item->title, 40));
      vector<std::pair<int, int>> scored;  // score, rec
      vector<string> toks = split(q, ' ');
      for (size_t i = 0; i < P->corpus.recs.size(); i++) {
        const Record& rr = P->corpus.recs[i];
        string hay = lower(rr.title + " " + (rr.authors.empty() ? "" : rr.authors[0]) + " " + std::to_string(rr.year));
        int score = 0;
        for (auto& t : toks) if (t.size() > 1 && contains(hay, t)) score += int(t.size());
        if (score > 0 || q.empty()) scored.push_back({score, int(i)});
      }
      std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      int shown = 0;
      for (auto& sc : scored) {
        if (shown++ >= 8) break;
        const Record& rr = P->corpus.recs[size_t(sc.second)];
        Rect r2{content.x + 4 * s, yy, content.w - 8 * s, 44 * s};
        bool click = ui.listRow(r2, "rdlink:" + std::to_string(sc.second), sc.second == rec);
        ui.text({inner.x + 6 * s, r2.y + 4 * s, inner.w - 12 * s, 18 * s}, rr.title, 12 * s, ui.c.text, AL_LEFT, 500);
        string sub2 = (rr.authors.empty() ? "" : rr.authors[0] + (rr.authors.size() > 1 ? " et al." : "")) + (rr.year ? " \xC2\xB7 " + std::to_string(rr.year) : "") + (rr.source.empty() ? "" : " \xC2\xB7 " + truncate(rr.source, 36));
        ui.text({inner.x + 6 * s, r2.y + 23 * s, inner.w - 12 * s, 16 * s}, sub2, 11 * s, ui.c.textDim);
        if (click) { P->library.link(*item, rr); P->dirty = true; R.linkPick = false; ui.toast("Linked", truncate(rr.title, 70), 1, 3); }
        yy += 46 * s;
      }
      if (scored.empty()) { ui.text(row(18 * s), "No record matches.", 11.5f * s, ui.c.textFaint); }
      if (record) { Rect rr = row(24 * s); if (ui.button({rr.x, rr.y, 120 * s, rr.h}, "Unlink", BTN_GHOST)) { item->recordKey.clear(); P->dirty = true; R.linkPick = false; } }
      yy += 4 * s;
    }
    lbl("PDF");
    {
      const pdf::DocInfo& I = R.doc.info;
      if (!I.title.empty()) val(I.title);
      if (!I.author.empty()) val(I.author);
      string meta = plural(std::max(I.pages, item->pages), "page");
      if (!I.sizes.empty()) meta += "  \xC2\xB7  " + fmtNum(I.sizes[0][0] / 72 * 25.4, 0) + " \xC3\x97 " + fmtNum(I.sizes[0][1] / 72 * 25.4, 0) + " mm";
      if (item->size > 0) meta += "  \xC2\xB7  " + fmtBytes(item->size);
      val(meta);
      if (!I.producer.empty() || !I.creator.empty()) val(trim(I.creator + (I.creator.empty() || I.producer.empty() ? "" : " / ") + I.producer));
      if (!I.created.empty()) val("Created " + I.created.substr(0, 10));
      int inFile = 0;
      for (auto& a : item->annots) if (a.inFile) inFile++;
      int fileOwn = 0;
      for (auto& p : R.doc.pages) fileOwn += int(p.fileAnnots.size());
      if (fileOwn > 0) val(plural(fileOwn, "annotation") + " inside the file" + (inFile ? " (" + std::to_string(inFile) + " saved from here)" : string()));
    }
    lbl("File");
    {
      float h = ui.textWrap({inner.x, 0, inner.w, 1000}, item->path, 11 * s, ui.c.textDim, 400, false);
      ui.textWrap(row(h), item->path, 11 * s, ui.c.textDim);
      Rect rr = row(28 * s);
      auto c3 = cols(rr, 3, 6 * s);
      if (ui.button(c3[0], "Explorer", BTN_GHOST, "folder")) revealInExplorer(item->path);
      if (ui.button(c3[1], "Open in\xE2\x80\xA6", BTN_GHOST, "external")) openUrl(item->path);
      ui.tip("Open in the default PDF application");
      if (ui.button(c3[2], "Locate\xE2\x80\xA6", BTN_GHOST, "search")) readerRelocate(item->id);
    }
    lbl("Annotations");
    {
      int n = item->annotCount();
      int inFile = 0;
      for (auto& a : item->annots) if (a.inFile) inFile++;
      val(n == 0 ? "None yet. They are stored in the project; the PDF file is not modified until you save them into it." : plural(n, "annotation") + " in the project" + (inFile == n ? ", all saved into the file." : inFile ? ", " + std::to_string(n - inFile) + " not yet in the file." : "; the file has none of them yet."));
      Rect rr = row(28 * s);
      auto c2 = cols(rr, 2, 6 * s);
      if (ui.button(c2[0], "Save into PDF", BTN_NORMAL, "save", n > 0)) readerSaveIntoPdf(item->id);
      ui.tip("Writes highlights, notes, areas, and freehand strokes into the PDF as standard annotations. The project keeps its own copy.");
      if (ui.button(c2[1], "Detach", BTN_GHOST, "trash")) { string id = item->id; closeReader(); readerDetach(id); ui.endScroll(yy - y0); return; }
    }
    ui.endScroll(yy - y0 + 20 * s);
  }
}

}  // namespace win
}  // namespace vs
