// VOSStudio Native: the papers table. Every record of the project as one sortable, filterable list in the main area
// (Ctrl+T, the table button in the top bar, Data page, the palette, the `papers` command and the assistant's show_papers).
// Rows are virtual: only the visible ones are laid out, so 50,000 records scroll as smoothly as 50.
#include "../core/records.h"
#include "../core/oa.h"
#include "app.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace vs {
namespace win {

namespace {

// "Smith J; Lee K; +3" — the first authors as they appear in the record
string authorsCell(const Record& r, size_t maxN) {
  string s;
  for (size_t i = 0; i < r.authors.size() && i < maxN; i++) s += (i ? "; " : "") + r.authors[i];
  if (r.authors.size() > maxN) s += " +" + std::to_string(r.authors.size() - maxN);
  return s;
}

string firstAuthorKey(const Record& r) { return r.authors.empty() ? string("\xEF\xBF\xBF") : lower(r.authors[0]); }  // unknown authors sort last

// one filter token against one record (all lower case); "2015-2020" and "2015..2020" are year ranges
bool tokenMatches(const Record& r, const string& tok, const string& hayLower) {
  if (tok.size() >= 9 && (tok.find('-') == 4 || tok.find("..") == 4)) {
    int a = toInt(tok.substr(0, 4), 0), b = toInt(tok.substr(tok.find("..") == 4 ? 6 : 5), 0);
    if (a >= 1000 && b >= 1000) return r.year >= std::min(a, b) && r.year <= std::max(a, b);
  }
  return hayLower.find(tok) != string::npos;
}

// everything a filter word can match, lower case, built once per record per rebuild
string haystack(const Record& r) {
  string h = r.title;
  h += '\n'; h += r.source;
  h += '\n'; h += r.doi;
  h += '\n'; h += r.docType;
  if (r.year) { h += '\n'; h += std::to_string(r.year); }
  for (auto& a : r.authors) { h += '\n'; h += a; }
  for (auto& k : r.keywords) { h += '\n'; h += k; }
  for (auto& k : r.indexTerms) { h += '\n'; h += k; }
  for (auto& c : r.countries) { h += '\n'; h += c; }
  return lower(h);
}

void addUniqueTag(vector<string>& tags, const string& raw) {
  string t = trim(raw), key = lower(t);
  if (key.empty()) return;
  for (auto& old : tags) if (lower(trim(old)) == key) return;
  tags.push_back(std::move(t));
}

const char* kSortNames[] = {"year", "title", "authors", "source", "citations"};

string lineList(const vector<string>& values) { return join(values, "\n"); }
vector<string> parseLines(const string& raw) {
  vector<string> out;
  std::istringstream in(raw);
  string line;
  while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); line = trim(line); if (!line.empty()) out.push_back(std::move(line)); }
  if (out.empty() && !trim(raw).empty()) out.push_back(trim(raw));
  return out;
}
vector<string> parseListText(const string& raw) {
  vector<string> out;
  for (const string& line : splitAny(raw, ";,\r\n")) { string item = trim(line); if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) out.push_back(std::move(item)); }
  return out;
}
string metadataExtraText(const std::map<string, string>& extra) {
  Json j = Json::object();
  for (const auto& kv : extra) j.set(kv.first, kv.second);
  return j.dump(2);
}
}  // namespace

int App::papersSortFromName(const string& n) {
  string s = lower(trim(n));
  if (s.empty()) return -1;
  if (s == "year" || s == "recent" || s == "date") return 0;
  if (s == "title") return 1;
  if (s == "author" || s == "authors") return 2;
  if (s == "source" || s == "journal") return 3;
  if (s == "citations" || s == "cited" || s == "cites") return 4;
  return -1;
}

void App::openPapers(const string& filter, int sort, bool desc, bool resetScroll) {
  if (!P) return;
  if (workspace != WS_BIBLIOGRAPHY) setWorkspace(WS_BIBLIOGRAPHY);
  if (readerOpen) closeReader();
  if (writerOpen) { writerToProject(); P->dirty = true; }
  writerOpen = false;  // the table takes the main area; the document stays in the project
  workspace = WS_BIBLIOGRAPHY;
  lastBibliographyRoute = BR_PAPERS;
  page = PG_READ;       // keep the shared library navigation visible beside the records table
  papersOpen = true;
  mainChartOpen = false;
  figZoomOpen = false;
  showStart = false;
  if (sort >= 0) { papersSort = clampv(sort, 0, 4); papersDesc = desc; }
  if (sort >= 0 || !filter.empty()) papersFilter = filter;
  papersRebuild();
  if (resetScroll) ui.scrollSet("papers", 0);
  settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  needFrame = true;
}

void App::applyPaperView(int status, int pdf, const string& collectionId, const string& tag, const string& query, BibliographyRoute route) {
  if (!P) return;
  if (workspace != WS_BIBLIOGRAPHY) setWorkspace(WS_BIBLIOGRAPHY);
  if (writerOpen) { writerToProject(); P->dirty = true; }
  writerOpen = false;
  workspace = WS_BIBLIOGRAPHY;
  papersStatusFilter = status >= 0 && status < int(ReadStatus::Count) ? status : -1;
  papersPdfFilter = pdf == 0 || pdf == 1 ? pdf : -1;
  papersCollectionFilter = collectionId;
  papersTagFilter = trim(tag);
  papersFilter = query;
  papersOpen = !readerOpen;
  page = PG_READ;
  lastBibliographyRoute = route;
  if (readerOpen) { bibliographyReaderReturnRoute = route; bibliographyReaderReturnToPapers = true; }
  mainChartOpen = false;
  figZoomOpen = false;
  showStart = false;
  papersBuiltKey.clear();
  ui.scrollSet("papers", 0);
  settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute));
  needFrame = true;
}

void App::saveCurrentPaperView(const string& rawName) {
  if (!P) return;
  SavedPaperView view;
  view.name = trim(rawName); view.query = papersFilter;
  view.status = papersStatusFilter; view.pdf = papersPdfFilter;
  view.collectionId = papersCollectionFilter; view.tag = papersTagFilter;
  string id = P->library.addSavedView(view);
  if (id.empty()) { ui.toast("Saved view not added", view.name.empty() ? "Enter a name first." : "A saved view with that name already exists.", 2, 4); return; }
  P->dirty = true;
  papersBuiltKey.clear();
  needFrame = true;
  ui.toast("View saved", view.name, 1, 3);
}

void App::organizePaperSelection(const string& collectionId) {
  if (!P) return;
  PaperCollection* collection = P->library.collection(collectionId);
  if (!collection) return;
  vector<int> targets;
  for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) targets.push_back(rec);
  if (targets.empty() && papersCursor >= 0 && std::find(papersRows.begin(), papersRows.end(), papersCursor) != papersRows.end()) targets.push_back(papersCursor);
  if (targets.empty()) { ui.toast("Select papers", "Select one or more rows, or focus a paper, then organize them.", 2, 4); return; }
  for (int rec : targets) if (rec >= 0 && size_t(rec) < P->corpus.recs.size() && papersAmbiguousRecordKeys_.count(PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]))) {
    ui.toast("Can't organize these papers individually", "The current reference key is shared with another record. Resolve the duplicate or edit its identifiers first; no changes were made.", 2, 6);
    return;
  }
  bool all = true;
  for (int rec : targets) {
    if (rec < 0 || size_t(rec) >= P->corpus.recs.size() || !collection->contains(PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]))) { all = false; break; }
  }
  int changed = 0;
  for (int rec : targets) if (rec >= 0 && size_t(rec) < P->corpus.recs.size()) {
    string key = PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]);
    if (!key.empty() && P->library.setCollectionMember(collectionId, key, !all)) changed++;
  }
  if (changed) {
    P->dirty = true;
    papersBuiltKey.clear();
    ui.toast(all ? "Removed from collection" : "Added to collection", plural(changed, "paper") + " \xC2\xB7 " + collection->name, 1, 3);
  }
  needFrame = true;
}

void App::togglePaperTagForSelection(const string& rawTag) {
  if (!P) return;
  string tag = trim(rawTag);
  if (tag.empty()) return;
  vector<int> targets;
  for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) targets.push_back(rec);
  if (targets.empty() && papersCursor >= 0 && std::find(papersRows.begin(), papersRows.end(), papersCursor) != papersRows.end()) targets.push_back(papersCursor);
  if (targets.empty()) { ui.toast("Select papers", "Select one or more visible rows, or focus a paper, then edit its tags.", 2, 4); return; }
  for (int rec : targets) if (rec >= 0 && size_t(rec) < P->corpus.recs.size() && papersAmbiguousRecordKeys_.count(PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]))) {
    ui.toast("Can't tag these papers individually", "The current reference key is shared with another record. Resolve the duplicate or edit its identifiers first; no changes were made.", 2, 6);
    return;
  }
  string normalized = lower(tag);
  bool all = true;
  for (int rec : targets) {
    if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) { all = false; break; }
    string key = PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]);
    vector<string> tags = P->library.tagsForRecord(key);
    bool found = false; for (auto& old : tags) if (lower(trim(old)) == normalized) { found = true; break; }
    if (!found) { all = false; break; }
  }
  int changed = 0;
  for (int rec : targets) {
    if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) continue;
    string key = PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]);
    if (key.empty()) continue;
    vector<string> tags = P->library.tagsForRecord(key);
    auto it = std::find_if(tags.begin(), tags.end(), [&](const string& old) { return lower(trim(old)) == normalized; });
    if (all) { if (it != tags.end()) { tags.erase(it); P->library.setTagsForRecord(key, tags); changed++; } }
    else if (it == tags.end()) { tags.push_back(tag); P->library.setTagsForRecord(key, tags); changed++; }
  }
  if (changed) { P->dirty = true; papersBuiltKey.clear(); ui.toast(all ? "Tag removed" : "Tag added", "#" + tag + " · " + plural(changed, "paper"), 1, 3); }
  needFrame = true;
}

