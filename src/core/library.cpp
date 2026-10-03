#include "library.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "doc.h"  // refKeyFor

namespace vs {

namespace {
const char* kCodeNames[] = {"None", "Aim", "Method", "Finding", "Theory", "Gap", "Quote", "Question"};
// distinct, printable, readable through black text; the highlight is drawn multiplied over the page
const uint32_t kCodeColors[] = {0xFFFFE066, 0xFFFFD166, 0xFF8ECAE6, 0xFFA7E08C, 0xFFD8B4F8, 0xFFFFA69E, 0xFFFFE066, 0xFFB8C4FF};
const char* kStatusNames[] = {"To read", "Reading", "Read", "Excluded"};

string csvCell(const string& s) {
  bool q = s.find_first_of(",\"\n\r") != string::npos;
  if (!q) return s;
  string o = "\"";
  for (char c : s) { if (c == '"') o += '"'; o += c; }
  return o + "\"";
}

string normTitle(const string& t) {
  string s = lower(asciiFold(collapseWs(trim(t))));
  string o;
  for (char c : s) if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
  return o;
}

string firstAuthor(const Record& r) {
  if (r.authors.empty()) return string();
  string a = r.authors[0];
  size_t k = a.find(',');
  return trim(k == string::npos ? a : a.substr(0, k));
}

vector<string> cleanTags(const vector<string>& tags) {
  vector<string> out;
  std::set<string> seen;
  for (auto& raw : tags) {
    string t = trim(raw), key = lower(t);
    if (!key.empty() && seen.insert(key).second) out.push_back(std::move(t));
  }
  return out;
}

void appendUnique(vector<string>& out, const string& raw) {
  string t = trim(raw), key = lower(t);
  if (key.empty()) return;
  for (auto& old : out) if (lower(trim(old)) == key) return;
  out.push_back(std::move(t));
}
}  // namespace

const char* codeName(Code c) { int i = int(c); return i >= 0 && i < int(Code::Count) ? kCodeNames[i] : "None"; }
uint32_t codeColor(Code c) { int i = int(c); return i >= 0 && i < int(Code::Count) ? kCodeColors[i] : kCodeColors[0]; }
Code codeFromName(const string& s) {
  string l = lower(trim(s));
  for (int i = 0; i < int(Code::Count); i++) if (l == lower(kCodeNames[i])) return Code(i);
  if (l == "result" || l == "results" || l == "findings") return Code::Finding;
  if (l == "methods" || l == "methodology" || l == "data") return Code::Method;
  if (l == "objective" || l == "aims" || l == "purpose" || l == "rq") return Code::Aim;
  if (l == "limitation" || l == "limitations" || l == "future work" || l == "gaps") return Code::Gap;
  if (l == "framework" || l == "concept" || l == "definition") return Code::Theory;
  return Code::None;
}
const char* statusName(ReadStatus s) { int i = int(s); return i >= 0 && i < int(ReadStatus::Count) ? kStatusNames[i] : kStatusNames[0]; }

bool PdfItem::hasTag(const string& t) const {
  string l = lower(trim(t));
  for (auto& x : tags) if (lower(x) == l) return true;
  return false;
}

bool PaperCollection::contains(const string& key) const {
  if (key.empty()) return false;
  if (!recordIndex.empty()) return recordIndex.count(key) != 0;
  return std::find(recordKeys.begin(), recordKeys.end(), key) != recordKeys.end();
}

PaperCollection* PdfLibrary::collection(const string& id) {
  for (auto& c : collections) if (c.id == id) return &c;
  return nullptr;
}
const PaperCollection* PdfLibrary::collection(const string& id) const {
  for (auto& c : collections) if (c.id == id) return &c;
  return nullptr;
}

string PdfLibrary::addCollection(const string& rawName) {
  string name = trim(rawName);
  if (name.empty()) return string();
  for (auto& c : collections) if (lower(trim(c.name)) == lower(name)) return string();
  PaperCollection c;
  c.id = "c" + std::to_string(nextCollectionId++);
  c.name = std::move(name);
  collections.push_back(std::move(c));
  touchOrganization();
  return collections.back().id;
}

bool PdfLibrary::removeCollection(const string& id) {
  auto it = std::find_if(collections.begin(), collections.end(), [&](const PaperCollection& c) { return c.id == id; });
  if (it == collections.end()) return false;
  // Keep saved views meaningful: ask the caller to remove or edit dependent views first.
  for (auto& v : savedViews) if (v.collectionId == id) return false;
  collections.erase(it);
  touchOrganization();
  return true;
}

bool PdfLibrary::setCollectionMember(const string& id, const string& recordKey, bool member) {
  PaperCollection* c = collection(id);
  if (!c || recordKey.empty()) return false;
  if (c->recordIndex.empty()) for (auto& key : c->recordKeys) if (!key.empty()) c->recordIndex.insert(key);
  auto it = std::find(c->recordKeys.begin(), c->recordKeys.end(), recordKey);
  if (member) {
    if (!c->recordIndex.insert(recordKey).second) return false;
    c->recordKeys.push_back(recordKey);
  } else {
    if (!c->recordIndex.erase(recordKey)) return false;
    if (it != c->recordKeys.end()) c->recordKeys.erase(it);
  }
  touchOrganization();
  return true;
}

vector<string> PdfLibrary::tagsForRecord(const string& recordKey) const {
  if (recordKey.empty()) return {};
  if (recordTagIndexVersion != organizationVersion) {
    recordTagIndex.clear();
    for (auto& set : recordTags) if (!set.recordKey.empty()) for (auto& tag : set.tags) appendUnique(recordTagIndex[set.recordKey], tag);
    for (auto& item : items) if (!item.recordKey.empty()) for (auto& tag : item.tags) appendUnique(recordTagIndex[item.recordKey], tag);
    recordTagIndexVersion = organizationVersion;
  }
  auto it = recordTagIndex.find(recordKey);
  return it == recordTagIndex.end() ? vector<string>() : it->second;
}

void PdfLibrary::setTagsForRecord(const string& recordKey, const vector<string>& tags) {
  if (recordKey.empty()) return;
  vector<string> clean = cleanTags(tags);
  auto it = std::find_if(recordTags.begin(), recordTags.end(), [&](const PaperTagSet& set) { return set.recordKey == recordKey; });
  bool changed = false;
  if (clean.empty()) {
    if (it != recordTags.end()) { recordTags.erase(it); changed = true; }
  } else if (it == recordTags.end()) {
    recordTags.push_back({recordKey, clean});
    changed = true;
  } else if (it->tags != clean) {
    it->tags = clean;
    changed = true;
  }
  // Legacy tags stay on their PDF items until an explicit shared edit. From then on, all linked copies agree.
  for (auto& item : items) if (item.recordKey == recordKey && item.tags != clean) { item.tags = clean; changed = true; }
  if (changed) touchOrganization();
}

string PdfLibrary::addSavedView(const SavedPaperView& source) {
  SavedPaperView v = source;
  v.name = trim(v.name);
  if (v.name.empty()) return string();
  for (auto& old : savedViews) if (lower(trim(old.name)) == lower(v.name)) return string();
  v.id = "v" + std::to_string(nextSavedViewId++);
  v.status = v.status >= 0 && v.status < int(ReadStatus::Count) ? v.status : -1;
  v.pdf = v.pdf == 0 || v.pdf == 1 ? v.pdf : -1;
  v.query = trim(v.query);
  savedViews.push_back(std::move(v));
  touchOrganization();
  return savedViews.back().id;
}

bool PdfLibrary::removeSavedView(const string& id) {
  auto it = std::find_if(savedViews.begin(), savedViews.end(), [&](const SavedPaperView& v) { return v.id == id; });
  if (it == savedViews.end()) return false;
  savedViews.erase(it);
  touchOrganization();
  return true;
}

// ---------------------------------------------------------------- items
PdfItem* PdfLibrary::find(const string& id) { for (auto& it : items) if (it.id == id) return &it; return nullptr; }
const PdfItem* PdfLibrary::find(const string& id) const { for (auto& it : items) if (it.id == id) return &it; return nullptr; }
int PdfLibrary::indexOf(const string& id) const { for (size_t i = 0; i < items.size(); i++) if (items[i].id == id) return int(i); return -1; }

PdfItem* PdfLibrary::byPath(const string& path) {
  string p = lower(replaceAll(path, "\\", "/"));
  for (auto& it : items) if (lower(replaceAll(it.path, "\\", "/")) == p) return &it;
  return nullptr;
}

PdfItem* PdfLibrary::byRecord(const string& key) {
  if (key.empty()) return nullptr;
  for (auto& it : items) if (it.recordKey == key) return &it;
  return nullptr;
}

PdfItem& PdfLibrary::add(const string& path) {
  if (PdfItem* e = byPath(path)) return *e;
  PdfItem it;
  it.id = "p" + std::to_string(nextId++);
  it.path = path;
  it.title = fileTitle(path);
  it.added = nowIso();
  long long n = fileSizeU(path);
  it.size = n > 0 ? n : 0;
  items.push_back(std::move(it));
  return items.back();
}

bool PdfLibrary::remove(const string& id) {
  int i = indexOf(id);
  if (i < 0) return false;
  if (!items[size_t(i)].recordKey.empty() && !items[size_t(i)].tags.empty()) {
    string key = items[size_t(i)].recordKey;
    vector<string> merged = tagsForRecord(key);
    setTagsForRecord(key, merged);  // keep linked legacy tags with the record when its PDF is detached
  }
  items.erase(items.begin() + i);
  touchOrganization();
  return true;
}

void PdfLibrary::sortAnnots(PdfItem& it) {
  std::stable_sort(it.annots.begin(), it.annots.end(), [](const PdfAnnot& a, const PdfAnnot& b) {
    if (a.page != b.page) return a.page < b.page;
    float ay = a.isMarkup() && !a.quads.empty() ? a.quads[0].y0 : a.rect.y0, by = b.isMarkup() && !b.quads.empty() ? b.quads[0].y0 : b.rect.y0;
    if (std::fabs(ay - by) > 2) return ay < by;
    float ax = a.isMarkup() && !a.quads.empty() ? a.quads[0].x0 : a.rect.x0, bx = b.isMarkup() && !b.quads.empty() ? b.quads[0].x0 : b.rect.x0;
    return ax < bx;
  });
}

PdfAnnot& PdfLibrary::addAnnot(PdfItem& it, PdfAnnot a) {
  a.id = "a" + std::to_string(it.nextAnnot++);
  if (a.created.empty()) a.created = nowIso();
  a.modified = a.created;
  if (a.code != Code::None) a.color = codeColor(a.code);
  const string id = a.id;  // sortAnnots may move the new annotation away from the end
  it.annots.push_back(std::move(a));
  sortAnnots(it);
  touchOrganization();  // annotation tags participate in Library filters
  for (auto& x : it.annots) if (x.id == id) return x;
  return it.annots.back();
}

bool PdfLibrary::removeAnnot(PdfItem& it, const string& annotId) {
  for (size_t i = 0; i < it.annots.size(); i++) if (it.annots[i].id == annotId) { it.annots.erase(it.annots.begin() + long(i)); touchOrganization(); return true; }
  return false;
}

PdfAnnot* PdfLibrary::annot(PdfItem& it, const string& annotId) {
  for (auto& a : it.annots) if (a.id == annotId) return &a;
  return nullptr;
}

// ---------------------------------------------------------------- json
namespace {
Json boxJson(const pdf::Box& b) { Json a = Json::array(); a.push(std::round(b.x0 * 100) / 100); a.push(std::round(b.y0 * 100) / 100); a.push(std::round(b.x1 * 100) / 100); a.push(std::round(b.y1 * 100) / 100); return a; }
pdf::Box boxFrom(const Json& j) { pdf::Box b; if (j.t == Json::Arr && j.size() == 4) { b.x0 = float(j[0].num()); b.y0 = float(j[1].num()); b.x1 = float(j[2].num()); b.y1 = float(j[3].num()); } return b; }
}  // namespace

Json PdfLibrary::toJson() const {
  Json j = Json::object();
  j.set("nextId", nextId);
  Json arr = Json::array();
  for (auto& it : items) {
    Json o = Json::object();
    o.set("id", it.id);
    o.set("path", it.path);
    if (!it.recordKey.empty()) o.set("record", it.recordKey);
    if (!it.title.empty()) o.set("title", it.title);
    if (!it.doi.empty()) o.set("doi", it.doi);
    if (!it.author.empty()) o.set("author", it.author);
    if (it.year) o.set("year", it.year);
    if (it.pages) o.set("pages", it.pages);
    if (it.size) o.set("size", (long long)it.size);
    if (it.status != ReadStatus::ToRead) o.set("status", int(it.status));
    if (it.rating) o.set("rating", it.rating);
    if (!it.tags.empty()) { Json t = Json::array(); for (auto& x : it.tags) t.push(x); o.set("tags", t); }
    if (!it.notes.empty()) o.set("notes", it.notes);
    if (it.lastPage > 0) o.set("lastPage", std::round(it.lastPage * 1000) / 1000);
    if (it.zoom > 0) o.set("zoom", std::round(it.zoom * 1000) / 1000);
    if (it.rot) o.set("rot", it.rot);
    if (!it.added.empty()) o.set("added", it.added);
    if (!it.opened.empty()) o.set("opened", it.opened);
    if (!it.source.empty()) o.set("source", it.source.toJson());
    o.set("nextAnnot", it.nextAnnot);
    if (!it.annots.empty()) {
      Json an = Json::array();
      for (auto& a : it.annots) {
        Json x = Json::object();
        x.set("id", a.id);
        x.set("kind", a.kind);
        x.set("page", a.page);
        if (!a.quads.empty()) { Json q = Json::array(); for (auto& b : a.quads) q.push(boxJson(b)); x.set("quads", q); }
        if (!a.strokes.empty()) {
          Json paths = Json::array();
          for (auto& stroke : a.strokes) {
            Json points = Json::array();
            for (auto& p : stroke.points) { Json xy = Json::array(); xy.push(std::round(p.x * 100) / 100); xy.push(std::round(p.y * 100) / 100); points.push(xy); }
            if (points.size() >= 2) paths.push(points);
          }
          if (paths.size()) { x.set("strokes", paths); x.set("strokeWidth", std::round(a.strokeWidth * 100) / 100); }
        }
        if (!a.rect.empty()) x.set("rect", boxJson(a.rect));
        x.set("color", (long long)a.color);
        if (a.code != Code::None) x.set("code", codeName(a.code));
        if (!a.tag.empty()) x.set("tag", a.tag);
        if (!a.text.empty()) x.set("text", a.text);
        if (!a.note.empty()) x.set("note", a.note);
        if (a.start >= 0) { x.set("start", a.start); x.set("count", a.count); }
        if (!a.created.empty()) x.set("created", a.created);
        if (!a.modified.empty() && a.modified != a.created) x.set("modified", a.modified);
        if (a.inFile) x.set("inFile", true);
        an.push(x);
      }
      o.set("annots", an);
    }
    arr.push(o);
  }
  j.set("items", arr);
  if (!collections.empty()) {
    Json cs = Json::array();
    for (auto& c : collections) {
      Json x = Json::object(); x.set("id", c.id); x.set("name", c.name);
      Json members = Json::array(); for (auto& key : c.recordKeys) members.push(key);
      x.set("records", members); cs.push(x);
    }
    j.set("collections", cs);
  }
  if (!recordTags.empty()) {
    Json tags = Json::array();
    for (auto& set : recordTags) {
      Json x = Json::object(); x.set("record", set.recordKey);
      Json values = Json::array(); for (auto& tag : set.tags) values.push(tag);
      x.set("tags", values); tags.push(x);
    }
    j.set("recordTags", tags);
  }
  if (!savedViews.empty()) {
    Json views = Json::array();
    for (auto& v : savedViews) {
      Json x = Json::object(); x.set("id", v.id); x.set("name", v.name);
      if (!v.query.empty()) x.set("query", v.query);
      if (v.status >= 0) x.set("status", v.status);
      if (v.pdf >= 0) x.set("pdf", v.pdf);
      if (!v.collectionId.empty()) x.set("collection", v.collectionId);
      if (!v.tag.empty()) x.set("tag", v.tag);
      views.push(x);
    }
    j.set("savedViews", views);
  }
  if (!fetch.empty()) {
    Json f = Json::object();
    for (auto& kv : fetch) if (kv.second.state != oa::FetchState::None) f.set(kv.first, kv.second.toJson());
    j.set("fetch", f);
  }
  return j;
}

string PdfSource::versionLabel() const {
  if (version == "publishedVersion") return "published version";
  if (version == "acceptedVersion") return "accepted manuscript";
  if (version == "submittedVersion") return kind == "arxiv" ? "preprint" : "submitted manuscript";
  return "";
}
Json PdfSource::toJson() const {
  Json j = Json::object();
  if (!url.empty()) j.set("url", url);
  if (!host.empty()) j.set("host", host);
  if (!kind.empty()) j.set("kind", kind);
  if (!version.empty()) j.set("version", version);
  if (!license.empty()) j.set("license", license);
  if (!oaStatus.empty()) j.set("oaStatus", oaStatus);
  if (!fetched.empty()) j.set("fetched", fetched);
  return j;
}
PdfSource PdfSource::fromJson(const Json& j) {
  PdfSource s;
  if (j.t != Json::Obj) return s;
  s.url = j["url"].str(); s.host = j["host"].str(); s.kind = j["kind"].str(); s.version = j["version"].str();
  s.license = j["license"].str(); s.oaStatus = j["oaStatus"].str(); s.fetched = j["fetched"].str();
  return s;
}

void PdfLibrary::fromJson(const Json& j) {
  items.clear();
  collections.clear();
  recordTags.clear();
  savedViews.clear();
  fetch.clear();
  nextId = nextCollectionId = nextSavedViewId = 1;
  organizationVersion = 1;
  recordTagIndexVersion = 0;
  recordTagIndex.clear();
  allTagsCacheVersion = 0;
  allTagsCache.clear();
  if (j.t != Json::Obj) return;
  nextId = std::max(1, j["nextId"].integer(1));
  const Json& fj = j["fetch"];
  if (fj.t == Json::Obj) for (auto& kv : fj.o) { oa::FetchStatus f = oa::FetchStatus::fromJson(kv.second); if (f.state != oa::FetchState::None) fetch[kv.first] = f; }
  const Json& arr = j["items"];
  for (size_t i = 0; i < arr.size(); i++) {
    const Json& o = arr[i];
    PdfItem it;
    it.id = o["id"].str();
    it.path = o["path"].str();
    if (it.id.empty() || it.path.empty()) continue;
    it.recordKey = o["record"].str();
    it.title = o["title"].str();
    it.doi = o["doi"].str();
    it.author = o["author"].str();
    it.year = o["year"].integer();
    it.pages = o["pages"].integer();
    it.size = (long long)o["size"].num();
    int st = o["status"].integer();
    it.status = st >= 0 && st < int(ReadStatus::Count) ? ReadStatus(st) : ReadStatus::ToRead;
    it.rating = std::max(0, std::min(5, o["rating"].integer()));
    for (size_t k = 0; k < o["tags"].size(); k++) if (!o["tags"][k].str().empty()) it.tags.push_back(o["tags"][k].str());
    it.notes = o["notes"].str();
    it.lastPage = o["lastPage"].num();
    it.zoom = o["zoom"].num();
    it.rot = o["rot"].integer(0) & 3;
    it.added = o["added"].str();
    it.opened = o["opened"].str();
    it.source = PdfSource::fromJson(o["source"]);
    it.nextAnnot = std::max(1, o["nextAnnot"].integer(1));
    const Json& an = o["annots"];
    for (size_t k = 0; k < an.size(); k++) {
      const Json& x = an[k];
      PdfAnnot a;
      a.id = x["id"].str();
      if (a.id.empty()) continue;
      a.kind = std::max(0, std::min(5, x["kind"].integer()));
      a.page = std::max(0, x["page"].integer());
      for (size_t q = 0; q < x["quads"].size(); q++) { pdf::Box b = boxFrom(x["quads"][q]); if (!b.empty()) a.quads.push_back(b); }
      double width = x["strokeWidth"].num(1.8);
      a.strokeWidth = float(std::isfinite(width) ? clampv(width, 0.25, 12.0) : 1.8);
      size_t inkPoints = 0;
      const Json& paths = x["strokes"];
      for (size_t q = 0; q < paths.size() && q < 128 && inkPoints < 20000; q++) {
        const Json& path = paths[q];
        if (path.t != Json::Arr) continue;
        pdf::InkStroke stroke;
        for (size_t v = 0; v < path.size() && inkPoints < 20000; v++) {
          const Json& point = path[v];
          if (point.t != Json::Arr || point.size() != 2) continue;
          double px = point[0].num(), py = point[1].num();
          if (!std::isfinite(px) || !std::isfinite(py) || std::fabs(px) > 1000000 || std::fabs(py) > 1000000) continue;
          stroke.points.push_back({float(px), float(py)});
          inkPoints++;
        }
        if (stroke.points.size() >= 2) a.strokes.push_back(std::move(stroke));
      }
      a.rect = boxFrom(x["rect"]);
      if (a.kind == 5 && a.rect.empty()) {
        bool first = true;
        for (auto& stroke : a.strokes) for (auto& p : stroke.points) {
          if (first) { a.rect = {p.x, p.y, p.x, p.y}; first = false; }
          else { a.rect.x0 = std::min(a.rect.x0, p.x); a.rect.y0 = std::min(a.rect.y0, p.y); a.rect.x1 = std::max(a.rect.x1, p.x); a.rect.y1 = std::max(a.rect.y1, p.y); }
        }
        if (!first && a.rect.x1 <= a.rect.x0) a.rect.x1 = a.rect.x0 + 0.01f;
        if (!first && a.rect.y1 <= a.rect.y0) a.rect.y1 = a.rect.y0 + 0.01f;
      }
      a.color = x.has("color") ? uint32_t(x["color"].num()) : 0xFFFFE066;
      a.code = codeFromName(x["code"].str());
      a.tag = x["tag"].str();
      a.text = x["text"].str();
      a.note = x["note"].str();
      a.start = x.has("start") ? x["start"].integer() : -1;
      a.count = x["count"].integer();
      a.created = x["created"].str();
      a.modified = x.has("modified") ? x["modified"].str() : a.created;
      a.inFile = x["inFile"].boolean();
      // an id counter that fell behind (hand-edited file) must not produce duplicates
      if (a.id.size() > 1 && a.id[0] == 'a') it.nextAnnot = std::max(it.nextAnnot, atoi(a.id.c_str() + 1) + 1);
      it.annots.push_back(std::move(a));
    }
    sortAnnots(it);
    if (it.id.size() > 1 && it.id[0] == 'p') nextId = std::max(nextId, atoi(it.id.c_str() + 1) + 1);
    items.push_back(std::move(it));
  }

  std::set<string> collectionIds;
  const Json& cs = j["collections"];
  for (size_t i = 0; i < cs.size(); i++) {
    const Json& x = cs[i];
    PaperCollection c;
    c.id = x["id"].str(); c.name = trim(x["name"].str());
    if (c.name.empty()) continue;
    if (c.id.empty() || !collectionIds.insert(c.id).second) {
      do { c.id = "c" + std::to_string(nextCollectionId++); } while (!collectionIds.insert(c.id).second);
    }
    for (size_t k = 0; k < x["records"].size(); k++) {
      string key = x["records"][k].str();
      if (!key.empty() && c.recordIndex.insert(key).second) c.recordKeys.push_back(std::move(key));
    }
    if (c.id.size() > 1 && c.id[0] == 'c') nextCollectionId = std::max(nextCollectionId, atoi(c.id.c_str() + 1) + 1);
    collections.push_back(std::move(c));
  }

  std::set<string> tagKeys;
  const Json& ts = j["recordTags"];
  for (size_t i = 0; i < ts.size(); i++) {
    const Json& x = ts[i];
    PaperTagSet set; set.recordKey = x["record"].str();
    if (set.recordKey.empty() || !tagKeys.insert(set.recordKey).second) continue;
    vector<string> raw;
    for (size_t k = 0; k < x["tags"].size(); k++) raw.push_back(x["tags"][k].str());
    set.tags = cleanTags(raw);
    if (!set.tags.empty()) recordTags.push_back(std::move(set));
  }

  std::set<string> viewIds;
  const Json& vs = j["savedViews"];
  for (size_t i = 0; i < vs.size(); i++) {
    const Json& x = vs[i];
    SavedPaperView v;
    v.id = x["id"].str(); v.name = trim(x["name"].str());
    if (v.name.empty()) continue;
    if (v.id.empty() || !viewIds.insert(v.id).second) {
      do { v.id = "v" + std::to_string(nextSavedViewId++); } while (!viewIds.insert(v.id).second);
    }
    v.query = x["query"].str();
    int status = x["status"].integer(-1), pdf = x["pdf"].integer(-1);
    v.status = status >= 0 && status < int(ReadStatus::Count) ? status : -1;
    v.pdf = pdf == 0 || pdf == 1 ? pdf : -1;
    v.collectionId = x["collection"].str(); v.tag = x["tag"].str();
    if (v.id.size() > 1 && v.id[0] == 'v') nextSavedViewId = std::max(nextSavedViewId, atoi(v.id.c_str() + 1) + 1);
    savedViews.push_back(std::move(v));
  }
}

LibraryStats PdfLibrary::stats(const std::function<bool(const string&)>& exists) const {
  LibraryStats s;
  for (auto& it : items) {
    s.total++;
    switch (it.status) {
      case ReadStatus::ToRead: s.toRead++; break;
      case ReadStatus::Reading: s.reading++; break;
      case ReadStatus::Read: s.read++; break;
      default: s.excluded++; break;
    }
    bool withNotes = !it.notes.empty();
    for (auto& a : it.annots) {
      if (a.kind == 3) s.notes++;
      else if (a.kind == 4) s.areas++;
      else if (a.kind == 5) s.drawings++;
      else if (a.kind >= 0 && a.kind <= 2) s.highlights++;
      if (!a.note.empty()) withNotes = true;
    }
    if (withNotes) s.withNotes++;
    if (!it.recordKey.empty()) s.linked++;
    if (exists && !exists(it.path)) s.missing++;
  }
  return s;
}

const vector<string>& PdfLibrary::allTags() const {
  if (allTagsCacheVersion == organizationVersion) return allTagsCache;
  std::set<string> seen;
  vector<string> out;
  auto put = [&](const string& t) { string k = lower(trim(t)); if (k.empty() || seen.count(k)) return; seen.insert(k); out.push_back(trim(t)); };
  for (auto& it : items) { for (auto& t : it.tags) put(t); for (auto& a : it.annots) if (!a.tag.empty()) put(a.tag); }
  for (auto& set : recordTags) for (auto& t : set.tags) put(t);
  std::sort(out.begin(), out.end(), [](const string& a, const string& b) { return lower(a) < lower(b); });
  allTagsCache = std::move(out);
  allTagsCacheVersion = organizationVersion;
  return allTagsCache;
}

vector<string> PdfLibrary::paperTags() const {
  std::set<string> seen;
  vector<string> out;
  auto put = [&](const string& raw) { string t = trim(raw), key = lower(t); if (!key.empty() && seen.insert(key).second) out.push_back(std::move(t)); };
  for (auto& item : items) for (auto& tag : item.tags) put(tag);
  for (auto& set : recordTags) for (auto& tag : set.tags) put(tag);
  std::sort(out.begin(), out.end(), [](const string& a, const string& b) { return lower(a) < lower(b); });
  return out;
}

// ---------------------------------------------------------------- linking
string PdfLibrary::keyOf(const Record& r) { return !r.id.empty() ? r.id : legacyKeyOf(r); }
string PdfLibrary::legacyKeyOf(const Record& r) { return refKeyFor(r.doi, r.title, r.year ? std::to_string(r.year) : string()); }

int PdfLibrary::matchRecord(const Corpus& c, const string& doi, const string& title, int year) {
  string d = lower(normDoi(doi));
  if (!d.empty()) {
    int found = -1, count = 0;
    for (size_t i = 0; i < c.recs.size(); i++) if (lower(normDoi(c.recs[i].doi)) == d) { found = int(i); count++; }
    if (count == 1) return found;  // a shared DOI is not sufficient; try a title disambiguation below
  }
  string t = normTitle(title);
  if (t.size() < 12) return -1;
  int best = -1, bestScore = -1;
  bool tied = false;
  for (size_t i = 0; i < c.recs.size(); i++) {
    const Record& r = c.recs[i];
    string rt = normTitle(r.title);
    if (rt.empty()) continue;
    bool exact = rt == t;
    bool same = exact || (rt.size() > 24 && t.size() > 24 && (rt.find(t) == 0 || t.find(rt) == 0));
    if (!same) continue;
    if (year && r.year && std::abs(year - r.year) > 1) continue;
    int score = exact ? 2 : 1;
    if (year && r.year == year) score += 4;
    if (score > bestScore) { best = int(i); bestScore = score; tied = false; }
    else if (score == bestScore) tied = true;
  }
  return tied ? -1 : best;
}

int PdfLibrary::recordIndex(const Corpus& c, const PdfItem& it) const {
  if (it.recordKey.empty()) return -1;
  for (size_t i = 0; i < c.recs.size(); i++) if (keyOf(c.recs[i]) == it.recordKey) return int(i);
  int legacy = -1, count = 0;
  for (size_t i = 0; i < c.recs.size(); i++) if (legacyKeyOf(c.recs[i]) == it.recordKey) { legacy = int(i); count++; }
  return count == 1 ? legacy : -1;
}

void PdfLibrary::link(PdfItem& it, const Record& r) {
  const string key = keyOf(r);
  bool relinked = it.recordKey != key;
  vector<string> merged = tagsForRecord(key);
  for (auto& tag : it.tags) appendUnique(merged, tag);
  it.recordKey = key;
  if (!r.title.empty()) it.title = r.title;
  if (!r.doi.empty()) it.doi = normDoi(r.doi);
  it.author = firstAuthor(r);
  if (r.year) it.year = r.year;
  if (!merged.empty()) {
    uint64_t beforeVersion = organizationVersion;
    setTagsForRecord(key, merged);  // keep legacy PDF tags and new record tags together
    if (relinked && organizationVersion == beforeVersion) touchOrganization();
  } else if (relinked) touchOrganization();
}

int PdfLibrary::relinkAll(const Corpus& c) {
  int n = 0;
  for (auto& it : items) {
    int current = recordIndex(c, it);
    if (current >= 0) {
      const string stable = keyOf(c.recs[size_t(current)]);
      if (it.recordKey != stable) { link(it, c.recs[size_t(current)]); n++; }
      continue;
    }
    int k = matchRecord(c, it.doi, it.title, it.year);
    if (k >= 0) { link(it, c.recs[size_t(k)]); n++; }
  }
  return n;
}

int PdfLibrary::migrateRecordKeys(const Corpus& c) {
  std::unordered_map<string, string> unique;
  std::unordered_set<string> ambiguous;
  for (const Record& r : c.recs) {
    string old = legacyKeyOf(r), stable = keyOf(r);
    if (old.empty() || stable.empty() || old == stable) continue;
    auto it = unique.emplace(old, stable);
    if (!it.second && it.first->second != stable) ambiguous.insert(old);
  }
  int changed = 0;
  auto migrate = [&](string& key) {
    if (key.empty() || ambiguous.count(key)) return;
    auto it = unique.find(key);
    if (it != unique.end() && key != it->second) { key = it->second; changed++; }
  };
  for (PdfItem& item : items) migrate(item.recordKey);
  for (PaperCollection& collection : collections) {
    for (string& key : collection.recordKeys) migrate(key);
    collection.recordIndex.clear();
    for (const string& key : collection.recordKeys) if (!key.empty()) collection.recordIndex.insert(key);
  }
  for (PaperTagSet& tags : recordTags) migrate(tags.recordKey);
  if (!fetch.empty()) {
    std::map<string, oa::FetchStatus> migrated;
    for (auto& kv : fetch) {
      string key = kv.first;
      migrate(key);
      auto it = migrated.find(key);
      if (it == migrated.end()) migrated.emplace(std::move(key), kv.second);
      else if (it->second.message(true).empty() && !kv.second.message(true).empty()) it->second = kv.second;
    }
    fetch.swap(migrated);
  }
  if (changed) touchOrganization();
  return changed;
}

void PdfLibrary::mergeRecordKeys(const string& fromId, const string& keepId, const Record& keep) {
  if (fromId.empty() || keepId.empty() || fromId == keepId) return;
  vector<string> tags = tagsForRecord(fromId);
  for (const string& tag : tagsForRecord(keepId)) appendUnique(tags, tag);
  for (PdfItem& item : items) if (item.recordKey == fromId) {
    item.recordKey = keepId;
    if (!keep.title.empty()) item.title = keep.title;
    if (!keep.doi.empty()) item.doi = normDoi(keep.doi);
    if (!keep.authors.empty()) item.author = firstAuthor(keep);
    if (keep.year) item.year = keep.year;
  }
  for (PaperCollection& collection : collections) {
    vector<string> keys;
    keys.reserve(collection.recordKeys.size());
    for (const string& key : collection.recordKeys) {
      string value = key == fromId ? keepId : key;
      if (std::find(keys.begin(), keys.end(), value) == keys.end()) keys.push_back(std::move(value));
    }
    if (std::find(keys.begin(), keys.end(), keepId) == keys.end()) {
      bool wasMember = std::find(collection.recordKeys.begin(), collection.recordKeys.end(), fromId) != collection.recordKeys.end();
      if (wasMember) keys.push_back(keepId);
    }
    collection.recordKeys.swap(keys);
    collection.recordIndex.clear();
    for (const string& key : collection.recordKeys) if (!key.empty()) collection.recordIndex.insert(key);
  }
  recordTags.erase(std::remove_if(recordTags.begin(), recordTags.end(), [&](const PaperTagSet& set) { return set.recordKey == fromId; }), recordTags.end());
  if (!tags.empty()) setTagsForRecord(keepId, tags);
  auto from = fetch.find(fromId), to = fetch.find(keepId);
  if (from != fetch.end()) {
    if (to == fetch.end()) fetch[keepId] = from->second;
    else {
      oa::FetchStatus& target = to->second;
      const oa::FetchStatus& source = from->second;
      if (target.state == oa::FetchState::None) target.state = source.state;
      if (target.when.empty()) target.when = source.when;
      if (target.detail.empty()) target.detail = source.detail;
      else if (!source.detail.empty() && target.detail != source.detail) target.detail += " | merged: " + source.detail;
      if (target.landing.empty()) target.landing = source.landing;
      if (target.oaLink.empty()) target.oaLink = source.oaLink;
      if (target.pmcid.empty()) target.pmcid = source.pmcid;
      if (target.oaStatus.empty()) target.oaStatus = source.oaStatus;
      target.cachedAvailable = target.cachedAvailable || source.cachedAvailable;
      if (target.source.empty()) target.source = source.source;
    }
    fetch.erase(fromId);
  }
  touchOrganization();
}

string PdfLibrary::guessDoi(const string& text) {
  // 10.NNNN/suffix — stop at whitespace or a closing bracket; trim trailing punctuation
  size_t p = 0;
  while ((p = text.find("10.", p)) != string::npos) {
    size_t q = p + 3;
    int digits = 0;
    while (q < text.size() && text[q] >= '0' && text[q] <= '9') { q++; digits++; }
    if (digits >= 4 && q < text.size() && text[q] == '/') {
      size_t e = q + 1;
      while (e < text.size() && !isspace((unsigned char)text[e]) && text[e] != '>' && text[e] != '<' && text[e] != '"' && text[e] != '\'' && text[e] != ')' && text[e] != ']') e++;
      string d = text.substr(p, e - p);
      while (!d.empty() && (d.back() == '.' || d.back() == ',' || d.back() == ';' || d.back() == ':')) d.pop_back();
      if (d.size() > q - p + 2) return normDoi(d);
    }
    p = q;
  }
  return string();
}

string PdfLibrary::guessTitle(const pdf::TextPage& fp) {
  if (fp.empty()) return string();
  // the line(s) with the largest characters in the top 60 % of the page; joins consecutive lines of that size
  float best = 0;
  vector<float> lineH(fp.lines.size(), 0);
  for (size_t li = 0; li < fp.lines.size(); li++) {
    float h = 0;
    int n = 0;
    for (int i = fp.lines[li][0]; i <= fp.lines[li][1]; i++) { const pdf::Char& c = fp.chars[size_t(i)]; if (!c.generated && c.b.h() > 0 && c.cp > ' ') { h += c.b.h(); n++; } }
    if (n < 4) continue;
    lineH[li] = h / float(n);
    best = std::max(best, lineH[li]);
  }
  if (best <= 0) return string();
  string title;
  bool inRun = false;
  for (size_t li = 0; li < fp.lines.size(); li++) {
    bool big = lineH[li] > best * 0.9f;
    if (big) {
      string s = trim(replaceAll(fp.slice(fp.lines[li][0], fp.lines[li][1]), "\n", " "));
      if (!s.empty()) { title += (title.empty() ? "" : " ") + s; inRun = true; }
    } else if (inRun) break;
    if (title.size() > 300) break;
  }
  if (!title.empty() && title.back() == '-') title.pop_back();
  return collapseWs(title);
}

string PdfLibrary::fileTitle(const string& path) {
  string n = path;
  size_t k = n.find_last_of("/\\");
  if (k != string::npos) n = n.substr(k + 1);
  if (n.size() > 4 && lower(n.substr(n.size() - 4)) == ".pdf") n = n.substr(0, n.size() - 4);
  return collapseWs(replaceAll(replaceAll(n, "_", " "), "%20", " "));
}

// ---------------------------------------------------------------- exports
string PdfLibrary::exportMarkdown(const PdfItem& it, const Record* rec) const {
  string md = "## " + (it.title.empty() ? fileTitle(it.path) : it.title) + "\n\n";
  string meta;
  if (rec) {
    string au;
    for (size_t i = 0; i < rec->authors.size() && i < 3; i++) au += (i ? "; " : "") + rec->authors[i];
    if (rec->authors.size() > 3) au += " et al.";
    if (!au.empty()) meta += au;
    if (rec->year) meta += (meta.empty() ? "" : " (") + std::to_string(rec->year) + (meta.empty() ? "" : ")");
    if (!rec->source.empty()) meta += ". *" + rec->source + "*";
  } else {
    if (!it.author.empty()) meta += it.author;
    if (it.year) meta += (meta.empty() ? "" : " (") + std::to_string(it.year) + (meta.empty() ? "" : ")");
  }
  if (!it.doi.empty()) meta += (meta.empty() ? "" : ". ") + string("https://doi.org/") + it.doi;
  if (!meta.empty()) md += meta + "\n\n";
  string flags = string("Status: ") + statusName(it.status);
  if (it.rating) flags += " · Rating: " + string(size_t(it.rating), '*');
  if (!it.tags.empty()) { flags += " · Tags: "; for (size_t i = 0; i < it.tags.size(); i++) flags += (i ? ", " : "") + it.tags[i]; }
  md += flags + "\n\n";
  if (!it.notes.empty()) md += "### Notes\n\n" + trim(it.notes) + "\n\n";
  if (!it.annots.empty()) {
    md += "### Annotations\n\n";
    for (auto& a : it.annots) {
      string label = a.code != Code::None ? codeName(a.code) : a.tag;
      string pg = "p. " + std::to_string(a.page + 1);
      if (a.kind == 3) {
        md += "- **Note** (" + pg + "): " + trim(a.note) + "\n";
        continue;
      }
      md += "- ";
      if (!label.empty()) md += "**" + label + "** ";
      if (a.kind == 4) md += "[area] ";
      else if (a.kind == 5) md += "[drawing] ";
      if (!a.text.empty()) md += "\u201C" + collapseWs(replaceAll(a.text, "\n", " ")) + "\u201D";
      md += " (" + pg + ")";
      if (!a.note.empty()) md += " \u2014 " + trim(a.note);
      md += "\n";
    }
    md += "\n";
  }
  return md;
}

string PdfLibrary::exportMarkdownAll(const Corpus* c) const {
  string md = "# Reading notes\n\n";
  LibraryStats s = stats();
  md += std::to_string(s.total) + " papers · " + std::to_string(s.read) + " read · " + std::to_string(s.highlights) + " highlights · " + std::to_string(s.areas) + " areas · " + std::to_string(s.drawings) + " drawings · " + std::to_string(s.notes) + " notes\n\n";
  for (auto& it : items) {
    const Record* rec = nullptr;
    if (c) { int k = recordIndex(*c, it); if (k >= 0) rec = &c->recs[size_t(k)]; }
    md += exportMarkdown(it, rec);
  }
  return md;
}

string PdfLibrary::codingCsv(const Corpus* c) const {
  string out = "Title,Year,DOI,Status,Rating,Tags";
  for (int k = 1; k < int(Code::Count); k++) out += string(",") + codeName(Code(k));
  out += ",Other highlights,Areas,Drawings,Notes\n";
  for (auto& it : items) {
    const Record* rec = nullptr;
    if (c) { int k = recordIndex(*c, it); if (k >= 0) rec = &c->recs[size_t(k)]; }
    string tags;
    for (size_t i = 0; i < it.tags.size(); i++) tags += (i ? "; " : "") + it.tags[i];
    out += csvCell(rec ? rec->title : it.title) + "," + (rec && rec->year ? std::to_string(rec->year) : it.year ? std::to_string(it.year) : "") + "," + csvCell(it.doi) + "," + statusName(it.status) + "," + std::to_string(it.rating) + "," + csvCell(tags);
    int counts[int(Code::Count)] = {0};
    int areas = 0, drawings = 0, notes = 0;
    for (auto& a : it.annots) {
      if (a.kind == 3) notes++;
      else if (a.kind == 4) areas++;
      else if (a.kind == 5) drawings++;
      else if (a.kind >= 0 && a.kind <= 2) counts[int(a.code)]++;
    }
    for (int k = 1; k < int(Code::Count); k++) out += "," + std::to_string(counts[k]);
    out += "," + std::to_string(counts[0]) + "," + std::to_string(areas) + "," + std::to_string(drawings) + "," + std::to_string(notes) + "\n";
  }
  return out;
}

}  // namespace vs
