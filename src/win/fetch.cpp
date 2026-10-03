// VOSStudio Native — Get PDF (1.18): the open-access copy of a record's paper, by its DOI.
//
// Three channels, in this order: OpenAlex's cached copy (only with an API key; no bot walls, about a cent of the
// free daily budget), the open locations OpenAlex / Unpaywall list (published before accepted before submitted,
// the publisher before repositories), and arXiv. Every answer is accepted only when its first bytes are a PDF, and
// the file is checked (end marker, size, the engine opens it) before it is attached and linked to the record.
// When a paper is not open, or the site refused the download, the record keeps a precise notice (the "Without a
// PDF" list on the Read page, the PDF cell of the papers table) so the user can attach the file by hand — with the
// hint that a free OpenAlex key would have fetched the copy when OpenAlex has one. The pure logic (parsers, order,
// acceptance, messages) is in src/core/oa.*; this file is the job, the file handling and the cards.
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

#include "../core/oa.h"
#include "../core/pdfium.h"
#include "reader.h"

namespace vs {
namespace win {

namespace {
string dirOfPath(const string& p) { size_t k = p.find_last_of("\\/"); return k == string::npos ? string() : p.substr(0, k); }
string stemOf(const string& p) { string n = fileName(p); size_t d = n.rfind('.'); return d == string::npos || d == 0 ? n : n.substr(0, d); }
string tailOfFile(const string& path, size_t n) {
  FILE* f = _wfopen(widen(path).c_str(), L"rb");
  if (!f) return string();
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  long from = sz > long(n) ? sz - long(n) : 0;
  fseek(f, from, SEEK_SET);
  string b(size_t(sz - from), '\0');
  size_t got = b.empty() ? 0 : fread(&b[0], 1, b.size(), f);
  fclose(f);
  b.resize(got);
  return b;
}
string uniquePath(const string& dir, const string& name) {  // name ends with .pdf
  string base = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
  string p = dir + "\\" + base + ".pdf";
  for (int i = 2; fileExistsU(p) && i < 1000; i++) p = dir + "\\" + base + "-" + std::to_string(i) + ".pdf";
  return p;
}
struct Target {
  int rec = -1;
  string key, doi, author, title, itemId, name;  // itemId: a linked item whose file is missing (it gets the new path)
  int year = 0;
};
string firstAuthorOf(const Record& r) { return r.authors.empty() ? string() : r.authors[0]; }
}  // namespace

// ------------------------------------------------------------------ where the files go, what is missing
string App::fetchDir() const {
  if (!P->path.empty()) {
    string d = dirOfPath(P->path);
    if (!d.empty()) return d + "\\" + stemOf(P->path) + " attachments";
  }
  return attachmentsDir();
}

bool App::fetchRecordHasFile(int rec) const {
  const PdfItem* it = readerItemForRecord(rec);
  return it && fileExistsU(it->path);
}

vector<int> App::fetchCandidates(const vector<int>& among) const {
  vector<int> out;
  if (!hasCorpus()) return out;
  for (int rec : among) {
    if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) continue;
    if (oa::normDoi(P->corpus.recs[size_t(rec)].doi).empty()) continue;
    if (fetchRecordHasFile(rec)) continue;
    out.push_back(rec);
  }
  return out;
}

vector<int> App::fetchNeedsFile() const {
  vector<int> out;
  if (!hasCorpus() || P->library.fetch.empty()) return out;
  // the record keys of thousands of records are not recomputed every frame: cached until something changes
  uint64_t sig = (uint64_t(P->corpus.recs.size()) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(P->library.fetch.size()) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(P->library.items.size()) << 40) ^ (uint64_t(fetchGen) << 8) ^ uint64_t(reinterpret_cast<uintptr_t>(P->corpus.recs.data()));
  if (sig == fetchNeedSig_) return fetchNeedCache_;
  for (size_t i = 0; i < P->corpus.recs.size(); i++) {
    auto f = P->library.fetch.find(PdfLibrary::keyOf(P->corpus.recs[i]));
    if (f == P->library.fetch.end() || !f->second.needsFile()) continue;
    if (fetchRecordHasFile(int(i))) continue;  // attached by hand since
    out.push_back(int(i));
  }
  auto rank = [&](int rec) {
    auto f = P->library.fetch.find(PdfLibrary::keyOf(P->corpus.recs[size_t(rec)]));
    if (f == P->library.fetch.end()) return 5;
    switch (f->second.state) {
      case oa::FetchState::Refused: return 0;
      case oa::FetchState::Budget: return 1;
      case oa::FetchState::Failed: return 2;
      case oa::FetchState::Closed: return 3;
      default: return 4;
    }
  };
  std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return rank(a) < rank(b); });
  fetchNeedSig_ = sig;
  fetchNeedCache_ = out;
  return out;
}