void App::openPaperMetadataEditor(int rec) {
  if (!P || rec < 0 || size_t(rec) >= P->corpus.recs.size()) return;
  papersMetadataRecord_ = rec;
  papersMetadataDraft_ = P->corpus.recs[size_t(rec)];
  papersEditYear_ = papersMetadataDraft_.year ? std::to_string(papersMetadataDraft_.year) : string();
  papersEditAuthors_ = lineList(papersMetadataDraft_.authors);
  papersEditKeywords_ = lineList(papersMetadataDraft_.keywords);
  papersEditIndexTerms_ = lineList(papersMetadataDraft_.indexTerms);
  papersEditReferences_ = lineList(papersMetadataDraft_.refs);
  papersEditAffiliations_ = lineList(papersMetadataDraft_.affiliations);
  papersEditCountries_ = lineList(papersMetadataDraft_.countries);
  papersEditExtra_ = metadataExtraText(papersMetadataDraft_.extra);
  ui.openModal("papersmeta");
}

void App::openPaperDuplicateReview() {
  if (!P) return;
  if (papersDuplicateCacheVersion_ != P->corpusVersion) {
    papersDuplicateCache_ = recordDuplicateGroups(P->corpus.recs);
    papersDuplicateCacheVersion_ = P->corpusVersion;
  }
  papersDuplicateGroups_.clear();
  for (const RecordDuplicateGroup& group : papersDuplicateCache_) {
    bool pending = false;
    for (int index : group.indices) if (index >= 0 && size_t(index) < P->corpus.recs.size() && !P->corpus.recs[size_t(index)].duplicateReviewed) pending = true;
    if (pending) papersDuplicateGroups_.push_back(group);
  }
  if (papersDuplicateGroups_.empty()) { ui.toast("Duplicates reviewed", "No duplicate candidates are waiting for review.", 1, 3); return; }
  papersDuplicatePos_ = 0;
  papersDuplicateKeepFirst_ = true;
  papersDuplicateChoices_.clear();
  ui.openModal("papersduplicates");
}

void App::keepPaperDuplicatesBoth(int groupIndex) {
  if (!P || groupIndex < 0 || size_t(groupIndex) >= papersDuplicateGroups_.size()) return;
  for (int index : papersDuplicateGroups_[size_t(groupIndex)].indices) if (index >= 0 && size_t(index) < P->corpus.recs.size()) P->corpus.recs[size_t(index)].duplicateReviewed = true;
  P->dirty = true;
  papersDuplicateCacheVersion_ = ~uint64_t(0);
  ui.closeModal();
  openPaperDuplicateReview();
  needFrame = true;
}

void App::mergePaperDuplicate(int groupIndex, bool keepFirst) {
  if (!P || groupIndex < 0 || size_t(groupIndex) >= papersDuplicateGroups_.size()) return;
  vector<int> pair;
  for (int index : papersDuplicateGroups_[size_t(groupIndex)].indices) if (index >= 0 && size_t(index) < P->corpus.recs.size()) pair.push_back(index);
  if (pair.size() < 2) { ui.closeModal(); openPaperDuplicateReview(); return; }
  int keepIndex = pair[0], removeIndex = pair[1];
  if (!keepFirst) std::swap(keepIndex, removeIndex);
  const Record loserBefore = P->corpus.recs[size_t(removeIndex)];
  Record merged = mergeRecords(P->corpus.recs[size_t(keepIndex)], loserBefore, papersDuplicateChoices_);
  int newKeepIndex = keepIndex > removeIndex ? keepIndex - 1 : keepIndex;

  if (!loserBefore.id.empty()) P->library.mergeRecordKeys(loserBefore.id, merged.id, merged);
  if (P->document.t == Json::Obj) {
    Document doc;
    if (docFromJson(doc, P->document)) {
      docMergeRecordLinks(doc, merged, loserBefore.id, newKeepIndex, removeIndex);
      P->document = docToJson(doc);
    }
  }

  auto remapNet = [&](Network& net) {
    for (Node& node : net.nodes) {
      vector<int> remapped;
      remapped.reserve(node.recs.size());
      for (int old : node.recs) {
        int now = old == removeIndex ? newKeepIndex : old > removeIndex ? old - 1 : old;
        if (now >= 0 && std::find(remapped.begin(), remapped.end(), now) == remapped.end()) remapped.push_back(now);
      }
      node.recs.swap(remapped);
    }
  };
  remapNet(P->net);
  for (SavedMap& map : P->maps) remapNet(map.net);

  vector<char> nextSelection(P->corpus.recs.size() - 1, 0);
  if (papersSel.size() == P->corpus.recs.size()) for (size_t old = 0; old < papersSel.size(); old++) {
    int now = int(old) == removeIndex ? newKeepIndex : int(old) > removeIndex ? int(old) - 1 : int(old);
    if (now >= 0 && size_t(now) < nextSelection.size()) nextSelection[size_t(now)] = char(nextSelection[size_t(now)] || papersSel[old]);
  }
  auto remapIndex = [&](int& value) {
    if (value == removeIndex) value = newKeepIndex;
    else if (value > removeIndex) value--;
  };
  remapIndex(papersCursor);
  remapIndex(docPreview);
  for (int& index : scopeRecs_) remapIndex(index);
  std::sort(scopeRecs_.begin(), scopeRecs_.end());
  scopeRecs_.erase(std::unique(scopeRecs_.begin(), scopeRecs_.end()), scopeRecs_.end());
  papersSel.swap(nextSelection);
  P->corpus.recs[size_t(keepIndex)] = std::move(merged);
  P->corpus.recs.erase(P->corpus.recs.begin() + removeIndex);
  long long parsed = 0; for (const SourceFile& file : P->corpus.files) parsed += file.records;
  P->corpus.duplicatesRemoved = int(std::max(0ll, parsed - (long long)P->corpus.recs.size()));
  P->corpusChanged();
  P->dirty = true;
  scopeChanged();
  papersBuiltKey.clear();
  papersAmbiguityIndexReady_ = false;
  papersDuplicateCacheVersion_ = ~uint64_t(0);
  ui.closeModal();
  openPaperDuplicateReview();
  ui.toast("Records merged", "One record was removed after an explicit field-by-field merge. The kept record retains its local ID.", 1, 5);
  needFrame = true;
}

void App::savePaperMetadata() {
  if (!P || papersMetadataRecord_ < 0 || size_t(papersMetadataRecord_) >= P->corpus.recs.size()) { ui.closeModal(); return; }
  Record& current = P->corpus.recs[size_t(papersMetadataRecord_)];
  if (current.id != papersMetadataDraft_.id) { ui.toast("Record changed", "This record changed while the editor was open. Reopen it before editing.", 2, 5); ui.closeModal(); return; }
  string extraError;
  Json extra = Json::parse(papersEditExtra_, &extraError);
  if (!extraError.empty() || extra.t != Json::Obj) { ui.toast("Other metadata is not valid JSON", "Use a JSON object such as {\\\"PMID\\\": \\\"12345\\\"} or restore the previous value.", 2, 6); return; }
  Record updated = papersMetadataDraft_;
  updated.title = trim(updated.title);
  updated.year = clampv(toInt(trim(papersEditYear_)), 0, 2100);
  updated.authors = parseLines(papersEditAuthors_);
  updated.keywords = parseListText(papersEditKeywords_);
  updated.indexTerms = parseListText(papersEditIndexTerms_);
  updated.refs = parseLines(papersEditReferences_);
  updated.affiliations = parseLines(papersEditAffiliations_);
  updated.countries = parseLines(papersEditCountries_);
  updated.extra.clear();
  for (const auto& kv : extra.o) updated.extra[kv.first] = kv.second.t == Json::Str ? kv.second.s : kv.second.dump();
  updated.doi = normDoi(updated.doi);
  current = updated;
  P->corpusChanged();
  P->dirty = true;
  if (P->document.t == Json::Obj) {
    Document doc;
    if (docFromJson(doc, P->document)) { docUpdateRecordReference(doc, current, papersMetadataRecord_); P->document = docToJson(doc); }
  }
  papersBuiltKey.clear();
  papersDuplicateCacheVersion_ = ~uint64_t(0);
  papersAmbiguityIndexReady_ = false;
  ui.closeModal();
  ui.toast("Metadata saved", "The record ID and document links were preserved. Rebuild an analysis if edited fields affect its network.", 1, 5);
  needFrame = true;
}

