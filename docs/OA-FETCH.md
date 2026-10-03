# Get PDF — open-access copies by DOI (1.18)

*Read › **Get PDFs…***, the **Get PDFs** button and the PDF cells of the papers table, *Read › Get open-access
PDFs…* in the menu, the assistant's `get_pdfs` tool and the script command `getpdf` all do the same thing: for the
records that have a DOI and no PDF file yet, find a legal open-access copy, download it into the project's
attachments folder, attach it to the reading library and link it to the record. When a paper is not open, or the
site refused the automatic download, the record keeps a **precise notice** and the manual path is one click away
(*Attach a file…*, the DOI page, the open-access link).

This document describes what was built. The research and the measurements it rests on are in
`docs/OA-FETCH-PLAN.md` (design) and `docs/OA-FETCH-TEST.md` (a live test on 37 real DOIs, 30 September 2026,
raw data in `docs/oa-probe-results.json`, tools `tools/oa_probe.py` / `tools/oa_report.py`).

## 1. What to expect

| the paper is … | without an OpenAlex key | with a free OpenAlex key |
|---|---|---|
| open access (gold, hybrid, green, bronze) | about **half** come through — the publishers' sites often refuse automatic downloads (Wiley, MDPI, OUP, IOP, PubMed Central answer 403 or an HTML page) | about **nine in ten** — OpenAlex serves its own cached copy first, without bot walls, at about $0.01 of the key's free $1/day budget (≈ 100 PDFs a day) |
| on arXiv (also arXiv's own DOIs `10.48550/arXiv.…`) | yes, always (arXiv serves PDFs to everyone) | yes |
| closed access | no — the record says *Not openly available* and offers the DOI page (your library's access applies there) and *Attach a file…* | no |
| without a DOI | nothing to look up; the record says so | — |

The key is the same one the OpenAlex import uses (*Preferences › OpenAlex*, free at openalex.org/settings/api).
Nothing is ever downloaded from behind a paywall, the user's institutional access is not used, and the app never
pretends to be a browser (the test showed a browser user agent gains nothing anyway).

## 2. How a paper is fetched

1. **Where are the copies?** One OpenAlex request per fifty DOIs (`works?filter=doi:a|b|…`, `select` limited to
   the open-access fields; the key travels in an `Authorization: Bearer` header, never in a URL). A paper OpenAlex
   does not know, or calls closed, is asked to **Unpaywall** (`/v2/{doi}?email=…`) as a second opinion; a paper
   nobody lists as open is asked to **Semantic Scholar** for an arXiv id or a PMCID. arXiv's own DOIs are turned
   into `arxiv.org/pdf/<id>` directly (no service knows them).
2. **In which order?** OpenAlex's cached copy first when a key is set (for *every* open work, because the
   `has_content` flag lags behind the cache — Heliyon was served although flagged false); then the open locations,
   the **published version before the accepted manuscript before the preprint**, the publisher before repositories
   within a version; then arXiv; PubMed Central's endpoints last (they refused every non-browser request in the
   test). At most six URLs per paper.
3. **Is it a PDF?** The first kilobyte decides: `%PDF-` within it, or the connection is dropped and nothing is
   written — an HTML page, a cookie wall or a JSON error never lands on disk. Limits: 60 s per file, 40 MB.
   A dropped connection (fewer bytes than `Content-Length`) is retried once.
4. **Is it whole?** `%%EOF` in the last 2 KB, size against `Content-Length`, and the engine opens it on the reader's
   worker thread (a file PDFium cannot open is discarded; when the engine is not available the byte checks
   decide).
5. **Attach and link.** The file is stored as `<Surname>_<Year>_<four-title-words>_<hash>.pdf` in the attachments
   folder (below), added to the reading library, linked to the record, and the item remembers its **source**
   (URL, host, kind, version, licence, OA status, time). A record that already had an item whose file went missing
   gets the new path instead of a second item.
6. **Say what happened.** Every record that ends without a file keeps a status (section 4).

Politeness: 300 ms between papers, three seconds between arXiv requests, exponential back-off (2/4/8 s) on
OpenAlex 429s, one download at a time; the run is the application's single background job (status-bar progress
and *Cancel*; the Read page shows the paper being fetched and a *Stop* button).

## 3. Where the files go