string App::fetchSummary() const {
  if (!hasCorpus()) return "No records loaded.";
  int withDoi = 0, files = 0, closed = 0, refused = 0, failed = 0, budget = 0, untried = 0;
  for (size_t i = 0; i < P->corpus.recs.size(); i++) {
    const Record& r = P->corpus.recs[i];
    bool doi = !oa::normDoi(r.doi).empty();
    withDoi += doi;
    if (fetchRecordHasFile(int(i))) { files++; continue; }
    if (!doi) continue;
    auto f = P->library.fetch.find(PdfLibrary::keyOf(r));
    if (f == P->library.fetch.end()) { untried++; continue; }
    switch (f->second.state) {
      case oa::FetchState::Closed: closed++; break;
      case oa::FetchState::Refused: refused++; break;
      case oa::FetchState::Budget: budget++; break;
      case oa::FetchState::Failed: failed++; break;
      default: untried++; break;
    }
  }
  string s = std::to_string(files) + " of " + plural(long(P->corpus.recs.size()), "record") + " have a PDF; " + std::to_string(withDoi) + " have a DOI";
  if (untried) s += "; " + std::to_string(untried) + " with a DOI not fetched yet";
  if (closed) s += "; " + std::to_string(closed) + " not open access";
  if (refused) s += "; " + std::to_string(refused) + " refused by the site (attach by hand" + string(openAlexKey().empty() ? ", or add an OpenAlex key" : "") + ")";
  if (budget) s += "; " + std::to_string(budget) + " waiting for OpenAlex's daily budget";
  if (failed) s += "; " + std::to_string(failed) + " failed lookups";
  return s + ".";
}

// ------------------------------------------------------------------ the confirm card
void App::fetchAsk(const vector<int>& recs, bool always) {
  if (!hasCorpus()) { ui.toast("No records", "Import the bibliographic records first (Data).", 2); return; }
  int noDoi = 0, closedKnown = 0, haveFile = 0;
  vector<int> cands;
  for (int rec : recs) {
    if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) continue;
    const Record& r = P->corpus.recs[size_t(rec)];
    if (fetchRecordHasFile(rec)) { haveFile++; continue; }
    if (oa::normDoi(r.doi).empty()) { noDoi++; continue; }
    if (recs.size() > 1) {  // a bulk run leaves the papers known to be closed alone; the row's card retries one
      auto f = P->library.fetch.find(PdfLibrary::keyOf(r));
      if (f != P->library.fetch.end() && f->second.state == oa::FetchState::Closed) { closedKnown++; continue; }
    }
    cands.push_back(rec);
  }
  if (cands.empty()) {
    string why = recs.empty() ? "No records selected." : haveFile == int(recs.size()) ? "Every selected record already has its PDF." : noDoi ? plural(noDoi, "record") + " without a DOI: nothing to look up. Attach the files by hand." : "The selected records are known to be closed access. Retry one from its card.";
    ui.toast("Nothing to get", why, 2, 5);
    return;
  }
  fetchAskRecs = cands;
  fetchSkippedNoDoi = noDoi;
  if (cands.size() == 1 && !always) { fetchPdfs(cands); return; }
  ui.openPopup("fetchask");
}

void App::fetchRecordMenu(int rec, float x, float y) {
  fetchMenuRec = rec;
  fetchMenuX = x;
  fetchMenuY = y;
  ui.openPopup("fetchrec");
}