void App::drawPapersMetadataModal() {
  if (!ui.isModalOpen("papersmeta")) return;
  ui.beginModalLayer();
  float s = ui.s;
  Rect full{0, 0, float(g.W), float(g.H)};
  ui.fill(full, Color(0, 0, 0, ui.dark ? 0.42f : 0.18f));
  float w = std::min(760 * s, float(g.W) - 36 * s), h = std::min(760 * s, float(g.H) - 32 * s);
  Rect d{std::round((g.W - w) / 2), std::round((g.H - h) / 2), w, h};
  ui.shadow(d, 12 * s, 28 * s); ui.fill(d, ui.c.panel2, 12 * s); ui.stroke(d, ui.c.border, 12 * s);
  ui.text({d.x + 20 * s, d.y + 12 * s, d.w - 40 * s, 24 * s}, "Edit paper metadata", 15 * s, ui.c.text, AL_CENTER, 650);
  Rect body{d.x + 20 * s, d.y + 48 * s, d.w - 40 * s, d.h - 112 * s};
  ui.beginScroll("papersmeta", body);
  float y = body.y - ui.scrollY(), x = body.x, cw = body.w;
  auto line = [&](const string& label, string& value, const string& placeholder = string()) {
    ui.text({x, y, cw, 16 * s}, label, 11.5f * s, ui.c.textDim, AL_LEFT, 550); y += 17 * s;
    ui.textInput({x, y, cw, 28 * s}, "papersmeta:" + label, value, placeholder); y += 34 * s;
  };
  auto area = [&](const string& label, string& value, float height, const string& placeholder = string()) {
    ui.text({x, y, cw, 16 * s}, label, 11.5f * s, ui.c.textDim, AL_LEFT, 550); y += 17 * s;
    ui.textArea({x, y, cw, height * s}, "papersmeta:" + label, value, placeholder, nullptr, nullptr, 12 * s); y += (height + 8) * s;
  };
  line("Title", papersMetadataDraft_.title, "Paper title");
  line("Year", papersEditYear_, "YYYY or blank");
  area("Authors · one per line", papersEditAuthors_, 54, "One author per line");
  line("Source / journal", papersMetadataDraft_.source);
  line("Document type", papersMetadataDraft_.docType);
  line("Volume", papersMetadataDraft_.volume);
  line("Issue", papersMetadataDraft_.issue);
  line("Pages", papersMetadataDraft_.pages);
  line("DOI", papersMetadataDraft_.doi);
  line("URL", papersMetadataDraft_.url, "https://");
  line("Publisher", papersMetadataDraft_.publisher);
  line("Language", papersMetadataDraft_.language);
  area("Abstract", papersMetadataDraft_.abstract_, 92);
  area("Author keywords · one per line", papersEditKeywords_, 54);
  area("Index terms · one per line", papersEditIndexTerms_, 54);
  area("Cited references · one per line", papersEditReferences_, 64);
  area("Affiliations · one per line", papersEditAffiliations_, 48);
  area("Countries · one per line", papersEditCountries_, 42);
  area("Other metadata · JSON object", papersEditExtra_, 110, "{}");
  ui.endScroll(std::max(0.f, y - body.y + ui.scrollY()));
  Rect by{d.x + 20 * s, d.b() - 48 * s, d.w - 40 * s, 32 * s};
  float bw = 106 * s;
  if (ui.button({by.r() - bw * 2 - 8 * s, by.y, bw, by.h}, "Cancel", BTN_NORMAL)) ui.closeModal();
  if (ui.button({by.r() - bw, by.y, bw, by.h}, "Save metadata", BTN_PRIMARY, "save")) savePaperMetadata();
  ui.endModalLayer();
}

void App::drawPapersDuplicateModal() {
  if (!ui.isModalOpen("papersduplicates")) return;
  ui.beginModalLayer();
  float s = ui.s;
  Rect full{0, 0, float(g.W), float(g.H)};
  ui.fill(full, Color(0, 0, 0, ui.dark ? 0.44f : 0.18f));
  float w = std::min(840 * s, float(g.W) - 36 * s), h = std::min(710 * s, float(g.H) - 32 * s);
  Rect d{std::round((g.W - w) / 2), std::round((g.H - h) / 2), w, h};
  ui.shadow(d, 12 * s, 28 * s); ui.fill(d, ui.c.panel2, 12 * s); ui.stroke(d, ui.c.border, 12 * s);
  if (papersDuplicateGroups_.empty() || papersDuplicatePos_ >= papersDuplicateGroups_.size()) {
    ui.text({d.x + 20 * s, d.y + 24 * s, d.w - 40 * s, 30 * s}, "No duplicate candidates remain to review", 14 * s, ui.c.text, AL_CENTER, 600);
    if (ui.button({d.r() - 116 * s, d.b() - 48 * s, 96 * s, 32 * s}, "Done", BTN_PRIMARY)) ui.closeModal();
    ui.endModalLayer(); return;
  }
  const RecordDuplicateGroup& group = papersDuplicateGroups_[papersDuplicatePos_];
  vector<int> indices;
  for (int index : group.indices) if (index >= 0 && size_t(index) < P->corpus.recs.size()) indices.push_back(index);
  if (indices.size() < 2) { ui.closeModal(); ui.endModalLayer(); return; }
  int first = indices[0], second = indices[1];
  int keepIndex = papersDuplicateKeepFirst_ ? first : second;
  int mergeIndex = papersDuplicateKeepFirst_ ? second : first;
  const Record& keep = P->corpus.recs[size_t(keepIndex)];
  const Record& merge = P->corpus.recs[size_t(mergeIndex)];
  vector<RecordFieldConflict> conflicts = recordMergeConflicts(keep, merge);
  ui.text({d.x + 20 * s, d.y + 12 * s, d.w - 40 * s, 24 * s}, "Duplicate review", 15 * s, ui.c.text, AL_CENTER, 650);
  string groupTitle = "Candidate " + std::to_string(papersDuplicatePos_ + 1) + " of " + std::to_string(papersDuplicateGroups_.size()) +
                      (group.hasDoiMatch ? " · DOI match" : " · title and year match");
  ui.text({d.x + 20 * s, d.y + 38 * s, d.w - 40 * s, 18 * s}, groupTitle, 11.5f * s, ui.c.textDim, AL_CENTER);
  Rect content{d.x + 18 * s, d.y + 62 * s, d.w - 36 * s, d.h - 178 * s};
  ui.beginScroll("papersduplicates", content);
  float y = content.y - ui.scrollY();
  float gap = 12 * s, cardW = (content.w - gap) / 2;
  auto card = [&](int recIndex, float cx, bool isKeep) {
    const Record& r = P->corpus.recs[size_t(recIndex)];
    Rect c{cx, y, cardW, 138 * s}; ui.fill(c, ui.c.panel, 8 * s); ui.stroke(c, ui.c.border, 8 * s);
    ui.text({c.x + 10 * s, c.y + 6 * s, c.w - 20 * s, 20 * s}, isKeep ? "KEPT RECORD" : "MERGE CANDIDATE", 10.5f * s, isKeep ? ui.c.accent : ui.c.textDim, AL_LEFT, 650);
    ui.textWrap({c.x + 10 * s, c.y + 28 * s, c.w - 20 * s, 35 * s}, r.title.empty() ? "(untitled)" : r.title, 12 * s, ui.c.text, 600);
    string details = authorsCell(r, 3) + (r.year ? " · " + std::to_string(r.year) : "") + (r.source.empty() ? "" : " · " + r.source);
    ui.textWrap({c.x + 10 * s, c.y + 66 * s, c.w - 20 * s, 26 * s}, details, 10.5f * s, ui.c.textDim);
    ui.textWrap({c.x + 10 * s, c.y + 94 * s, c.w - 20 * s, 24 * s}, (r.doi.empty() ? "No DOI" : "DOI: " + r.doi) + " · " + std::to_string(r.cites) + " cites", 10.5f * s, ui.c.textFaint);
    Rect pick{c.x + 8 * s, c.b() - 28 * s, c.w - 16 * s, 22 * s};
    if (ui.button(pick, isKeep ? "Use as the kept record" : "Use as the merge source", isKeep ? BTN_PRIMARY : BTN_NORMAL)) {
      papersDuplicateKeepFirst_ = recIndex == first;
      papersDuplicateChoices_.clear();
    }
  };
  card(keepIndex, content.x, true); card(mergeIndex, content.x + cardW + gap, false);
  y += 148 * s;
  ui.text({content.x, y, content.w, 20 * s}, "Conflicting values · choose which value to keep for every field", 11.5f * s, ui.c.textDim, AL_LEFT, 550); y += 24 * s;
  if (conflicts.empty()) { ui.text({content.x, y, content.w, 26 * s}, "No conflicting scalar metadata. Missing fields will be filled and list fields combined.", 11 * s, ui.c.ok); y += 30 * s; }
  for (const RecordFieldConflict& conflict : conflicts) {
    ui.text({content.x, y, content.w, 16 * s}, conflict.field, 10.5f * s, ui.c.textDim, AL_LEFT, 600); y += 17 * s;
    bool useRight = papersDuplicateChoices_.count(conflict.field) && papersDuplicateChoices_[conflict.field];
    float half = (content.w - 8 * s) / 2;
    if (ui.button({content.x, y, half, 42 * s}, truncate(conflict.keepValue, 74), useRight ? BTN_NORMAL : BTN_PRIMARY)) papersDuplicateChoices_[conflict.field] = false;
    if (ui.button({content.x + half + 8 * s, y, half, 42 * s}, truncate(conflict.mergeValue, 74), useRight ? BTN_PRIMARY : BTN_NORMAL)) papersDuplicateChoices_[conflict.field] = true;
    y += 48 * s;
  }
  ui.endScroll(std::max(0.f, y - content.y + ui.scrollY()));
  Rect footer{d.x + 18 * s, d.b() - 104 * s, d.w - 36 * s, 86 * s};
  ui.textWrap({footer.x, footer.y, footer.w, 30 * s}, "Keep both preserves separate records. Merge is explicit, keeps the selected record ID, combines list metadata, and applies the field choices above.", 10.5f * s, ui.c.textFaint);
  float by = footer.y + 42 * s;
  if (ui.button({footer.x, by, 118 * s, 30 * s}, "Keep both", BTN_NORMAL, "copy")) keepPaperDuplicatesBoth(int(papersDuplicatePos_));
  if (ui.button({footer.x + 126 * s, by, 118 * s, 30 * s}, "Merge records", BTN_PRIMARY, "layers")) mergePaperDuplicate(int(papersDuplicatePos_), papersDuplicateKeepFirst_);
  float navX = footer.r() - 184 * s;
  if (ui.button({navX, by, 84 * s, 30 * s}, "Close", BTN_NORMAL)) ui.closeModal();
  if (ui.button({navX + 90 * s, by, 94 * s, 30 * s}, "Next ›", BTN_NORMAL)) {
    papersDuplicatePos_ = (papersDuplicatePos_ + 1) % papersDuplicateGroups_.size();
    papersDuplicateChoices_.clear();
  }
  ui.endModalLayer();
}