`<project folder>\<project name> attachments\` when the project has been saved; otherwise
`Documents\VOSStudio\Attachments`. The confirm card says which before the run starts (and reminds to save the
project to keep the files next to it). *Open folder* on the summary opens it in Explorer.

## 4. The outcomes, and what the user sees

| state | meaning | the message (without a key) |
|---|---|---|
| `attached` | a file is attached and linked; the item's *source* says where it came from | toast / *PDF attached from PLOS (published version)* |
| `closed` | the services agree there is no open copy | *Not openly available — attach the file by hand or open the DOI page (your library's access applies there).* |
| `refused` | open access, but no URL produced a PDF; the detail lists each attempt (`onlinelibrary.wiley.com: refused (HTTP 403); pmc.ncbi.nlm.nih.gov: sent a web page instead of the PDF`) | *Open access, but the site refused the automatic download. OpenAlex has a copy: add a free OpenAlex API key in Preferences and try again — or open the link in the browser and drop the file here.* (the key sentence only when OpenAlex has a copy and no key is set) |
| `budget` | only OpenAlex's cached copy was available and today's budget is used up | *OpenAlex's daily budget is used up; the cached copy can be fetched after midnight UTC.* |
| `failed` | the lookup itself failed (network, an unknown DOI) | *The lookup failed: … — try again later.* |
| `nodoi` | the record has no DOI | *No DOI on the record: nothing to look up. Attach the file by hand.* |

Where they show:

- **Read page › Without a PDF (n)** — a collapsible list under the buttons: title, a one-line reason, coloured by
  state (refused in accent, failed in red, closed faint). A click (or right-click) opens the record's card. Below
  it: *Try again* (all of them; the closed ones are left alone) and either *Add a free key…* (when OpenAlex has a
  copy of some) or *Clear the list*.
- **Papers table › PDF column** — a *shield* for closed, a *warning* for refused/failed, the message in the
  tooltip; a click on any cell without a file opens the record's card.
- **The record's card** — the title, the message, then *Get the open-access PDF* / *Try again*, *Attach a file…*,
  *Open the open-access link*, *Open the DOI page*, *Add a free OpenAlex key…* (when it would help).
- **The summary** after a bulk run — attached / not open / refused (with how many OpenAlex could serve with a key)
  / waiting for the budget / failed / not tried, the folder, what is left of the OpenAlex budget today, and
  *Show the n to attach* (opens the Read page list).
- **Before a bulk run** — a confirm card: how many records, what the run does, the key hint with the measured
  yields, the folder, how many records without a DOI are skipped; *Add a free key…* leads to the settings tab.

## 5. Data model

```
"library": { …,
  "items": [ { …, "source": { "url": "https://content.openalex.org/works/W4378379138.pdf", "host": "OpenAlex",
                              "kind": "cache", "version": "publishedVersion", "license": "cc-by",
                              "oaStatus": "gold", "fetched": "2026-09-30T11:02:41Z" } } ],
  "fetch": { "<record key>": { "state": "refused", "when": "…", "detail": "onlinelibrary.wiley.com: refused (HTTP 403)",
                               "landing": "https://doi.org/10.1002/asi.24345", "oaLink": "https://…/pdf/10.1002/asi.24345",
                               "oaStatus": "hybrid", "cached": true }, … } }
```

`kind` is `cache | publisher | repository | arxiv | pmc`. `fetch` holds only records whose last run did not
end with a file that is still there (an `attached` entry is written too, for the source line). Keys are the
bibliography keys (`refKeyFor(doi, title, year)`), so the statuses survive a re-import of the same data.

## 6. The assistant and scripts

`get_pdfs {"papers": "all|selection|R3, R7", "retry": false}` — a *change* tool (asks unless everything is
allowed); it waits for the run and reports the counts, the folder, how many refused papers OpenAlex could have
served with a key, and the per-record summary. `reading_list` now ends with the records-and-PDFs summary and the
first fifteen papers without a PDF with their reason. Script command: `getpdf [all|R3,R7] [retry]`.

## 7. Code

- `src/core/oa.h/.cpp` — the pure part, unit-tested in `tests/oa_test.cpp` on the recorded answers of the
  services: DOI normalisation, the arXiv-DOI rule, the parsers (OpenAlex works list, Unpaywall, Semantic
  Scholar), `order()`, the acceptance rules (`pdfStart`, `pdfEnd`, `looksLikeHtml`), OpenAlex's rate-limit
  headers, the attempt descriptions, `FetchStatus` (JSON round trip, the messages) and the attachment name.
- `src/core/library.h/.cpp` — `PdfSource`, `PdfItem::source`, `PdfLibrary::fetch` (JSON in `tests/library_test.cpp`).
- `src/win/platform.cpp` — `httpDownload()`: a WinHTTP download to a file whose first chunk is shown to an
  `accept` callback before anything is written; final URL, content type/length and named response headers.
- `src/win/fetch.cpp` — the job (resolve → order → download → verify → attach), the confirm card, the summary
  and the record card; `src/win/readlib.cpp` — the Read page's progress box and *Without a PDF* list;
  `src/win/papers.cpp` — the table's button and PDF cells; `src/win/agent.cpp` — the tool and the command.

## 8. Verified and not verified

Verified here (Linux, 30 September 2026): the OpenAlex content channel with a personal key — 23 of 23 cached
copies downloaded (`200 application/pdf`, every file opened by PDFium, $0.01 each); the parsers and the order
against the recorded answers of OpenAlex, Unpaywall and Semantic Scholar for the 37-DOI sample; the acceptance
rules on real PDF, HTML and JSON bodies; the JSON round trips; the whole application compiles for Windows and all
21 host test programs pass. **Not run:** the Windows build itself (no Windows machine here) — the in-app flow
(job, cards, Read page list, papers table cells, the assistant tool) is reviewed, not exercised.