// ------------------------------------------------------------------ the job
bool App::fetchPdfs(const vector<int>& recs) {
  if (!hasCorpus()) return false;
  if (busy()) { ui.toast("Please wait", jobLabel + " is still running.", 2); return false; }
  vector<Target> targets;
  for (int rec : recs) {
    if (rec < 0 || size_t(rec) >= P->corpus.recs.size()) continue;
    const Record& r = P->corpus.recs[size_t(rec)];
    string key = PdfLibrary::keyOf(r), doi = oa::normDoi(r.doi);
    if (doi.empty()) {
      oa::FetchStatus fs;
      fs.state = oa::FetchState::NoDoi;
      fs.when = nowIso();
      P->library.fetch[key] = fs;
      P->dirty = true;
      fetchGen++;
      continue;
    }
    if (fetchRecordHasFile(rec)) continue;
    Target t;
    t.rec = rec;
    t.key = key;
    t.doi = doi;
    t.author = firstAuthorOf(r);
    t.title = r.title;
    t.year = r.year;
    if (const PdfItem* it = readerItemForRecord(rec)) t.itemId = it->id;
    t.name = oa::attachmentName(t.author, t.year, t.title, t.doi);
    targets.push_back(t);
  }
  if (targets.empty()) { ui.toast("Nothing to get", "The records have no DOI or already have their PDF.", 2); return false; }
  const string dir = fetchDir();
  if (!ensureDir(dir)) { ui.toast("Cannot create the folder", dir, 3, 6); return false; }
  readerEnsureEngine();  // the downloaded files are opened once on the engine thread (a damaged file is not attached)
  const string key = openAlexKey(), mail = trim(settings.j["openalexEmail"].str());
  auto run = std::make_shared<FetchRun>();
  run->total = int(targets.size());
  run->haveKey = !key.empty();
  run->dir = dir;
  run->firstKey = targets[0].key;
  fetchRun = run;
  std::shared_ptr<ReaderState> rdp = rd;
  const bool single = targets.size() == 1;
  startJob(single ? "Getting the PDF" : "Getting PDFs", [this, targets, key, mail, dir, run, rdp](Job& j) {
    using namespace oa;
    auto setCurrent = [&](const string& t) { std::lock_guard<std::mutex> lk(run->mu); run->current = t; };
    auto sleepMs = [&](int ms) { for (int t = 0; t < ms && !j.cancel; t += 50) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, ms - t))); };
    auto parse = [](const string& body) { string e; Json js = Json::parse(body, &e); return e.empty() ? js : Json(); };
    auto postStatus = [&](const Target& t, const FetchStatus& fs, int* counter) {
      post([this, t, fs, run, counter]() {
        if (hasCorpus() && size_t(t.rec) < P->corpus.recs.size() && PdfLibrary::keyOf(P->corpus.recs[size_t(t.rec)]) == t.key) {
          P->library.fetch[t.key] = fs;
          P->dirty = true;
          fetchGen++;
        }
        { std::lock_guard<std::mutex> lk(run->mu); run->done++; if (counter) (*counter)++; }
        jobLabel = string(run->total == 1 ? "Getting the PDF" : "Getting PDFs") + " " + std::to_string(run->done) + "/" + std::to_string(run->total);
        needFrame = true;
      });
    };
    // 1. one question per fifty DOIs: where are the open copies (OpenAlex; the key in a header, never in the URL)
    std::map<string, Resolved> known;
    string lookupProblem;
    vector<std::pair<string, string>> oaHeaders;
    if (!key.empty()) oaHeaders.push_back({"Authorization", "Bearer " + key});
    setCurrent("Asking OpenAlex where the open copies are\xE2\x80\xA6");
    for (size_t i = 0; i < targets.size() && !j.cancel; i += 50) {
      vector<string> dois;
      for (size_t k = i; k < std::min(targets.size(), i + 50); k++) dois.push_back(targets[k].doi);
      int st = 0;
      string err, body;
      for (int attempt = 0; attempt < 4 && !j.cancel; attempt++) {
        body = httpRequest("GET", openAlexBatchUrl(dois, mail), oaHeaders, "", &st, &err, nullptr, 40000);
        if (st == 429 && attempt < 3) { sleepMs((2 << attempt) * 1000); continue; }  // 2, 4, 8 s
        break;
      }
      if (st == 200) {
        Json js = parse(body);
        if (js.t == Json::Obj) parseOpenAlexWorks(js, known);
        else lookupProblem = "OpenAlex sent an unreadable answer";
      } else if (st == 401 || st == 403) lookupProblem = "OpenAlex did not accept the API key (Preferences \xE2\x80\xBA OpenAlex)";
      else if (st == 429) lookupProblem = "OpenAlex is rate-limited right now";
      else lookupProblem = err.empty() ? "OpenAlex answered HTTP " + std::to_string(st) : err;
      j.progress = 0.04 * double(std::min(targets.size(), i + 50)) / double(targets.size());
    }
    // 2. every paper: second opinions, the order of attack, the downloads, the verdict
    double lastArxiv = -1e9;
    bool budgetGone = false;
    int cachedNoKey = 0;
    for (size_t ti = 0; ti < targets.size(); ti++) {
      if (j.cancel) break;
      const Target& t = targets[ti];
      const double base = 0.04 + 0.96 * double(ti) / double(targets.size()), span = 0.96 / double(targets.size());
      j.progress = base;
      setCurrent(t.title.empty() ? t.doi : t.title);
      Resolved r;
      auto ki = known.find(t.doi);
      if (ki != known.end()) r = ki->second; else r.doi = t.doi;
      if (!r.known || !r.isOa) {  // Unpaywall: a second opinion when OpenAlex does not know the DOI or calls it closed
        int st = 0; string err;
        string body = httpRequest("GET", unpaywallUrl(t.doi, mail), {}, "", &st, &err, nullptr, 30000);
        if (st == 200) { Json js = parse(body); if (js.t == Json::Obj) parseUnpaywall(js, r); }
      }
      if (r.candidates.empty() && r.arxivId.empty() && !arxivIdFromDoi(t.doi, nullptr)) {  // Semantic Scholar: an arXiv id or a PMCID for a paper nobody lists as open
        int st = 0; string err;
        string body = httpRequest("GET", semanticScholarUrl(t.doi), {}, "", &st, &err, nullptr, 30000);
        if (st == 200) { Json js = parse(body); if (js.t == Json::Obj) parseSemanticScholar(js, r); }
      }
      order(r, !key.empty());
      FetchStatus fs;
      fs.when = nowIso();
      fs.landing = r.landing.empty() ? "https://doi.org/" + t.doi : r.landing;
      fs.oaStatus = r.oaStatus;
      fs.pmcid = r.pmcid;
      fs.cachedAvailable = r.cached || (r.isOa && !r.workId.empty());  // has_content lags: any open work may be served
      for (auto& c : r.candidates) if (c.kind != "cache") { fs.oaLink = c.url; break; }
      if (r.candidates.empty()) {
        if (!r.known) { fs.state = FetchState::Failed; fs.detail = !lookupProblem.empty() ? lookupProblem : "the DOI is not known to OpenAlex or Unpaywall (check it on the record)"; }
        else fs.state = FetchState::Closed;
        postStatus(t, fs, fs.state == FetchState::Closed ? &run->closed : &run->failed);
        sleepMs(250);
        continue;
      }
      vector<Attempt> attempts;
      PdfSource got;
      string finalPath;
      int tried = 0;
      bool onlyBudget = true;
      const string part = dir + "\\" + t.name + ".part";
      for (size_t ci = 0; ci < r.candidates.size() && tried < 6 && !j.cancel; ci++) {
        const Candidate& c = r.candidates[ci];
        if (c.kind == "cache" && (key.empty() || budgetGone)) continue;
        if (c.kind == "arxiv") {  // arXiv asks for one request every three seconds
          double wait = 3.0 - (nowSeconds() - lastArxiv);
          if (wait > 0) sleepMs(int(wait * 1000));
          lastArxiv = nowSeconds();
        }
        tried++;
        vector<std::pair<string, string>> hd = {{"Accept", "application/pdf,*/*;q=0.8"}};
        if (c.kind == "cache") hd.push_back({"Authorization", "Bearer " + key});
        HttpDownload res;
        bool ok = false;
        for (int attempt = 0; attempt < 2; attempt++) {
          ok = httpDownload(c.url, hd, part, res, [](const char* p, size_t n) { return pdfStart(p, n); },
                            [&](long long gotB, long long total) { j.progress = base + span * (0.2 + 0.7 * (total > 0 ? double(gotB) / double(total) : 0.5)); return !j.cancel.load(); },
                            40ll << 20, 60000, {"X-RateLimit-Remaining-USD", "X-RateLimit-Reset", "X-RateLimit-Cost-USD", "Retry-After"});
          if (ok || j.cancel || !(res.accepted && res.err == "the transfer stopped early")) break;  // a dropped connection: once more
        }
        Attempt a;
        a.host = c.host.empty() ? hostOf(c.url) : c.host;
        a.kind = c.kind;
        if (c.kind == "cache") {  // what is left of today's OpenAlex budget, from the answer's headers
          RateLimit rl = parseRateLimit([&](const string& n) { auto h = res.headers.find(lower(n)); return h == res.headers.end() ? string() : h->second; });
          if (rl.remainingUsd >= 0) { std::lock_guard<std::mutex> lk(run->mu); run->remainingUsd = rl.remainingUsd; }
        }
        if (ok) {
          string tail = tailOfFile(part, 2048);
          bool endOk = pdfEnd(tail.data(), tail.size());
          int pages = -1;
          if (rdp && rdp->worker.running()) {  // the engine opens it: a damaged file is not attached
            auto prom = std::make_shared<std::promise<int>>();
            auto fut = prom->get_future();
            rdp->worker.submit(30, [part, prom](pdf::Worker&) {
              int n = -1;
              if (pdf::bound()) { pdf::Document d; string e; n = d.open(part, &e) ? d.info().pages : 0; }
              prom->set_value(n);
            });
            for (int waited = 0; waited < 30000 && !j.cancel; waited += 250) {  // the engine may be busy with the reader's tiles
              if (fut.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready) { pages = fut.get(); break; }
            }
          }
          if (pages == 0 || (pages < 0 && !endOk)) {
            removeFileU(part);
            a.result = "the file arrived damaged";
            attempts.push_back(a);
            onlyBudget = false;
            continue;
          }
          finalPath = uniquePath(dir, t.name);
          if (!renameFileU(part, finalPath)) { removeFileU(part); a.result = "the file could not be stored in " + dir; attempts.push_back(a); onlyBudget = false; continue; }
          got.url = c.url; got.host = a.host; got.kind = c.kind; got.version = c.version; got.license = c.license; got.oaStatus = r.oaStatus; got.fetched = nowIso();
          a.result = "ok";
          attempts.push_back(a);
          break;
        }
        removeFileU(part);
        if (c.kind == "cache") {
          if (res.status == 429 || res.status == 402) { budgetGone = true; a.result = "OpenAlex's daily download budget is used up"; }
          else if (res.status == 404) { a.result = "no cached copy"; onlyBudget = false; }
          else if (res.status == 401 || res.status == 403) { a.result = "OpenAlex did not accept the API key"; onlyBudget = false; }
          else { a.result = res.err.empty() ? "HTTP " + std::to_string(res.status) : res.err; onlyBudget = false; }
        } else {
          onlyBudget = false;
          if (res.status == 401 || res.status == 403) a.result = "refused (HTTP " + std::to_string(res.status) + ")";
          else if (res.status == 404 || res.status == 410) a.result = "gone (HTTP " + std::to_string(res.status) + ")";
          else if (res.status == 429) a.result = "rate-limited (HTTP 429)";
          else if (res.status >= 500) a.result = "server error (HTTP " + std::to_string(res.status) + ")";
          else if (res.err == "refused") a.result = contains(res.contentType, "html") || looksLikeHtml(res.first.data(), res.first.size()) ? "sent a web page instead of the PDF" : contains(res.contentType, "json") ? "sent a message instead of the PDF" : "sent something that is not a PDF";
          else if (res.err == "cancelled") a.result = "stopped";
          else a.result = !res.err.empty() ? res.err : "HTTP " + std::to_string(res.status);
        }
        attempts.push_back(a);
      }
      if (j.cancel && got.empty()) break;  // the record keeps its previous status
      if (!got.empty()) {
        fs.state = FetchState::Attached;
        fs.source = got.host + (got.version.empty() ? "" : " (" + got.versionLabel() + ")");
        fs.detail = describe(attempts);
        post([this, t, fs, got, finalPath, run]() {
          bool same = hasCorpus() && size_t(t.rec) < P->corpus.recs.size() && PdfLibrary::keyOf(P->corpus.recs[size_t(t.rec)]) == t.key;
          if (same) {
            const Record& rec = P->corpus.recs[size_t(t.rec)];
            PdfItem* it = t.itemId.empty() ? nullptr : P->library.find(t.itemId);
            if (it) { it->path = finalPath; it->size = std::max(0ll, fileSizeU(finalPath)); if (it->added.empty()) it->added = nowIso(); }
            else it = &P->library.add(finalPath);
            P->library.link(*it, rec);
            it->source = got;
            P->library.fetch[t.key] = fs;
            P->dirty = true;
            fetchGen++;
            std::lock_guard<std::mutex> lk(run->mu);
            run->newIds.push_back(it->id);
          }
          { std::lock_guard<std::mutex> lk(run->mu); run->done++; run->attached++; }
          jobLabel = string(run->total == 1 ? "Getting the PDF" : "Getting PDFs") + " " + std::to_string(run->done) + "/" + std::to_string(run->total);
          needFrame = true;
        });
      } else {
        bool triedCache = false;
        for (auto& a : attempts) triedCache |= a.kind == "cache";
        fs.detail = describe(attempts);
        if (tried == 0) { fs.state = FetchState::Budget; fs.detail = "The only copy listed is OpenAlex's own."; }  // only the cache, and it is out of budget
        else if (onlyBudget && triedCache) fs.state = FetchState::Budget;
        else fs.state = FetchState::Refused;
        if (fs.state == FetchState::Refused && fs.cachedAvailable && key.empty()) cachedNoKey++;
        postStatus(t, fs, fs.state == FetchState::Budget ? &run->budget : &run->refused);
      }
      j.progress = base + span;
      sleepMs(300);
    }
    {
      std::lock_guard<std::mutex> lk(run->mu);
      run->cancelled = j.cancel;
      run->cachedNoKey = cachedNoKey;
    }
  }, [this, run, single]() {
    { std::lock_guard<std::mutex> lk(run->mu); run->finished = true; }
    if (single) {
      // one paper: a toast says what happened, the card stays out of the way
      string title = "PDF not attached", msg;
      int kind = 2;
      if (run->attached) { title = "PDF attached"; msg = "Saved in " + run->dir; kind = 1; }
      else if (run->cancelled) { title = "Stopped"; msg = "The download was cancelled."; }
      else {
        msg = "See the record's PDF cell or the Read page for the reason.";
        auto f = P->library.fetch.find(run->firstKey);
        if (f != P->library.fetch.end()) msg = f->second.message(run->haveKey);
      }
      ui.toast(title, msg, kind, run->attached ? 4 : 9);
    } else {
      ui.openPopup("fetchsum");
    }
    needFrame = true;
  });
  return true;
}