void App::papersRebuild() {
  const vector<Record>& recs = P->corpus.recs;
  uintptr_t projectPtr = reinterpret_cast<uintptr_t>(P.get());
  if (!papersAmbiguityIndexReady_ || papersAmbiguityProject_ != projectPtr || papersAmbiguityCorpusVersion_ != P->corpusVersion || papersAmbiguityRecordCount_ != recs.size()) {
    std::unordered_set<string> seen;
    seen.reserve(recs.size());
    papersAmbiguousRecordKeys_.clear();
    for (const Record& record : recs) {
      string recordKey = PdfLibrary::keyOf(record);
      if (recordKey.empty()) continue;
      if (!seen.insert(recordKey).second) papersAmbiguousRecordKeys_.insert(std::move(recordKey));
    }
    papersAmbiguityProject_ = projectPtr;
    papersAmbiguityCorpusVersion_ = P->corpusVersion;
    papersAmbiguityRecordCount_ = recs.size();
    papersAmbiguityIndexReady_ = true;
  }
  bool scoped = scopeActive();  // linked selection: only the records behind the highlighted cluster / selected items
  string key = papersFilter + "|" + std::to_string(papersSort) + (papersDesc ? "d" : "a") + "|" + std::to_string(P->corpusVersion) + "|" + std::to_string(recs.size()) +
               "|" + std::to_string(reinterpret_cast<uintptr_t>(P.get())) + "|sc" + std::to_string(scoped ? scopeVer_ : 0) +
               "|org" + std::to_string(P->library.organizationVersion) + "|st" + std::to_string(papersStatusFilter) + "|pdf" + std::to_string(papersPdfFilter) +
               "|c" + papersCollectionFilter + "|t" + lower(papersTagFilter);
  if (key == papersBuiltKey) return;
  papersBuiltKey = key;
  if (papersSel.size() != recs.size()) { papersSel.assign(recs.size(), 0); papersCursor = -1; }
  vector<char> inScope;
  if (scoped) { inScope.assign(recs.size(), 0); for (int r : scopeRecs_) if (r >= 0 && size_t(r) < recs.size()) inScope[size_t(r)] = 1; }

  struct PaperOrg { unsigned statusMask = 0; vector<string> tags; };
  std::unordered_map<string, PaperOrg> org;
  org.reserve(P->library.items.size() + P->library.recordTags.size());
  for (auto& tags : P->library.recordTags) if (!tags.recordKey.empty()) for (auto& tag : tags.tags) addUniqueTag(org[tags.recordKey].tags, tag);
  for (auto& item : P->library.items) if (!item.recordKey.empty()) {
    PaperOrg& info = org[item.recordKey];
    info.statusMask |= 1u << clampv(int(item.status), 0, int(ReadStatus::Count) - 1);
    for (auto& tag : item.tags) addUniqueTag(info.tags, tag);
    for (auto& annot : item.annots) if (!annot.tag.empty()) addUniqueTag(info.tags, annot.tag);
  }
  const PaperCollection* collection = papersCollectionFilter.empty() ? nullptr : P->library.collection(papersCollectionFilter);
  const string selectedTag = lower(trim(papersTagFilter));
  vector<string> toks;
  for (auto& t : splitAny(lower(papersFilter), " \t")) if (!t.empty()) toks.push_back(t);
  papersRows.clear();
  papersRows.reserve(recs.size());
  for (size_t i = 0; i < recs.size(); i++) {
    if (scoped && !inScope[i]) continue;
    const Record& record = recs[i];
    string recordKey = PdfLibrary::keyOf(record);
    auto oi = org.find(recordKey);
    bool hasPdf = oi != org.end() && oi->second.statusMask != 0;
    if (papersPdfFilter == 0 && hasPdf) continue;
    if (papersPdfFilter == 1 && !hasPdf) continue;
    if (papersStatusFilter >= 0 && (oi == org.end() || !(oi->second.statusMask & (1u << papersStatusFilter)))) continue;
    if (collection && !collection->contains(recordKey)) continue;
    if (!papersCollectionFilter.empty() && !collection) continue;
    if (!selectedTag.empty()) {
      bool tagged = false;
      if (oi != org.end()) for (auto& tag : oi->second.tags) if (lower(trim(tag)) == selectedTag) { tagged = true; break; }
      if (!tagged) continue;
    }
    if (!toks.empty()) {
      string hay = haystack(record);
      if (oi != org.end()) for (auto& tag : oi->second.tags) { hay += '\n'; hay += lower(tag); }
      bool ok = true;
      for (auto& t : toks) if (!tokenMatches(record, t, hay)) { ok = false; break; }
      if (!ok) continue;
    }
    papersRows.push_back(int(i));
  }
  // sort: the chosen column, then citations, year and title so that equal keys keep a stable, meaningful order
  struct Key { string s, t; int a = 0, b = 0; };
  vector<Key> keys(recs.size());
  for (int i : papersRows) {
    const Record& r = recs[size_t(i)];
    Key& k = keys[size_t(i)];
    k.t = lower(r.title);
    if (papersSort == 1) k.s = k.t;
    else if (papersSort == 2) k.s = firstAuthorKey(r);
    else if (papersSort == 3) k.s = lower(r.source);
    k.a = papersSort == 4 ? r.cites : r.year;
    k.b = papersSort == 4 ? r.year : r.cites;
  }
  const bool desc = papersDesc, textual = papersSort == 1 || papersSort == 2 || papersSort == 3;
  std::stable_sort(papersRows.begin(), papersRows.end(), [&](int x, int y) {
    const Key& kx = keys[size_t(x)];
    const Key& ky = keys[size_t(y)];
    if (textual) {
      if (kx.s != ky.s) return desc ? kx.s > ky.s : kx.s < ky.s;
      if (kx.b != ky.b) return kx.b > ky.b;  // then the more cited / more recent
      if (kx.a != ky.a) return kx.a > ky.a;
    } else {
      if (kx.a != ky.a) return desc ? kx.a > ky.a : kx.a < ky.a;
      if (kx.b != ky.b) return kx.b > ky.b;  // ties: more cited (or more recent) first
    }
    return kx.t < ky.t;
  });
}

int App::papersSelectedCount() const {
  int n = 0;
  for (char c : papersSel) n += c ? 1 : 0;
  return n;
}

// the rows an export or the assistant works on: the selection when there is one, otherwise the filtered list
vector<int> App::papersTargets() const {
  vector<int> v;
  if (papersSelectedCount() > 0) { for (int i : papersRows) if (papersSel[size_t(i)]) v.push_back(i); }
  else v = papersRows;
  return v;
}

