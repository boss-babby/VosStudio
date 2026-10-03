# Fetching open-access PDFs by DOI — research and design (for 1.18)

The Read stage attaches PDFs by hand: drop a file, *Attach…*, *Locate…*. The next step is **"Get PDF"**: for a
record with a DOI, find a legal open-access copy, download it into the project's attachments folder, attach and
link it — and, when there is none or the download did not produce a PDF, **say so clearly** and hand over to the
manual path (*Attach…*, the DOI link, the library's link resolver). This document records what was checked
against the live services on 30 September 2026, the decisions, and the design. **Built in 1.18.0** — what was
built, and where it differs, is in `docs/OA-FETCH.md`; this document is kept as the record of the research.
**Revised the same day after the 37-DOI live test** (`docs/OA-FETCH-TEST.md`, raw data `docs/oa-probe-results.json`,
tools `tools/oa_probe.py` / `tools/oa_report.py`): the measured yields, OpenAlex's metering and its content API,
and the redirect rule changed sections 1, 3.2, 3.3, 3.5, 3.6 and 4. Where this document and the test disagree, the
test is right.

## 1. What the services return (verified live)

All calls were made from this environment with plain HTTPS GETs and no credentials, on two DOIs: a bronze Nature
paper (`10.1038/nature12373`) and a gold PLOS ONE paper (`10.1371/journal.pone.0166578`), plus a closed Elsevier
paper (`10.1016/j.joi.2017.08.007`, *bibliometrix*) and a DOI that does not exist.

| Service | Call | What came back | Notes |
| --- | --- | --- | --- |
| **Unpaywall v2** | `GET https://api.unpaywall.org/v2/{doi}?email={mail}` | JSON: `is_oa`, `oa_status` (`gold/green/bronze/hybrid/closed`), `title`, `best_oa_location{url, url_for_pdf, url_for_landing_page, host_type (publisher/repository), version, license}`, `oa_locations[]` (same shape). Closed paper: `is_oa:false, oa_status:"closed", best_oa_location:null`. Unknown DOI: **HTTP 404 with an HTML body** (not JSON). | Email is mandatory (any string; ours: a per-install `vosstudio+<hash>@…` or the user's from settings). Limit 100 000 calls/day. The `/v2/search` endpoint was **retired on 18 September 2026** (410); OpenAlex is the announced successor "from the same team and the same data". |
| **OpenAlex** | `GET https://api.openalex.org/works/doi:{doi}?mailto={mail}&select=open_access,best_oa_location,locations` | `open_access{is_oa, oa_status, oa_url, any_repository_has_fulltext}`, `best_oa_location{pdf_url, landing_page_url, is_oa, version, license, source{display_name,…}}`, `locations[]` with `pdf_url` per location. For both OA test DOIs the `pdf_url`s were identical to Unpaywall's `url_for_pdf`. Unknown DOI: 404 JSON. | Already used by the app (data import, `app.cpp:1975`), same `get()` helper, same `mailto` politeness; the polite pool allows 10 req/s and 100 000/day. **Primary source for the feature.** Unpaywall as a second opinion only when OpenAlex has no location. |
| **Semantic Scholar** | `GET https://api.semanticscholar.org/graph/v1/paper/DOI:{doi}?fields=title,openAccessPdf,isOpenAccess,externalIds` | `isOpenAccess`, `openAccessPdf{url, status, license}`, `externalIds{ArXiv, PubMedCentral, PubMed, DOI,…}`. | Unauthenticated: a **shared** 1 000 req/s pool that is throttled under load; a free key gives 1 req/s dedicated. Useful for the **arXiv id and PMCID** it returns (both give reliable direct PDFs) more than for its own URL, which was the publisher URL again. Third source, optional. |
| **Europe PMC** | `GET https://www.ebi.ac.uk/europepmc/webservices/rest/search?query=DOI:{doi}&format=json&resultType=core` | `pmcid`, `isOpenAccess (Y/N)`, `inPMC`, `hasPDF`, `license`, `fullTextUrlList.fullTextUrl[] {availabilityCode: OA/S, documentStyle: pdf/html/doi, url}` — for the PLOS paper: `https://europepmc.org/articles/PMC5113961?pdf=render`. | Metadata is reliable; **the PDF render endpoints (`…?pdf=render`, `backend/ptpmcrender.fcgi`) answered 403 with an HTML page** to a non-browser client. Do not rely on them for the download; use the PMCID for the notice ("in PubMed Central: open in the browser"). |
| **PMC OA web service** | `…/pmc/utils/oa/oa.fcgi?id=PMC…` (old) and `https://pmc.ncbi.nlm.nih.gov/api/oa/v1/?id=PMC…` (new host) | Old URL 404; new URL answered with the HTML site shell, not XML, to a plain client. | Not usable without further work; skip in v1. |
| **arXiv** | `GET https://arxiv.org/pdf/{id}` | **200, `application/pdf`**, no redirect, no bot wall. | The most reliable direct PDF. The id comes from Semantic Scholar's `externalIds.ArXiv` or from an OpenAlex location whose source is arXiv (`pdf_url` = `https://arxiv.org/pdf/…`). arXiv asks for ≤ 1 request / 3 s; the app fetches one file at a time anyway. |
| **Crossref** | `GET https://api.crossref.org/works/{doi}` → `message.link[] {URL, content-type, intended-application}` | The Nature paper lists an `application/pdf` link — for **text-mining under a TDM licence**, not for readers. | Not an OA signal; do not use for downloading. |
| **Publisher "bronze" PDF** | `GET https://www.nature.com/articles/nature12373.pdf` | **303 → `idp.nature.com/authorize?…`**, i.e. a cookie/authorisation dance, no PDF for a non-browser client, although Unpaywall, OpenAlex and Semantic Scholar all call it open. | This is the case that must be reported honestly: *"reported as open access, but the automatic download did not return a PDF"*. |
| **Gold publisher PDF (PLOS)** | `GET https://journals.plos.org/plosone/article/file?id=…&type=printable` | **200, `application/pdf`, 1.17 MB, starts with `%PDF-`**. | The happy path. |

Conclusions from the measurements:

1. "Is OA" and "can be downloaded by a program" are different questions. The sources agree on the first; only a
   real download answers the second. The feature must therefore attempt the download and judge the **bytes**
   (`%PDF-` magic, `Content-Type`), never the metadata.
2. Repositories (arXiv, institutional repositories such as the Iowa State one above, PLOS, MDPI, Frontiers,
   eLife …) serve PDFs directly; large commercial publishers usually do not, even for bronze/hybrid articles
   (Cloudflare, cookie walls, `idp.` redirects). Try **every** OA location in a sensible order, not only the
   "best" one: publisher PDF of the published version first (if it works, it is the copy of record), then
   repository copies (`publishedVersion` > `acceptedVersion` > `submittedVersion`), arXiv when an id is known.
3. Every service wants a contact e-mail (Unpaywall requires it, OpenAlex's polite pool, arXiv's terms). Settings
   already hold the OpenAlex `mailto`; reuse it, and default to a generated per-install address when empty.

**Added by the 37-DOI test** (details and tables in `docs/OA-FETCH-TEST.md`):

4. Of 26 DOIs reported open, a plain client got **13 (50 %)**; the refusals are bot walls at Wiley, MDPI, Oxford,
   PMC, Europe PMC, IOP and a few repositories, and a browser `User-Agent` changed **nothing** (0 of 28 retries).
   The services were right about the status every time (OpenAlex and Unpaywall agreed on all 36 answers).
5. **OpenAlex's content API** (`has_content.pdf`, `content_urls.pdf` → `https://content.openalex.org/works/{W}.pdf?api_key=…`)
   holds a cached copy for **23 of the 26**, including every MDPI/Wiley/Oxford article that answered 403. It
   **requires an API key** (401 without one — verified; the download itself was not exercised here), costs $0.01
   per file against a free key's $1/day budget (≈ 100 PDFs a day). With it, 88 % of the open DOIs are obtainable.
6. **OpenAlex meters requests** (2026): single-work lookups by DOI are free and unmetered; `filter=doi:a|b|…`
   batches cost $0.10 per 1 000 calls, searches $1 per 1 000; without a key the budget is $0.10 a day *shared per
   IP address*, and anonymous searches were refused with 429 (*"…or use a free API key"*). `mailto` no longer
   buys anything. The app's existing `openalexKey` setting (sent as `api_key`) is the answer for the importer
   as much as for this feature.
7. **Redirects**: Springer's `idp.springer.com/authorize` hop is a cookie bounce that ends in the PDF; Nature's
   `idp.nature.com` hop ends in an HTML page. Follow every redirect (WinHTTP does, with cookies) and judge only
   the first bytes of the final answer — no host-name rules.
8. **arXiv DataCite DOIs** (`10.48550/arXiv.<id>`) are unknown to all three services; derive `arxiv.org/pdf/<id>`
   from the DOI itself.

## 2. Legal footing

Only locations that the services classify as open access are downloaded: Unpaywall/OpenAlex list a location
only when a licence, a repository deposit or the publisher's free-to-read flag says so. The app never scrapes
publisher HTML, never follows a login redirect, never uses Sci-Hub-like sources, never sends institutional
credentials, and never fetches the Crossref TDM links. The downloaded copy is kept locally in the user's project
like any file they attached by hand; the source URL, the OA status, the version (published / accepted /
submitted) and the licence are stored with the item and shown in the *Paper* pane, so the reader always knows
whether they are looking at the version of record or an author manuscript. Bulk fetching is throttled (one
download at a time, ≤ 2 metadata requests per second, an arXiv gap of 3 s) so that a 500-paper library never
looks like a crawler.

## 3. Design

### 3.1 Where it lives

- **Read page › reading list**: a *Get PDF* icon on every linked record without a file, and a **Get PDFs…** button
  above the list ("for the 43 records of the map that have a DOI and no PDF") that runs the bulk job with a
  progress line and a final summary.
- **Papers table**: the row menu gets *Get PDF*, the multi-selection menu *Get PDFs for the selection*.
- **Reader › Paper pane**: for an item whose file is missing, *Get PDF* next to *Locate…*.
- **Assistant**: `getpdf [record ids | selection | all]` and `getpdfstatus`, so the AI can run it and report — with
  the usual approval gate, because it writes files and talks to the network.

### 3.2 The single-record procedure (a `Job`, off the UI thread)

```
resolve(doi):
  0. DOI of the form 10.48550/arXiv.<id> → candidates = [https://arxiv.org/pdf/<id>] (no service knows it)
  1. OpenAlex  works?filter=doi:a|b|…  (≤ 50 per call; api_key when set, else mailto)
     select=id,doi,open_access,best_oa_location,locations,ids,has_content,content_urls
     cached    = has_content.pdf ? content_urls.pdf : none              (channel 1, needs the key)
     candidates = [loc.pdf_url for loc in locations if loc.is_oa and loc.pdf_url]  (+ best first)  (channel 2)
     arxiv     = ids.arxiv / a location whose source is arXiv                                       (channel 3)
  2. if no candidates and !open_access.is_oa → Unpaywall v2 (second opinion; add its url_for_pdf/oa_locations)
  3. if still nothing → Semantic Scholar externalIds (ArXiv → https://arxiv.org/pdf/{id}; PMC id → note only)
  order: cached copy (if key) → published > accepted > submitted, publisher before repository → arXiv
download(candidates):
  for each url (max 5):  GET, User-Agent "VOSStudio/<ver> (mailto:…)", 60 s, ≤ 40 MB; redirects followed with
  cookies (WinHTTP's default, ≤ 10 hops) — no host-name rules;
  the FIRST chunk decides: starts with "%PDF-" → stream the rest to disk; anything else → drop the connection
  and record the reason (HTML page, HTTP status, not a PDF, too large, timeout, 401/402/429 from the content API)
  on the first accepted body: write attachments/<Author>_<Year>_<short-title>_<doi-hash>.pdf, verify it opens
  in the engine (page count ≥ 1) on the worker; then PdfLibrary::add + link(recordKey) + item.source = {url,
  host ("OpenAlex (cached copy)" | publisher | repository | arXiv), version, license, oa_status, fetched (ISO)}
```

Budget awareness (OpenAlex): read `X-RateLimit-Remaining` / `X-RateLimit-Reset` on every answer and `Retry-After`
on 429; back off 2, 4, 8 s; when the daily budget is gone, finish the run with channels 2–3 only and count the
records that "can be fetched from OpenAlex's cache after midnight UTC"; 401 from the content API = key missing
or wrong, said as such.

The download uses the existing `httpRequest()` streaming callback (`platform.h`) so that the size limit and the
`%PDF-` check happen on the first chunk, and the body never goes through a `string` twice. The file name is the
same scheme as *Attach…* uses, in an `attachments` folder next to the project file (the folder that
`docs/PDF-READER.md` lists as "next"; this feature is the reason to build it — a fetched copy must live inside
the project, not in Downloads).

### 3.3 Outcomes and how the user is told

Every attempt ends in exactly one of these states, stored on the record (`PdfLibrary` gets a small
`FetchStatus {state, when, detail, landingUrl, pmcid}` keyed by record) and shown as a coloured dot with a
tooltip in the reading list and the Papers table:

| State | Meaning | What the user sees / can do |
| --- | --- | --- |
| **Attached** | A PDF was downloaded, verified, attached and linked. | Toast "PDF attached — accepted manuscript from arXiv" (version and host named); the row gets the file icon; *Open* reads it. |
| **Not open access** | All sources say closed (`is_oa:false`, no locations). | Row note "Not openly available"; buttons **Attach manually…** (file dialog, pre-linked to the record) and **Open DOI page** (browser; the library's proxy/link resolver takes it from there when the user is on campus). Optional setting: a link-resolver base URL (`https://<resolver>?url_ver=Z39.88-2004&rft_id=info:doi/{doi}`) to add a third button *Find via library*. |
| **Reported open, download failed** | The services list a PDF but no candidate returned a PDF (HTML page, 403, timeout). | Row note "Open access, but the site refused the automatic download — open it in the browser and drop the file here": **Open the OA link** (the best `pdf_url`, in the browser, where cookies and JavaScript make it work) and **Attach manually…**; the note names the sites and what they answered ("onlinelibrary.wiley.com: HTTP 403; pmc.ncbi.nlm.nih.gov: an HTML page"). When OpenAlex has a cached copy and no key is set, the note says so first: **"OpenAlex has a copy: add a free OpenAlex API key in Preferences and try again"** with a button to the setting — the one action that turns the most common refusals (MDPI, Wiley, Oxford, PMC) into successes. |
| **Budget used up** | The OpenAlex daily budget (key or anonymous) is spent (429/402). | The run continues with the direct channels; the summary says "N more can be fetched from OpenAlex's cache after midnight UTC" and offers *Retry those tomorrow*. |
| **No DOI** | The record has no DOI. | Not offered; the bulk summary counts them ("12 records without a DOI were skipped"). |
| **Lookup failed** | Network down, 5xx, rate-limited (429). | "Could not reach OpenAlex/Unpaywall — try again later"; the bulk job pauses on 429 with back-off (2, 4, 8 s) and stops after three consecutive failures instead of hammering. |

The bulk job ends with one summary card (not one toast per paper): *"31 attached · 9 not openly available · 3
open but refused · 12 without DOI"*, with **Show the ones to attach manually**, which filters the reading list to
those records. Nothing is ever downloaded silently over an existing attachment: a record that already has a
file is skipped unless the user asks *Replace with the published version*.

### 3.4 What is stored (project file)

```json
"library": { "items": [ { "id": "p7", "path": "attachments/Aria_2017_bibliometrix_3f9a.pdf",
  "source": { "url": "https://…", "host": "arXiv", "version": "acceptedVersion", "license": "cc-by",
              "oa_status": "green", "fetched": "2026-09-30T10:12:00Z" } } ],
  "fetch": { "<recordKey>": { "state": "closed|refused|failed|attached", "when": "…", "detail": "…",
             "landing": "https://doi.org/…", "pmcid": "PMC…" } } }
```

Older projects load without these keys; nothing else changes shape.

### 3.5 Performance and footprint

- Metadata: one OpenAlex call per 50 records (`filter=doi:a|b|c…`, as the importer already does at
  `app.cpp:1975`), so the *lookup* for 500 records is 10 requests ($0.001 of budget); Unpaywall and Semantic
  Scholar only for the records OpenAlex could not place. Cached downloads cost $0.01 each: a 500-record list is
  at most $5 — five days of a free key's budget, or a prepaid balance; the bulk job says before it starts how many
  will come from the cache and how many directly, and never spends budget on a record that already has a file.
- Expected yield, to be honest in the UI: about half of the open papers directly, about nine in ten with a free
  key (the 37-DOI sample; `docs/OA-FETCH-TEST.md`), none of the closed ones — and the closed ones are recognised
  from the lookup, so no download is attempted for them.
- Downloads: one at a time, streamed to disk; the UI thread only receives progress messages; memory is one
  64 KB chunk buffer. The engine's verification (open + page count) runs on the PDF worker like every other
  document and closes the document at once.
- The Read page keeps the list virtualised (1.16) — a bulk job that adds 300 items does not change the cost of
  a frame.

### 3.6 Settings

*Preferences › Reading*: contact e-mail for OA lookups (defaults to the OpenAlex `mailto` already there); the
**OpenAlex API key** (the existing `openalexKey` setting — one key for the importer's searches and for the
cached copies; a line under it says what it buys: "free at openalex.org/settings/api; about 100 PDFs a day and
ten times the search budget"); "Prefer the published version even when a repository copy is available" (on);
size limit (40 MB); optional library link-resolver URL; "Also ask Semantic Scholar" (off by default — the shared
pool is throttled).

## 4. Effort and order

1. Attachments folder + copy-into-project for manual attachments (prerequisite; ½ day).
2. `oa.cpp` in `src/core`: resolvers (OpenAlex incl. `has_content`/`content_urls` and the rate-limit headers,
   Unpaywall, Semantic Scholar parsers), the arXiv-DOI rule, candidate ordering across the three channels, the
   first-chunk `%PDF-` acceptance rule, the status model — pure functions on strings, unit-tested against the
   JSON recorded in `docs/oa-probe-results.json` (`tests/oa_test.cpp`) (1 day).
3. The download job on `httpRequest` (first-chunk decision, streaming to disk, budget/back-off); per-record and
   bulk UI; the summary card; the "add a key" notice; Papers-table and reader entry points; assistant commands
   and help (1½ days).
4. First run with a real OpenAlex key against the 23 cached DOIs of the sample (the one channel this environment
   could not exercise); docs and the release notes.

The service behaviour recorded in section 1 is the part that ages; the resolvers should log the raw JSON of a
failing lookup (behind *Diagnostics*) so that field changes are visible without a debugger.