// ------------------------------------------------------------------ the cards
void App::drawFetchPopups() {
  const float s = ui.s;
  const bool haveKey = !openAlexKey().empty();
  // ---- confirm
  if (ui.isPopupOpen("fetchask")) {
    ui.overlay([this, s, haveKey]() {
      const int n = int(fetchAskRecs.size());
      const string dir = fetchDir();
      Rect pr{0, 0, 440 * s, 0};
      pr.x = mainR.x + (mainR.w - pr.w) / 2;
      float pad = 18 * s, tw = pr.w - 2 * pad;
      string body = plural(n, "record") + " with a DOI " + (n == 1 ? "has" : "have") + " no PDF file yet. VOSStudio asks OpenAlex (and Unpaywall) where an open copy is, downloads it and links it to the record. Nothing is downloaded from behind paywalls; your library's access is not used.";
      string keyLine = haveKey ? "With your OpenAlex key, the copies OpenAlex keeps are fetched first: about nine in ten open-access papers come through, at about a cent each of the free daily budget."
                               : "Without an OpenAlex API key about half of the open-access papers come through (the publishers' sites refuse many automatic downloads). A free key raises this to about nine in ten.";
      string where = "Files go to " + dir + (P->path.empty() ? " (save the project to keep them next to it)" : "");
      string skip = fetchSkippedNoDoi ? plural(fetchSkippedNoDoi, "record") + " without a DOI " + (fetchSkippedNoDoi == 1 ? "is" : "are") + " skipped: attach those by hand." : "";
      float hb = ui.textWrap({0, 0, tw, 0}, body, 12.5f * s, ui.c.text, 400, false);
      float hk = ui.textWrap({0, 0, tw - 30 * s, 0}, keyLine, 12 * s, ui.c.text, 400, false);
      float hw = ui.textWrap({0, 0, tw, 0}, where, 11.5f * s, ui.c.text, 400, false);
      float hs = skip.empty() ? 0 : ui.textWrap({0, 0, tw, 0}, skip, 11.5f * s, ui.c.text, 400, false);
      pr.h = pad + 26 * s + 8 * s + hb + 10 * s + hk + 16 * s + 10 * s + hw + (hs ? hs + 6 * s : 0) + 14 * s + 32 * s + pad;
      pr.y = std::max(mainR.y + 24 * s, mainR.y + (mainR.h - pr.h) * 0.38f);
      ui.shadow(pr, 12 * s);
      ui.fill(pr, ui.c.panel2, 12 * s);
      ui.stroke(pr, ui.c.border, 12 * s);
      ui.popupRect(pr);
      float y = pr.y + pad;
      ui.icon("download", pr.x + pad + 10 * s, y + 12 * s, 20 * s, ui.c.accent);
      ui.text({pr.x + pad + 30 * s, y, tw - 30 * s, 26 * s}, "Get open-access PDFs", 16 * s, ui.c.text, AL_LEFT, 600);
      y += 26 * s + 8 * s;
      ui.textWrap({pr.x + pad, y, tw, hb}, body, 12.5f * s, ui.c.text);
      y += hb + 10 * s;
      Rect kr{pr.x + pad, y - 6 * s, tw, hk + 12 * s};
      ui.fill(kr, haveKey ? ui.c.ok.withA(0.10f) : ui.c.accent.withA(0.10f), 8 * s);
      ui.icon(haveKey ? "check" : "warn", kr.x + 14 * s, kr.y + kr.h / 2, 16 * s, haveKey ? ui.c.ok : ui.c.accent);
      ui.textWrap({kr.x + 28 * s, y, tw - 30 * s, hk}, keyLine, 12 * s, ui.c.text);
      y += hk + 16 * s + 10 * s;
      ui.textWrap({pr.x + pad, y, tw, hw}, where, 11.5f * s, ui.c.textDim);
      y += hw;
      if (hs) { y += 6 * s; ui.textWrap({pr.x + pad, y, tw, hs}, skip, 11.5f * s, ui.c.textDim); y += hs; }
      y += 14 * s;
      Rect br{pr.x + pad, y, tw, 32 * s};
      float bw = 118 * s;
      if (ui.button({br.r() - bw, br.y, bw, br.h}, n == 1 ? "Get the PDF" : "Get " + std::to_string(n) + " PDFs", BTN_PRIMARY, "download")) { ui.closePopup(); fetchPdfs(fetchAskRecs); return; }
      if (ui.button({br.r() - bw - 6 * s - 84 * s, br.y, 84 * s, br.h}, "Cancel")) { ui.closePopup(); return; }
      if (!haveKey && ui.button({br.x, br.y, 150 * s, br.h}, "Add a free key\xE2\x80\xA6", BTN_NORMAL, "settings")) { ui.closePopup(); openSettings(); setTab = 1; return; }
      if (!haveKey) ui.tip("A personal OpenAlex API key is free (openalex.org/settings/api). It also raises the OpenAlex import allowance.");
    });
  }
  // ---- summary of a run
  if (ui.isPopupOpen("fetchsum") && fetchRun) {
    ui.overlay([this, s, haveKey]() {
      auto run = fetchRun;
      int total, done, attached, closed, refused, failed, budget, cachedNoKey;
      bool cancelled;
      string dir;
      double remainingUsd;
      { std::lock_guard<std::mutex> lk(run->mu); total = run->total; done = run->done; attached = run->attached; closed = run->closed; refused = run->refused; failed = run->failed; budget = run->budget; cachedNoKey = run->cachedNoKey; cancelled = run->cancelled; dir = run->dir; remainingUsd = run->remainingUsd; }
      struct Line { string text; Color col; };
      vector<Line> lines;
      if (closed) lines.push_back({plural(closed, "paper") + " not openly available: attach by hand or read on the DOI page (your library's access applies there).", ui.c.textDim});
      if (refused) lines.push_back({plural(refused, "paper") + " open but refused by the site" + (cachedNoKey && !haveKey ? " \xE2\x80\x94 OpenAlex has a copy of " + std::to_string(cachedNoKey) + " of them: a free key would fetch those." : ".") , ui.c.text});
      if (budget) lines.push_back({plural(budget, "paper") + " waiting for OpenAlex's daily budget (resets at midnight UTC).", ui.c.textDim});
      if (failed) lines.push_back({plural(failed, "lookup") + " failed (network or unknown DOI): try again later.", ui.c.danger});
      if (cancelled && done < total) lines.push_back({std::to_string(total - done) + " not tried (stopped).", ui.c.textDim});
      if (remainingUsd >= 0) lines.push_back({"OpenAlex budget left today: $" + fmtFixed(remainingUsd, 2) + " of $1.00 (about " + std::to_string(int(remainingUsd * 100)) + " more cached copies; resets at midnight UTC).", ui.c.textFaint});
      Rect pr{0, 0, 440 * s, 0};
      pr.x = mainR.x + (mainR.w - pr.w) / 2;
      float pad = 18 * s, tw = pr.w - 2 * pad;
      vector<float> lh;
      float sum = 0;
      for (auto& l : lines) { float h = ui.textWrap({0, 0, tw - 16 * s, 0}, l.text, 12 * s, l.col, 400, false); lh.push_back(h); sum += h + 6 * s; }
      string where = attached ? "Saved in " + dir : "";
      float hw = where.empty() ? 0 : ui.textWrap({0, 0, tw, 0}, where, 11.5f * s, ui.c.text, 400, false);
      int toDo = closed + refused + budget + failed;
      pr.h = pad + 26 * s + 22 * s + 10 * s + sum + (hw ? hw + 8 * s : 0) + 12 * s + 32 * s + pad;
      pr.y = std::max(mainR.y + 24 * s, mainR.y + (mainR.h - pr.h) * 0.38f);
      ui.shadow(pr, 12 * s);
      ui.fill(pr, ui.c.panel2, 12 * s);
      ui.stroke(pr, ui.c.border, 12 * s);
      ui.popupRect(pr);
      float y = pr.y + pad;
      ui.icon(attached ? "check" : "warn", pr.x + pad + 10 * s, y + 12 * s, 20 * s, attached ? ui.c.ok : ui.c.accent);
      ui.text({pr.x + pad + 30 * s, y, tw - 30 * s, 26 * s}, cancelled ? "Get PDFs stopped" : "Get PDFs finished", 16 * s, ui.c.text, AL_LEFT, 600);
      y += 26 * s;
      ui.text({pr.x + pad + 30 * s, y, tw - 30 * s, 22 * s}, std::to_string(attached) + " of " + plural(total, "paper") + " attached and linked to their records", 12.5f * s, ui.c.textDim);
      y += 22 * s + 10 * s;
      for (size_t i = 0; i < lines.size(); i++) {
        ui.circle(pr.x + pad + 5 * s, y + 8 * s, 2.5f * s, lines[i].col, true, 1);
        ui.textWrap({pr.x + pad + 16 * s, y, tw - 16 * s, lh[i]}, lines[i].text, 12 * s, lines[i].col);
        y += lh[i] + 6 * s;
      }
      if (hw) { y += 2 * s; ui.textWrap({pr.x + pad, y, tw, hw}, where, 11.5f * s, ui.c.textDim); y += hw + 6 * s; }
      y += 12 * s;
      Rect br{pr.x + pad, y, tw, 32 * s};
      float x = br.r();
      if (ui.button({x - 76 * s, br.y, 76 * s, br.h}, "Close", BTN_PRIMARY)) { ui.closePopup(); return; }
      x -= 76 * s + 6 * s;
      if (toDo > 0) {
        if (ui.button({x - 150 * s, br.y, 150 * s, br.h}, "Show the " + std::to_string(toDo) + " to attach", BTN_NORMAL, "list")) { ui.closePopup(); fetchListOpen = true; if (readerOpen) closeReader(); if (papersOpen) papersOpen = false; page = PG_READ; return; }
        ui.tip("The Read page lists them with the reason and the links (DOI page, open-access link) to fetch them by hand.");
        x -= 150 * s + 6 * s;
      }
      if (attached && ui.button({x - 100 * s, br.y, 100 * s, br.h}, "Open folder", BTN_NORMAL, "folder")) { revealInExplorer(dir); }
      if (!haveKey && cachedNoKey && ui.button({br.x, br.y, 140 * s, br.h}, "Add a free key\xE2\x80\xA6", BTN_NORMAL, "settings")) { ui.closePopup(); openSettings(); setTab = 1; return; }
    });
  }
  // ---- one record: what happened, what to do
  if (ui.isPopupOpen("fetchrec")) {
    ui.overlay([this, s, haveKey]() {
      if (!hasCorpus() || fetchMenuRec < 0 || size_t(fetchMenuRec) >= P->corpus.recs.size()) { ui.closePopup(); return; }
      const int rec = fetchMenuRec;
      const Record& r = P->corpus.recs[size_t(rec)];
      const string doi = oa::normDoi(r.doi);
      const PdfItem* it = readerItemForRecord(rec);
      const bool hasFile = it && fileExistsU(it->path);
      const oa::FetchStatus* fs = nullptr;
      { auto f = P->library.fetch.find(PdfLibrary::keyOf(r)); if (f != P->library.fetch.end()) fs = &f->second; }
      string msg = hasFile ? (it->source.empty() ? "PDF attached." : "PDF attached from " + it->source.host + (it->source.version.empty() ? "" : " (" + it->source.versionLabel() + ")") + ".")
                 : fs ? fs->message(haveKey) : doi.empty() ? "No DOI on the record: nothing to look up. Attach the file by hand." : "Not fetched yet.";
      struct It { string label, key, icon; bool enabled = true; };
      vector<It> items;
      if (hasFile) items.push_back({"Open the PDF", "open", "book"});
      if (!doi.empty() && !hasFile) items.push_back({fs && fs->needsFile() ? "Try again" : "Get the open-access PDF", "get", "download", !busy()});
      items.push_back({hasFile ? "Attach another file\xE2\x80\xA6" : "Attach a file\xE2\x80\xA6", "attach", "plus"});
      if (fs && !fs->oaLink.empty() && !hasFile) items.push_back({"Open the open-access link", "oalink", "external"});
      if (!doi.empty()) items.push_back({"Open the DOI page", "doi", "external"});
      if (fs && fs->cachedAvailable && !haveKey && !hasFile) items.push_back({"Add a free OpenAlex key\xE2\x80\xA6", "key", "settings"});
      const float w = 320 * s, pad = 12 * s;
      float ht = ui.textWrap({0, 0, w - 2 * pad, 0}, r.title, 12.5f * s, ui.c.text, 600, false);
      ht = std::min(ht, 40 * s);
      float hm = ui.textWrap({0, 0, w - 2 * pad, 0}, msg, 11.5f * s, ui.c.text, 400, false);
      Rect pr{fetchMenuX, fetchMenuY, w, pad + ht + 4 * s + hm + 10 * s + 27 * s * float(items.size()) + 8 * s};
      if (pr.r() > float(g.W) - 8 * s) pr.x = float(g.W) - 8 * s - pr.w;
      if (pr.b() > float(g.H) - 8 * s) pr.y = std::max(8 * s, float(g.H) - 8 * s - pr.h);
      ui.shadow(pr, 8 * s);
      ui.fill(pr, ui.c.panel2, 8 * s);
      ui.stroke(pr, ui.c.border, 8 * s);
      ui.popupRect(pr);
      float y = pr.y + pad;
      ui.pushClip({pr.x, y, pr.w, ht});
      ui.textWrap({pr.x + pad, y, w - 2 * pad, ht}, r.title, 12.5f * s, ui.c.text, 600);
      ui.popClip();
      y += ht + 4 * s;
      Color mc = hasFile ? ui.c.ok : fs && fs->state == oa::FetchState::Refused ? ui.c.accent : fs && fs->state == oa::FetchState::Failed ? ui.c.danger : ui.c.textDim;
      ui.textWrap({pr.x + pad, y, w - 2 * pad, hm}, msg, 11.5f * s, mc);
      y += hm + 6 * s;
      ui.line(pr.x + 8 * s, y, pr.r() - 8 * s, y, ui.c.border, 1);
      y += 4 * s;
      for (auto& i : items) {
        Rect rr{pr.x + 4 * s, y, pr.w - 8 * s, 26 * s};
        bool click = i.enabled && ui.listRow(rr, "fetchrec:" + i.key, false);
        ui.icon(i.icon, rr.x + 14 * s, rr.y + rr.h / 2, 14 * s, i.enabled ? ui.c.textDim : ui.c.textFaint);
        ui.text({rr.x + 28 * s, rr.y, rr.w - 32 * s, rr.h}, i.label, 12.5f * s, i.enabled ? ui.c.text : ui.c.textFaint);
        y += 27 * s;
        if (!click) continue;
        ui.closePopup();
        if (i.key == "open") openReader(it->id);
        else if (i.key == "get") fetchAsk({rec}, false);
        else if (i.key == "attach") readerAttachDialog(rec);
        else if (i.key == "oalink") openUrl(fs->oaLink);
        else if (i.key == "doi") openUrl("https://doi.org/" + doi);
        else if (i.key == "key") { openSettings(); setTab = 1; }
        return;
      }
    });
  }
}

}  // namespace win
}  // namespace vs