void App::papersExport(const string& fmt) {
  vector<int> idx = papersTargets();
  if (idx.empty()) { ui.toast("Export", "No papers to export.", 2); return; }
  vector<Record> recs;
  recs.reserve(idx.size());
  for (int i : idx) recs.push_back(P->corpus.recs[size_t(i)]);
  string base = papersSelectedCount() > 0 ? "papers-selected" : papersFilter.empty() ? "papers" : "papers-filtered";
  if (fmt == "clip") {  // reference lines to the clipboard
    string out;
    for (auto& r : recs) {
      out += authorsCell(r, 3) + (r.year ? " (" + std::to_string(r.year) + ")" : string()) + ". " + trim(r.title) + ".";
      if (!r.source.empty()) out += " " + r.source + ".";
      if (!r.doi.empty()) out += " https://doi.org/" + r.doi;
      out += "\r\n";
    }
    setClipboardText(hwnd, out);
    ui.toast("Copied", plural(long(recs.size()), "reference") + " copied as text", 1, 2);
    return;
  }
  string path, body;
  if (fmt == "wos") {
    path = saveFileDialog(hwnd, "Export papers (Web of Science tagged text)", {{"Web of Science plain text", "*.txt"}}, base + "_wos.txt", "txt");
    if (path.empty()) return;
    body = writeRecords(recs, RecordExport::WoS);
  } else if (fmt == "ris") {
    path = saveFileDialog(hwnd, "Export papers (RIS)", {{"RIS", "*.ris"}}, base + ".ris", "ris");
    if (path.empty()) return;
    body = writeRecords(recs, RecordExport::RIS);
  } else if (fmt == "bib") {
    path = saveFileDialog(hwnd, "Export papers (BibTeX)", {{"BibTeX", "*.bib"}}, base + ".bib", "bib");
    if (path.empty()) return;
    body = writeRecords(recs, RecordExport::BibTeX);
  } else {
    path = saveFileDialog(hwnd, "Export papers (CSV)", {{"CSV (Scopus columns)", "*.csv"}}, base + ".csv", "csv");
    if (path.empty()) return;
    body = "\xEF\xBB\xBF" + writeRecords(recs, RecordExport::CSV);
  }
  if (writeFileU(path, body)) ui.toast("Exported", plural(long(recs.size()), "paper") + " \xE2\x86\x92 " + fileName(path), 1);
  else ui.toast("Export failed", "Could not write " + fileName(path), 3);
}

// keyboard: arrows, page keys, Home/End move the cursor (and the preview follows); Enter opens the preview; returns true when used
bool App::papersKey(int k) {
  if (!papersOpen || papersRows.empty()) return false;
  int n = int(papersRows.size());
  int pos = -1;
  if (papersCursor >= 0) { auto it = std::find(papersRows.begin(), papersRows.end(), papersCursor); if (it != papersRows.end()) pos = int(it - papersRows.begin()); }
  int rowsPerPage = std::max(1, int(papersListH_ / std::max(1.f, papersRowH_)) - 1);
  int np = pos;
  if (k == VK_DOWN) np = std::min(n - 1, pos + 1);
  else if (k == VK_UP) np = std::max(0, pos < 0 ? 0 : pos - 1);
  else if (k == VK_NEXT) np = std::min(n - 1, std::max(0, pos) + rowsPerPage);
  else if (k == VK_PRIOR) np = std::max(0, pos - rowsPerPage);
  else if (k == VK_HOME) np = 0;
  else if (k == VK_END) np = n - 1;
  else if (k == VK_RETURN) { if (papersCursor >= 0) openDoc(papersCursor); return papersCursor >= 0; }
  else return false;
  if (np < 0) np = 0;
  papersCursor = papersRows[size_t(np)];
  openDoc(papersCursor);
  // keep the cursor row in view
  float top = float(np) * papersRowH_, y = ui.scrollGet("papers");
  if (top < y) ui.scrollSet("papers", top);
  else if (top + papersRowH_ > y + papersListH_) ui.scrollSet("papers", top + papersRowH_ - papersListH_);
  needFrame = true;
  return true;
}

// what the assistant gets back from show_papers: the first rows of the table as text
string App::papersSummary(int n) const {
  const vector<Record>& recs = P->corpus.recs;
  string s = "Papers table: " + plural(long(recs.size()), "record") + " in the project, " + plural(long(papersRows.size()), "row") + " shown" +
             (scopeRecs_.empty() ? string() : " (linked selection: " + scopeLabel_ + ")") +
             (papersFilter.empty() ? string() : " for filter \"" + papersFilter + "\"") + ", sorted by " + kSortNames[clampv(papersSort, 0, 4)] + (papersDesc ? " (descending)" : " (ascending)") + ".\n";
  int m = std::min<int>(n, int(papersRows.size()));
  for (int k = 0; k < m; k++) {
    const Record& r = recs[size_t(papersRows[size_t(k)])];
    s += std::to_string(k + 1) + ". [R" + std::to_string(papersRows[size_t(k)] + 1) + "] " + authorsCell(r, 2) + " (" + (r.year ? std::to_string(r.year) : string("n.d.")) + "). " +
         truncate(trim(r.title), 110) + (r.source.empty() ? string() : ". " + truncate(r.source, 50)) + ". " + std::to_string(r.cites) + " citations\n";
  }
  if (int(papersRows.size()) > m) s += "(\xE2\x80\xA6 " + std::to_string(int(papersRows.size()) - m) + " more rows; read_papers with a query reads any of them)\n";
  return s;
}

// ------------------------------------------------------------------ drawing
void App::drawPapers(const Rect& r) {
  const float s = ui.s;
  papersRebuild();
  const vector<Record>& recs = P->corpus.recs;
  const int n = int(papersRows.size());
  if (papersDuplicateCacheVersion_ != P->corpusVersion) {
    papersDuplicateCache_ = recordDuplicateGroups(recs);
    papersDuplicateCacheVersion_ = P->corpusVersion;
  }
  int pendingDuplicates = 0;
  for (const RecordDuplicateGroup& group : papersDuplicateCache_) {
    bool pending = false;
    for (int index : group.indices) if (index >= 0 && size_t(index) < recs.size() && !recs[size_t(index)].duplicateReviewed) pending = true;
    if (pending) pendingDuplicates++;
  }
  ui.fill(r, ui.c.bg);

  // ---- header: title, count, filter, density, export, close
  Rect hdr{r.x, r.y, r.w, 50 * s};
  float x = hdr.x + 18 * s;
  ui.text({x, hdr.y, 90 * s, hdr.h}, "Papers", 15 * s, ui.c.text, AL_LEFT, 700);
  x += ui.textW("Papers", 15 * s, 700) + 10 * s;
  string count = fmtInt(long(recs.size())) + (n != int(recs.size()) ? " \xC2\xB7 " + fmtInt(long(n)) + " shown" : "");
  float cw = ui.textW(count, 12 * s, 500) + 16 * s;
  Rect badge{x, hdr.y + 16 * s, cw, 20 * s};
  ui.fill(badge, ui.c.accent.withA(0.12f), 10 * s);
  ui.text(badge, count, 12 * s, ui.c.accent, AL_CENTER, 600);
  x += cw + 14 * s;
  // linked selection chip: the table shows only the records behind the highlighted cluster / selected items
  if (scopeActive()) {
    string lab = truncate(scopeLabel(), 34);
    float lw = ui.textW(lab, 12 * s, 600) + 44 * s;
    Rect chip{x - 4 * s, hdr.y + 15 * s, lw, 22 * s};
    ui.fill(chip, ui.c.ok.withA(0.12f), 11 * s);
    ui.icon("link", chip.x + 13 * s, chip.y + chip.h / 2, 12 * s, ui.c.ok);
    ui.text({chip.x + 22 * s, chip.y, lw - 44 * s, chip.h}, lab, 12 * s, ui.c.ok, AL_LEFT, 600);
    if (ui.iconButton({chip.r() - 22 * s, chip.y + 1 * s, 20 * s, 20 * s}, "x", "Show all records (clears the cluster highlight / selection)")) clearScope();
    x += lw + 6 * s;
  }
  // right-hand controls, laid out from the right edge
  float rx = hdr.r() - 14 * s;
  auto btnW = [&](const string& l) { return ui.textW(l, 12.5f * s, 550) + 40 * s; };
  {
    float w = 32 * s;
    rx -= w;
    if (ui.iconButton({rx, hdr.y + 11 * s, w, 28 * s}, "x", workspace == WS_BIBLIOGRAPHY ? "Return to Bibliography home" : "Close the table  (Esc)")) {
      papersOpen = false;
      if (workspace == WS_BIBLIOGRAPHY) { lastBibliographyRoute = BR_REVIEW; papersStatusFilter = -1; papersPdfFilter = 1; papersBuiltKey.clear(); page = PG_READ; settings.j.set("workspaceBibliographyRoute", int(lastBibliographyRoute)); }
      return;
    }
    rx -= 8 * s;
  }
  {
    float w = 32 * s;
    rx -= w;
    bool canMap = hasMap() && (papersSelectedCount() > 0 || papersCursor >= 0);
    if (ui.iconButton({rx, hdr.y + 11 * s, w, 28 * s}, "map", "Show the selected paper(s) in the Visualization workspace", false, canMap)) showPapersOnMap();
    rx -= 8 * s;
  }
  {
    int visibleSelected = 0;
    for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) visibleSelected++;
    bool cursorVisible = papersCursor >= 0 && std::find(papersRows.begin(), papersRows.end(), papersCursor) != papersRows.end();
    float w = 32 * s;
    rx -= w;
    bool canOrganize = visibleSelected > 0 || cursorVisible;
    Rect anchor{rx, hdr.y + 11 * s, w, 28 * s};
    if (ui.iconButton(anchor, "folder", "Add or remove the focused paper(s) from a collection", false, canOrganize)) ui.openPopup("papersorg");
    if (ui.isPopupOpen("papersorg")) {
      ui.overlay([this, anchor, s]() {
        int count = 0;
        for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) count++;
        if (count == 0 && papersCursor >= 0 && std::find(papersRows.begin(), papersRows.end(), papersCursor) != papersRows.end()) count = 1;
        int rowCount = int(P->library.collections.size());
        Rect pr{anchor.r() - 260 * s, anchor.b() + 4 * s, 260 * s, (std::max(1, rowCount) * 32 + 42) * s};
        if (pr.x < 8 * s) pr.x = 8 * s;
        if (pr.b() > float(g.H) - 8 * s) pr.y = std::max(8 * s, float(g.H) - 8 * s - pr.h);
        ui.shadow(pr, 8 * s); ui.fill(pr, ui.c.panel2, 8 * s); ui.stroke(pr, ui.c.border, 8 * s); ui.popupRect(pr);
        ui.text({pr.x + 12 * s, pr.y + 4 * s, pr.w - 24 * s, 24 * s}, count == 1 ? "Organize 1 paper" : "Organize " + std::to_string(count) + " papers", 12 * s, ui.c.textDim, AL_LEFT, 550);
        if (P->library.collections.empty()) {
          ui.textWrap({pr.x + 12 * s, pr.y + 30 * s, pr.w - 24 * s, 38 * s}, "Create a collection in the library sidebar first.", 11.5f * s, ui.c.textFaint);
          return;
        }
        float y = pr.y + 30 * s;
        for (const auto& c : P->library.collections) {
          bool all = true;
          auto check = [&](int rec) { return rec >= 0 && size_t(rec) < P->corpus.recs.size() && c.contains(PdfLibrary::keyOf(P->corpus.recs[size_t(rec)])); };
          bool anySelected = false;
          for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) { anySelected = true; if (!check(rec)) { all = false; break; } }
          if (!anySelected) all = check(papersCursor);
          Rect rr{pr.x + 5 * s, y, pr.w - 10 * s, 30 * s};
          bool click = ui.listRow(rr, "papersorg:" + c.id, false);
          ui.text({rr.x + 10 * s, rr.y, 22 * s, rr.h}, all ? "\xE2\x9C\x93" : "+", 13 * s, all ? ui.c.accent : ui.c.textFaint, AL_CENTER, 600);
          ui.text({rr.x + 34 * s, rr.y, rr.w - 42 * s, rr.h}, c.name, 12.5f * s, ui.c.text);
          y += 32 * s;
          if (click) { string id = c.id; ui.closePopup(); organizePaperSelection(id); return; }
        }
      });
    }
    rx -= 8 * s;
  }
  {
    int visibleSelected = 0;
    for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) visibleSelected++;
    bool cursorVisible = papersCursor >= 0 && std::find(papersRows.begin(), papersRows.end(), papersCursor) != papersRows.end();
    float w = 32 * s;
    rx -= w;
    bool canTag = visibleSelected > 0 || cursorVisible;
    Rect anchor{rx, hdr.y + 11 * s, w, 28 * s};
    if (ui.iconButton(anchor, "tag", "Edit tags on the focused paper(s)", false, canTag)) ui.openPopup("paperstags");
    if (ui.isPopupOpen("paperstags")) {
      ui.overlay([this, anchor, s]() {
        vector<string> tags = P->library.paperTags();
        float maxH = std::max(180 * s, std::min(420 * s, float(g.H) - 24 * s));
        Rect pr{anchor.r() - 280 * s, anchor.b() + 4 * s, 280 * s, maxH};
        if (pr.x < 8 * s) pr.x = 8 * s;
        if (pr.b() > float(g.H) - 8 * s) pr.y = std::max(8 * s, float(g.H) - 8 * s - pr.h);
        ui.shadow(pr, 8 * s); ui.fill(pr, ui.c.panel2, 8 * s); ui.stroke(pr, ui.c.border, 8 * s); ui.popupRect(pr);
        ui.text({pr.x + 10 * s, pr.y + 3 * s, pr.w - 20 * s, 22 * s}, "Tags for selected papers", 12 * s, ui.c.textDim, AL_LEFT, 600);
        Rect input{pr.x + 8 * s, pr.y + 28 * s, pr.w - 76 * s, 28 * s};
        bool submitted = false;
        ui.textInput(input, "papers:newtag", papersNewTagName, "Add a tag\xE2\x80\xA6", &submitted, "tag");
        Rect add{input.r() + 4 * s, input.y, pr.r() - 8 * s - input.r() - 4 * s, input.h};
        bool addClick = ui.button(add, "Add", BTN_PRIMARY, "plus", !trim(papersNewTagName).empty());
        if ((submitted || addClick) && !trim(papersNewTagName).empty()) {
          string tag = trim(papersNewTagName); papersNewTagName.clear(); ui.closePopup(); togglePaperTagForSelection(tag); return;
        }
        vector<int> tagTargets;
        for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) tagTargets.push_back(rec);
        if (tagTargets.empty() && papersCursor >= 0 && std::find(papersRows.begin(), papersRows.end(), papersCursor) != papersRows.end()) tagTargets.push_back(papersCursor);
        vector<vector<string>> currentTagSets;
        currentTagSets.reserve(tagTargets.size());
        for (int rec : tagTargets) currentTagSets.push_back(P->library.tagsForRecord(PdfLibrary::keyOf(P->corpus.recs[size_t(rec)])));
        Rect list{pr.x + 6 * s, pr.y + 62 * s, pr.w - 12 * s, pr.h - 68 * s};
        if (tags.empty()) ui.text({list.x + 4 * s, list.y + 8 * s, list.w - 8 * s, 24 * s}, "No tags yet. Add one above.", 11.5f * s, ui.c.textFaint);
        ui.beginScroll("paperstags", list);
        float sy = ui.scrollY(), rowH = 28 * s;
        for (size_t i = 0; i < tags.size(); i++) {
          float y = list.y + float(i) * rowH - sy;
          if (y + rowH < list.y || y > list.b()) continue;
          int selected = 0, total = int(currentTagSets.size());
          for (auto& set : currentTagSets) for (auto& old : set) if (lower(trim(old)) == lower(trim(tags[i]))) { selected++; break; }
          Rect rr{list.x + 2 * s, y, list.w - 4 * s, rowH - 1 * s};
          bool click = ui.listRow(rr, "paperstag:" + tags[i], selected == total && total > 0);
          ui.text({rr.x + 8 * s, rr.y, 24 * s, rr.h}, selected == total && total > 0 ? "\xE2\x9C\x93" : "+", 12 * s, selected ? ui.c.accent : ui.c.textFaint, AL_CENTER, 600);
          ui.text({rr.x + 34 * s, rr.y, rr.w - 42 * s, rr.h}, "#" + tags[i], 12 * s, ui.c.text);
          if (click) { string tag = tags[i]; ui.closePopup(); togglePaperTagForSelection(tag); return; }
        }
        ui.endScroll(float(tags.size()) * rowH);
      });
    }
    rx -= 8 * s;
  }
  {
    int selected = 0, selectedRec = -1;
    for (int rec : papersRows) if (size_t(rec) < papersSel.size() && papersSel[size_t(rec)]) { selected++; selectedRec = rec; }
    bool cursorVisible = papersCursor >= 0 && std::find(papersRows.begin(), papersRows.end(), papersCursor) != papersRows.end();
    int editRec = selected == 1 ? selectedRec : cursorVisible ? papersCursor : -1;
    bool canEdit = editRec >= 0 && selected <= 1;
    float w = 32 * s;
    rx -= w;
    if (ui.iconButton({rx, hdr.y + 11 * s, w, 28 * s}, "pen", "Edit metadata for the focused paper (select exactly one row)", false, canEdit)) {
      if (selected > 1) ui.toast("Select one paper", "Metadata editing applies to one record at a time.", 2, 4);
      else openPaperMetadataEditor(editRec);
    }
    rx -= 8 * s;
  }
  {
    float w = 32 * s;
    rx -= w;
    string tip = pendingDuplicates ? "Review " + plural(pendingDuplicates, "duplicate candidate group") + " · inspect, keep both, or merge with explicit conflict choices" : "No duplicate candidates are waiting for review";
    if (ui.iconButton({rx, hdr.y + 11 * s, w, 28 * s}, "layers", tip, false, pendingDuplicates > 0)) openPaperDuplicateReview();
    rx -= 8 * s;
  }
  {
    string l = papersSelectedCount() > 0 ? "Export " + std::to_string(papersSelectedCount()) + " selected" : "Export";
    float w = btnW(l);
    rx -= w;
    Rect er{rx, hdr.y + 10 * s, w, 30 * s};
    if (ui.button(er, l, BTN_NORMAL, "download", n > 0)) ui.openPopup("papersexp");
    if (ui.isPopupOpen("papersexp")) {
      Rect anchor = er;
      ui.overlay([this, anchor, s]() {
        struct Opt { const char* fmt; const char* label; };
        static const Opt opts[] = {{"ris", "RIS (reference managers)"}, {"bib", "BibTeX (LaTeX, Overleaf)"}, {"csv", "CSV (Scopus columns)"}, {"wos", "Web of Science text"}, {"clip", "Copy as reference list"}};
        const int nOpt = int(sizeof(opts) / sizeof(opts[0]));
        Rect pr{anchor.r() - 250 * s, anchor.b() + 4 * s, 250 * s, nOpt * 32 * s + 8 * s};
        ui.shadow(pr, 8 * s);
        ui.fill(pr, ui.c.panel2, 8 * s);
        ui.stroke(pr, ui.c.border, 8 * s);
        ui.popupRect(pr);
        for (int i = 0; i < nOpt; i++) {
          Rect rr{pr.x + 4 * s, pr.y + 4 * s + i * 32 * s, pr.w - 8 * s, 30 * s};
          if (ui.listRow(rr, string("pex") + opts[i].fmt, false)) { ui.closePopup(); papersExport(opts[i].fmt); }
          ui.text({rr.x + 10 * s, rr.y, rr.w - 14 * s, rr.h}, opts[i].label, 12.5f * s, ui.c.text);
        }
      });
    }
    rx -= 8 * s;
  }
  {  // Get PDFs: the selection when there is one, else every shown row (those with a DOI and no file)
    string l = papersSelectedCount() > 0 ? "Get PDFs (" + std::to_string(papersSelectedCount()) + ")" : "Get PDFs";
    float w = btnW(l);
    rx -= w;
    if (ui.button({rx, hdr.y + 10 * s, w, 30 * s}, l, BTN_NORMAL, "file", n > 0 && !busy())) {
      vector<int> among;
      if (papersSelectedCount() > 0) { for (size_t i = 0; i < papersSel.size(); i++) if (papersSel[i]) among.push_back(int(i)); }
      else among = papersRows;
      fetchAsk(among, true);
    }
    ui.tip("Download the open-access copies of these papers (by DOI) and attach them. Not-open papers and refused downloads are marked in the PDF column with the reason.");
    rx -= 8 * s;
  }
  {
    float w = 32 * s;
    rx -= w;
    if (ui.iconButton({rx, hdr.y + 11 * s, w, 28 * s}, papersDetailed ? "list" : "table", papersDetailed ? "Switch to compact rows" : "Switch to detailed rows", papersDetailed)) papersDetailed = !papersDetailed;
    rx -= 8 * s;
  }
  // Keep search available at narrow widths: when the toolbar is crowded it gets a dedicated second row.
  float colTop = hdr.b();
  auto drawFilter = [&](const Rect& fr) {
    string before = papersFilter;
    ui.textInput(fr, "papers:filter", papersFilter, "Filter: words, author, source, year or 2015-2020", nullptr, "filter");
    if (papersFilter != before) ui.scrollSet("papers", 0);
    if (!papersFilter.empty() && ui.iconButton({fr.r() - 26 * s, fr.y + 4 * s, 22 * s, 22 * s}, "x", "Clear the filter")) { papersFilter.clear(); ui.scrollSet("papers", 0); }
  };
  float filterSpace = rx - x - 6 * s;
  if (filterSpace >= 240 * s) {
    float fw = std::min(360 * s, filterSpace);
    drawFilter({x, hdr.y + 10 * s, fw, 30 * s});
  } else {
    Rect row{r.x, hdr.b(), r.w, 36 * s};
    ui.fill(row, ui.c.panel);
    ui.line(row.x, row.b() - 0.5f * s, row.r(), row.b() - 0.5f * s, ui.c.border);
    drawFilter({row.x + 18 * s, row.y + 4 * s, row.w - 36 * s, 28 * s});
    colTop += row.h;
  }

  // ---- columns
  const bool detailed = papersDetailed;
  const float rowH = detailed ? 46 * s : 30 * s;
  papersRowH_ = rowH;
  struct Col { int sort; const char* title; float x = 0, w = 0; bool right = false; bool on = true; };
  Col cols[7] = {{-1, "#"}, {0, "Year"}, {1, "Title"}, {2, "Authors"}, {3, "Source"}, {4, "Cites"}, {-1, "PDF"}};
  const float pad = 14 * s;
  float avail = r.w - 2 * pad;
  cols[0].w = 42 * s; cols[1].w = 54 * s; cols[5].w = 64 * s; cols[5].right = true; cols[6].w = 44 * s;
  bool showSource = avail > 720 * s, showAuthors = avail > 540 * s;
  cols[4].on = showSource; cols[3].on = showAuthors;
  float fixed = cols[0].w + cols[1].w + cols[5].w + cols[6].w;
  float flex = avail - fixed;
  cols[3].w = showAuthors ? std::round(flex * (showSource ? 0.24f : 0.32f)) : 0;
  cols[4].w = showSource ? std::round(flex * 0.20f) : 0;
  cols[2].w = flex - cols[3].w - cols[4].w;
  float cx = r.x + pad;
  for (auto& c : cols) { c.x = cx; if (c.on) cx += c.w; }
  // column header row (sorting)
  Rect ch{r.x, colTop, r.w, 30 * s};
  ui.fill(ch, ui.c.panel);
  ui.line(ch.x, ch.b() - 0.5f * s, ch.r(), ch.b() - 0.5f * s, ui.c.border);
  for (auto& c : cols) {
    if (!c.on) continue;
    Rect hr{c.x, ch.y, c.w, ch.h};
    bool sortable = c.sort >= 0;
    bool hov = false;
    bool clicked = sortable && ui.behave(ui.id(string("papers:col:") + c.title), {hr.x - 4 * s, hr.y, hr.w + 8 * s, hr.h}, &hov);
    if (clicked) {
      if (papersSort == c.sort) papersDesc = !papersDesc;
      else { papersSort = c.sort; papersDesc = c.sort == 0 || c.sort == 4; }  // numbers start with the biggest, text with A
      ui.scrollSet("papers", 0);
    }
    if (hov) { ui.fill({hr.x - 4 * s, hr.y + 3 * s, hr.w + 8 * s, hr.h - 6 * s}, ui.c.hover, 4 * s); ui.cursor = "hand"; }
    bool active = c.sort == papersSort;
    string t = c.title;
    Color col = active ? ui.c.text : ui.c.textDim;
    float tw = ui.textW(t, 11.5f * s, 600);
    float tx = c.right ? hr.r() - tw - 14 * s : hr.x + 2 * s;
    if (c.right) ui.text({hr.x, hr.y, hr.w - 14 * s, hr.h}, t, 11.5f * s, col, AL_RIGHT, 600);
    else ui.text({tx, hr.y, hr.w - 16 * s, hr.h}, t, 11.5f * s, col, AL_LEFT, 600);
    if (active) ui.icon(papersDesc ? "chev-down" : "chev-up", (c.right ? hr.r() - 7 * s : tx + tw + 9 * s), hr.y + hr.h / 2, 11 * s, ui.c.accent, 2.f);
    if (sortable && hov) ui.tipFor(ui.id(string("papers:col:") + c.title), string("Sort by ") + lower(c.title) + (active ? (papersDesc ? " (ascending next)" : " (descending next)") : ""));
  }

  // ---- footer
  Rect ft{r.x, r.b() - 26 * s, r.w, 26 * s};
  // ---- rows (virtual)
  Rect list{r.x, ch.b(), r.w, ft.y - ch.b()};
  papersListH_ = list.h;
  if (n == 0) {
    if (recs.empty()) {
      float cy = list.y + std::max(48 * s, list.h * 0.34f);
      ui.text({list.x + 24 * s, cy, list.w - 48 * s, 30 * s}, "Your paper library is empty", 18 * s, ui.c.text, AL_CENTER, 650);
      ui.text({list.x + 32 * s, cy + 34 * s, list.w - 64 * s, 38 * s}, "Import references or drop PDFs here to start building your bibliography.", 12.5f * s, ui.c.textDim, AL_CENTER);
      float bw = 190 * s;
      Rect addR{list.x + (list.w - bw) / 2, cy + 82 * s, bw, 32 * s};
      if (ui.button(addR, "Import papers\xE2\x80\xA6", BTN_PRIMARY, "plus")) cmdOpenFiles();
    } else {
      string msg = "No paper matches \"" + papersFilter + "\".";
      ui.text({list.x, list.y + 40 * s, list.w, 24 * s}, msg, 13 * s, ui.c.textDim, AL_CENTER);
    }
  }
  ui.beginScroll("papers", list);
  float sy = ui.scrollY();
  int first = std::max(0, int(std::floor(sy / rowH)));
  int last = std::min(n, int(std::ceil((sy + list.h) / rowH)) + 1);
  const Color zebra = ui.c.text.withA(ui.dark ? 0.03f : 0.025f);
  const bool ctrl = ui.in.ctrl, shift = ui.in.shift;
  for (int k = first; k < last; k++) {
    int rec = papersRows[size_t(k)];
    const Record& d = recs[size_t(rec)];
    Rect rr{list.x, list.y + float(k) * rowH - sy, list.w, rowH};
    if (rr.b() < list.y || rr.y > list.b()) continue;
    // the PDF cell (before the row, so that its click is its own): open the attached paper, or attach one
    const PdfItem* pdfItem = readerItemForRecord(rec);
    Rect pdfR{cols[6].x + (cols[6].w - 26 * s) / 2, rr.y + (rowH - 26 * s) / 2, 26 * s, 26 * s};
    bool pdfHov = false;
    bool pdfClick = ui.behave(ui.id("papers:pdf:" + std::to_string(rec)), pdfR, &pdfHov);
    bool hov = false;
    bool clicked = ui.behave(ui.id("papers:row:" + std::to_string(rec)), rr, &hov);
    if (pdfHov) hov = false;
    bool sel = papersSel[size_t(rec)] != 0, cur = papersCursor == rec, previewed = docPreview == rec;
    if (k & 1) ui.fill(rr, zebra);
    if (sel) ui.fill(rr, ui.c.accent.withA(0.13f));
    else if (hov) ui.fill(rr, ui.c.hover);
    if (cur || previewed) ui.fill({rr.x, rr.y + 3 * s, 3 * s, rr.h - 6 * s}, ui.c.accent, 1.5f * s);
    if (clicked) {
      if (ctrl) { papersSel[size_t(rec)] = !papersSel[size_t(rec)]; papersCursor = rec; }
      else if (shift && papersCursor >= 0) {
        auto a = std::find(papersRows.begin(), papersRows.end(), papersCursor);
        int ia = a == papersRows.end() ? k : int(a - papersRows.begin());
        for (int q = std::min(ia, k); q <= std::max(ia, k); q++) papersSel[size_t(papersRows[size_t(q)])] = 1;
      } else {
        if (papersSelectedCount() > 0) std::fill(papersSel.begin(), papersSel.end(), 0);
        papersCursor = rec;
        openDoc(rec);
      }
    }
    if (hov) ui.cursor = "hand";
    // cells
    float ty = detailed ? rr.y + 5 * s : rr.y;
    float th = detailed ? 20 * s : rr.h;
    ui.text({cols[0].x, ty, cols[0].w - 8 * s, th}, std::to_string(k + 1), 11 * s, ui.c.textFaint, AL_RIGHT, 400, true);
    ui.text({cols[1].x + 6 * s, ty, cols[1].w - 8 * s, th}, d.year ? std::to_string(d.year) : "\xE2\x80\x94", 12.5f * s, ui.c.textDim, AL_LEFT, 500);
    ui.text({cols[2].x, ty, cols[2].w - 12 * s, th}, trim(d.title).empty() ? "(untitled)" : d.title, 13 * s, ui.c.text, AL_LEFT, detailed ? 550 : 450);
    if (cols[3].on) ui.text({cols[3].x, ty, cols[3].w - 12 * s, th}, d.authors.empty() ? "\xE2\x80\x94" : authorsCell(d, detailed ? 3 : 2), 12 * s, ui.c.textDim);
    if (cols[4].on) ui.text({cols[4].x, ty, cols[4].w - 12 * s, th}, d.source.empty() ? "\xE2\x80\x94" : d.source, 12 * s, ui.c.textDim);
    ui.text({cols[5].x, ty, cols[5].w - 14 * s, th}, fmtInt(d.cites), 12.5f * s, d.cites > 0 ? ui.c.text : ui.c.textFaint, AL_RIGHT, 500, true);
    const oa::FetchStatus* fetchSt = nullptr;  // the last Get PDF outcome without a file: the cell says why
    if (!pdfItem && !P->library.fetch.empty()) { auto f = P->library.fetch.find(PdfLibrary::keyOf(d)); if (f != P->library.fetch.end() && f->second.needsFile()) fetchSt = &f->second; }
    if (pdfItem || fetchSt || pdfHov || hov) {
      if (pdfHov) ui.fill(pdfR, ui.c.hover, 6 * s);
      Color pc = pdfItem ? (pdfItem->status == ReadStatus::Read ? ui.c.ok : ui.c.accent) : fetchSt ? (fetchSt->state == oa::FetchState::Refused ? ui.c.accent.withA(0.8f) : ui.c.textFaint) : ui.c.textFaint;
      ui.icon(pdfItem ? "book" : fetchSt ? (fetchSt->state == oa::FetchState::Closed ? "shield" : "warn") : "plus", pdfR.x + pdfR.w / 2, pdfR.y + pdfR.h / 2, 15 * s, pc, 1.7f);
      if (pdfHov) {
        ui.cursor = "hand";
        ui.tipFor(ui.id("papers:pdf:" + std::to_string(rec)), pdfItem ? "Read the PDF  (" + string(statusName(pdfItem->status)) + (pdfItem->annotCount() ? ", " + plural(pdfItem->annotCount(), "highlight") : string()) + ")"
                                                                  : fetchSt ? fetchSt->message(!openAlexKey().empty()) : "Get the open-access PDF, or attach a file\xE2\x80\xA6");
      }
    }
    if (pdfClick) { if (pdfItem) openReader(pdfItem->id); else fetchRecordMenu(rec, pdfR.x - 300 * s, pdfR.b()); }
    if (detailed) {  // second line: keywords, or the document type and DOI when there are none
      string sub;
      if (!d.keywords.empty()) { for (size_t i = 0; i < d.keywords.size() && i < 8; i++) sub += (i ? " \xC2\xB7 " : "") + d.keywords[i]; }
      else if (!d.indexTerms.empty()) { for (size_t i = 0; i < d.indexTerms.size() && i < 8; i++) sub += (i ? " \xC2\xB7 " : "") + d.indexTerms[i]; }
      else { sub = d.docType; if (!d.doi.empty()) sub += (sub.empty() ? "" : " \xC2\xB7 ") + string("doi:") + d.doi; }
      float subW = (cols[3].on ? cols[3].x : cols[5].x) - cols[2].x - 12 * s;
      if (!sub.empty()) ui.text({cols[2].x, rr.y + 25 * s, subW, 16 * s}, sub, 11 * s, ui.c.textFaint);
      if (cols[3].on && !d.docType.empty() && (!d.keywords.empty() || !d.indexTerms.empty()))  // the document type under the authors (unless the title line already shows it)
        ui.text({cols[3].x, rr.y + 25 * s, cols[3].w - 12 * s, 16 * s}, d.docType, 11 * s, ui.c.textFaint);
    }
    if (hov && !detailed && ui.textW(d.title, 13 * s, 450) > cols[2].w - 12 * s) ui.tip(d.title);
  }
  ui.endScroll(float(n) * rowH);
  // ---- footer text
  ui.line(ft.x, ft.y + 0.5f * s, ft.r(), ft.y + 0.5f * s, ui.c.border);
  ui.fill({ft.x, ft.y + 1 * s, ft.w, ft.h - 1 * s}, ui.c.panel);
  int selN = papersSelectedCount();
  string st = fmtInt(long(recs.size())) + (recs.size() == 1 ? " paper" : " papers");
  if (n != int(recs.size())) st += " \xC2\xB7 " + fmtInt(long(n)) + " match the filter";
  st += " \xC2\xB7 sorted by " + string(kSortNames[clampv(papersSort, 0, 4)]) + (papersDesc ? " \xE2\x86\x93" : " \xE2\x86\x91");
  if (selN > 0) st += " \xC2\xB7 " + std::to_string(selN) + " selected";
  ui.text({ft.x + 18 * s, ft.y, ft.w * 0.6f, ft.h}, st, 11.5f * s, ui.c.textDim);
  if (selN > 0) {
    string cl = "Clear selection";
    float w = ui.textW(cl, 11.5f * s, 500) + 16 * s;
    Rect cr{ft.x + 18 * s + ui.textW(st, 11.5f * s) + 10 * s, ft.y + 4 * s, w, ft.h - 8 * s};
    if (ui.listRow(cr, "papers:clearsel", false)) std::fill(papersSel.begin(), papersSel.end(), 0);
    ui.text(cr, cl, 11.5f * s, ui.c.accent, AL_CENTER, 500);
  }
  ui.text({ft.x + ft.w * 0.5f, ft.y, ft.w * 0.5f - 18 * s, ft.h}, "Click a row to preview it \xC2\xB7 Ctrl+click or Shift+click to select \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 to move", 11.5f * s, ui.c.textFaint, AL_RIGHT);
  drawPapersMetadataModal();
  drawPapersDuplicateModal();
}

}  // namespace win
}  // namespace vs
