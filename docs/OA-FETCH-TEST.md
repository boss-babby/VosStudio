# Fetching open-access PDFs by DOI — the live test (30 September 2026)

`docs/OA-FETCH-PLAN.md` designed the *Get PDF* feature from a handful of hand-picked DOIs. Before building it,
the plan's assumptions were **measured against the live services and the live publisher sites** with
`tools/oa_probe.py`; this document is the write-up. The raw record of every request is
`docs/oa-probe-results.json`; the tables below are generated from it by `tools/oa_report.py` and can be
regenerated after a new run (`python3 tools/oa_probe.py --out docs/oa-probe-results.json --per 3 --mail you@example.org`).

## 1. Method

- **Sample.** 6 fixed DOIs (the *bibliometrix* and *VOSviewer* papers, a PLOS ONE gold article, a bronze Nature
  article, an arXiv DataCite DOI, a *Quantitative Science Studies* article) plus a random sample: for three
  topics — *bibliometric analysis*, *graphene oxide membranes*, *randomized controlled trial depression* — 40
  journal articles from 2018–2024 drawn with Crossref's `sample=` parameter (free, not metered), each classified
  by OpenAlex's single-work lookup into gold / hybrid / bronze / green / closed, up to 3 per topic and status.
  Green is under-represented (one SSRN paper): a random draw from Crossref rarely lands on a paper whose only
  open copy is a repository deposit. 37 DOIs in total.
- **Lookups.** For every DOI: OpenAlex (`works/doi:…`, with `has_content`), Unpaywall v2, Semantic Scholar
  Graph, Europe PMC. Every PDF URL they returned was collected (OpenAlex best location first, then the other
  OpenAlex locations by version, Unpaywall's, Semantic Scholar's, arXiv when an arXiv id was known, Europe PMC's
  render links and PMC's `articles/PMC…/pdf/`), de-duplicated, at most 4 per DOI.
- **Downloads.** Plain HTTPS `GET` with the application's `User-Agent` (`VOSStudio/1.17.0`), redirects followed
  (up to 8, with cookies, as WinHTTP does by default), 60 s, 40 MB cap. A result counts as a **PDF only when the
  body starts with `%PDF-`**. Every failed attempt was repeated once with a Chrome `User-Agent` to see whether
  the UA is what the sites look at. arXiv was asked at most once every 3 s.
- **The cache pass.** `tools/oa_probe.py --content` re-read the results and downloaded every work with
  `has_content.pdf` from `content.openalex.org` with a free OpenAlex API key (taken from the environment, sent
  as a bearer header, never written to the record), then opened each file with PDFium.
- **Where from.** This sandbox's network (a cloud address). A home or campus network can fare *better* with
  sites that rate cloud ranges harshly, and a campus network additionally gets subscription PDFs through IP
  recognition — both are outside what an application can promise, so the numbers below are the floor.
- **A first run** stopped at redirects to hosts that looked like a login (`idp.`, `auth`, `login`…) and
  under-counted Springer: `link.springer.com/content/pdf/….pdf` answers with a hop through
  `idp.springer.com/authorize` that is a **cookie bounce, not a login**, and ends in the PDF. The second run
  (this one) follows every redirect and judges only the final bytes; the design was changed accordingly (§4).

## 2. Results

### Yield by open-access status (37 DOIs, 2026-09-30)

| status | DOIs | PDF downloaded directly | of which publisher / repository / arXiv / PMC | cached copy at OpenAlex (content API) | either | neither |
|---|---:|---:|---|---:|---:|---:|
| gold | 9 | 4 | 4 / 0 / 0 / 0 | 8 | 8 | 1 |
| hybrid | 5 | 2 | 2 / 0 / 0 / 0 | 4 | 4 | 1 |
| bronze | 7 | 3 | 2 / 1 / 0 / 0 | 7 | 7 | 0 |
| green | 1 | 0 | 0 / 0 / 0 / 0 | 0 | 0 | 1 |
| closed | 9 | 0 | 0 / 0 / 0 / 0 | 0 | 0 | 9 |
| fixed | 6 | 4 | 3 / 0 / 1 / 0 | 4 | 4 | 2 |
| **all** | 37 | 13 | | 23 | 23 | 14 |

Of the 26 DOIs that at least one service reports as open access, 13 (50 %) were downloaded directly and 23 (88 %) are downloadable directly or from OpenAlex's cache.

### What each site answered to a plain HTTP client (every attempt, application User-Agent)

| host | attempts | PDF | HTTP 403 | other HTTP error | HTML page instead | login page | network / size |
|---|---:|---:|---:|---:|---:|---:|---:|
| pmc.ncbi.nlm.nih.gov | 6 | 0 | 0 | 0 | 6 | 0 | 0 |
| europepmc.org | 5 | 0 | 5 | 0 | 0 | 0 | 0 |
| onlinelibrary.wiley.com | 4 | 0 | 4 | 0 | 0 | 0 | 0 |
| doi.org | 3 | 1 | 0 | 0 | 2 | 0 | 0 |
| mdpi.com | 3 | 0 | 3 | 0 | 0 | 0 | 0 |
| dergipark.org.tr | 2 | 2 | 0 | 0 | 0 | 0 | 0 |
| academic.oup.com | 1 | 0 | 1 | 0 | 0 | 0 | 0 |
| arxiv.org | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| cambridge.org | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| eelet.org.uk | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| hdl.handle.net | 1 | 0 | 1 | 0 | 0 | 0 | 0 |
| healtheconomicsreview.biomedcentral.com | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| iopscience.iop.org | 1 | 0 | 0 | 0 | 1 | 0 | 0 |
| jahonline.org | 1 | 0 | 1 | 0 | 0 | 0 | 0 |
| journal.esrgroups.org | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| journals.plos.org | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| link.springer.com | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| mitpressjournals.org | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| nature.com | 1 | 0 | 0 | 0 | 0 | 1 | 0 |
| repository.ubn.ru.nl | 1 | 0 | 1 | 0 | 0 | 0 | 0 |
| rss.onlinelibrary.wiley.com | 1 | 0 | 1 | 0 | 0 | 0 | 0 |
| russianlawjournal.org | 1 | 0 | 1 | 0 | 0 | 0 | 0 |
| trialsjournal.biomedcentral.com | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| utoronto.scholaris.ca | 1 | 1 | 0 | 0 | 0 | 0 | 0 |

### Does a browser User-Agent help?

28 failed attempts were repeated with a Chrome User-Agent; 0 of them then produced a PDF.

### What the services said

| service | answered | notes |
|---|---:|---|
| OpenAlex (single work by DOI) | 36 / 37 | free of charge and unmetered; `has_content.pdf` names the works with a cached copy |
| Unpaywall v2 | 36 / 37 | needs an `email` parameter, no registration |
| Semantic Scholar Graph | 36 / 37 | no key; shared pool |
| Europe PMC | 37 / 37 | found in PMC for the medical DOIs; its own PDF endpoints refused the client |

OpenAlex and Unpaywall agree on the OA status of 36 DOIs and disagree on 0.

Candidate PDF links per DOI: median 1, max 5; 12 DOIs had none (closed or arXiv-only DOIs).

Downloaded PDFs: median 0.9 MB, largest 5.3 MB; median time 2.0 s, slowest 11.9 s.

### OpenAlex's cached copies, downloaded with a free API key (2026-09-30)

23 works had `has_content.pdf`; **23 of 23 downloaded as a PDF** and 23 of them opened in the engine with a page count (every downloaded file was opened with PDFium (page count > 0)).
 Median 0.9 MB in 0.8 s (largest 34.0 MB); budget before 0.99, after 0.75 (the daily budget of a free key is $1; a download costs $0.01, a HEAD request too).

| case | answer |
|---|---|
| no key | HTTP 401 application/json `{"error":"API key required","message":"Content downloads require an API key. Get one free at https://openalex.` |
| wrong key | HTTP 401 application/json `{"error":"Invalid or missing API key","message":"API key not found"}` |
| no cached copy (W4402851183) | HTTP 200 application/pdf `%PDF-1.7.%..................................1321 0 obj.<< /T 7852702 /L 7879278 /Linearized 1 /E 232076 /O 132` |
| unknown work (W1) | HTTP 404 application/json `{"error":"Work not found in content index","work_id":"W1"}` |

| DOI | work | direct download had | cached copy | pages | title |
|---|---|---|---|---:|---|
| 10.1007/s11192-009-0146-3 | W2150220236 | succeeded | PDF, 0.9 MB, 0.7 s | 16 |  |
| 10.1371/journal.pone.0166578 | W2550066059 | succeeded | PDF, 1.2 MB, 0.7 s | 14 | Identification of the Core Set of Carbon-Associate |
| 10.1038/nature12373 | W2159974629 | succeeded | PDF, 1.0 MB, 0.6 s | 7 | Nanometre-scale thermometry in a living cell |
| 10.1162/qss_a_00019 | W3001554335 | succeeded | PDF, 0.5 MB, 0.9 s | 10 | Scopus as a curated, high-quality bibliometric dat |
| 10.52783/rlj.v11i2.3796 | W4404870646 | failed | PDF, 0.5 MB, 0.8 s | 9 |  |
| 10.52783/eel.v14i1s.1350 | W4395074853 | succeeded | PDF, 0.8 MB, 0.8 s | 19 |  |
| 10.56130/tucbis.1178247 | W4309183660 | succeeded | PDF, 0.9 MB, 0.7 s | 8 |  |
| 10.52783/jisem.v9i4.42 | W4411738445 | succeeded | PDF, 1.1 MB, 0.7 s | 21 |  |
| 10.17755/esosder.950426 | W4224247975 | succeeded | PDF, 1.3 MB, 0.8 s | 23 |  |
| 10.52783/jes.2137 | W4395042902 | succeeded | PDF, 0.7 MB, 0.8 s | 8 |  |
| 10.3390/membranes12050447 | W4224312710 | failed | PDF, 34.0 MB, 1.0 s — the probe's first read stopped after 26.3 of 34.0 MB (a dropped connection on this network); a second download was complete: 34,014,776 bytes, 18 pages | 18 | Construct -FeOOH-Reduced Graphene Oxide Aerogel as |
| 10.3390/membranes14020032 | W4391231754 | failed | PDF, 4.3 MB, 0.7 s | 13 | Facilitating Water Permeation in Graphene Oxide Me |
| 10.3390/membranes13110874 | W4388304784 | failed | PDF, 1.9 MB, 0.9 s | 22 | Adsorptive Membranes Incorporating Ionic Liquids ( |
| 10.1088/2053-1591/ab1ffd | W2944506514 | failed | PDF, 1.3 MB, 0.9 s | 9 | Sonication effect on graphene oxide (GO) membranes |
| 10.1002/admi.201970081 | W2954840823 | failed | PDF, 4.3 MB, 1.0 s | 1 | Graphene Oxide Membranes: Pressure‐Driven Solven |
| 10.1002/aic.17865 | W4292170648 | failed | PDF, 3.5 MB, 0.9 s | 18 | Transport properties of graphene oxide nanofiltrat |
| 10.1186/s13561-020-00273-0 | W3034687053 | succeeded | PDF, 1.0 MB, 0.7 s | 11 | Health economic evaluation of an internet interven |
| 10.1186/s13063-019-3815-4 | W2998958161 | succeeded | PDF, 0.9 MB, 1.0 s | 14 | Metta-based group meditation and individual cognit |
| 10.1002/da.22788 | W2887982140 | failed | PDF, 0.6 MB, 0.7 s | 11 | Mindfulness‐based cognitive therapy for patients |
| 10.1002/da.23249 | W4220948872 | failed | PDF, 0.9 MB, 0.7 s | 13 | Meditation‐based lifestyle modification in mild  |
| 10.1016/j.eurpsy.2018.05.010 | W2807356290 | succeeded | PDF, 0.6 MB, 0.8 s | 6 | Metacognitive Training for Depression (D-MCT) redu |
| 10.1093/ejcts/ezae297 | W4401386110 | failed | PDF, 0.9 MB, 0.7 s | 7 | Gender gap in cardiothoracic surgery randomized co |
| 10.1016/j.jadohealth.2018.10.030 | W2910518544 | succeeded | PDF, 0.1 MB, 0.5 s | 2 | 16. Mobile Phone-Based Peer Support In The Prevent |

### Every DOI

| DOI | cell | OpenAlex / Unpaywall status | result | source | OpenAlex cache |
|---|---|---|---|---|---|
| 10.1016/j.joi.2017.08.007 | bibliometrix (Elsevier) | closed / closed | no PDF link | — | no |
| 10.1007/s11192-009-0146-3 | VOSviewer (Springer) | hybrid / hybrid | PDF (0.9 MB) | openalex:best(Scientometrics) | yes |
| 10.1371/journal.pone.0166578 | PLOS ONE | gold / gold | PDF (1.2 MB) | openalex:best(PLoS ONE) | yes |
| 10.1038/nature12373 | Nature bronze | bronze / bronze | PDF (2.4 MB) | openalex:arXiv (Cornell University) | yes |
| 10.48550/arXiv.2303.08774 | arXiv DOI | ? / ? | no PDF link | — | no |
| 10.1162/qss_a_00019 | QSS (MIT Press) | gold / gold | PDF (0.5 MB) | openalex:best(Quantitative Science Studies) | yes |
| 10.36106/ijsr/7121954 | bibliometrics/closed | closed / closed | no PDF link | — | no |
| 10.52783/rlj.v11i2.3796 | bibliometrics/gold | gold / gold | HTTP 403 | — | yes |
| 10.52783/eel.v14i1s.1350 | bibliometrics/gold | gold / gold | PDF (0.8 MB) | openalex:best(?) | yes |
| 10.56130/tucbis.1178247 | bibliometrics/bronze | bronze / bronze | PDF (0.9 MB) | openalex:best(Türkiye Coğrafi Bilgi Sistemleri Dergisi) | yes |
| 10.3727/108354222x16534530194787 | bibliometrics/closed | closed / closed | no PDF link | — | no |
| 10.1108/md-01-2018-0085 | bibliometrics/closed | closed / closed | no PDF link | — | no |
| 10.52783/jisem.v9i4.42 | bibliometrics/gold | gold / gold | PDF (1.1 MB) | openalex:best(?) | yes |
| 10.17755/esosder.950426 | bibliometrics/hybrid | hybrid / hybrid | PDF (1.3 MB) | openalex:best(Elektronik Sosyal Bilimler Dergisi) | yes |
| 10.52783/jes.2137 | bibliometrics/hybrid | hybrid / hybrid | PDF (0.7 MB) | openalex:best(Journal of Electrical Systems) | yes |
| 10.3390/membranes12050447 | materials/gold | gold / gold | HTTP 403; html-page | — | yes |
| 10.3390/membranes14020032 | materials/gold | gold / gold | HTTP 403; html-page | — | yes |
| 10.3390/membranes13110874 | materials/gold | gold / gold | HTTP 403; html-page | — | yes |
| 10.1016/j.envres.2021.111576 | materials/closed | closed / closed | no PDF link | — | no |
| 10.1002/smll.201901023 | materials/closed | closed / closed | no PDF link | — | no |
| 10.1016/j.desal.2022.115601 | materials/closed | closed / closed | no PDF link | — | no |
| 10.1088/2053-1591/ab1ffd | materials/bronze | bronze / bronze | html-page | — | yes |
| 10.1002/admi.201970081 | materials/bronze | bronze / bronze | HTTP 403 | — | yes |
| 10.1002/aic.17865 | materials/bronze | bronze / bronze | HTTP 403 | — | yes |
| 10.2139/ssrn.4041239 | materials/green | green / green | no PDF link | — | no |
| 10.1037/emo0001328.supp | medicine/closed | closed / closed | no PDF link | — | no |
| 10.1016/j.explore.2023.04.012 | medicine/hybrid | hybrid / hybrid | html-page | — | no |
| 10.1212/wnl.0000000000011470 | medicine/closed | closed / closed | no PDF link | — | no |
| 10.1186/s13561-020-00273-0 | medicine/gold | gold / gold | PDF (1.0 MB) | openalex:best(Health Economics Review) | yes |
| 10.1212/wnl.0000000000011473 | medicine/closed | closed / closed | no PDF link | — | no |
| 10.4103/jfmpc.jfmpc_396_18 | medicine/gold | gold / gold | HTTP 403; html-page | — | no |
| 10.1186/s13063-019-3815-4 | medicine/gold | gold / gold | PDF (0.9 MB) | openalex:best(Trials) | yes |
| 10.1002/da.22788 | medicine/hybrid | hybrid / hybrid | HTTP 403 | — | yes |
| 10.1002/da.23249 | medicine/hybrid | hybrid / hybrid | HTTP 403 | — | yes |
| 10.1016/j.eurpsy.2018.05.010 | medicine/bronze | bronze / bronze | PDF (0.6 MB) | openalex:best(European Psychiatry) | yes |
| 10.1093/ejcts/ezae297 | medicine/bronze | bronze / bronze | HTTP 403 | — | yes |
| 10.1016/j.jadohealth.2018.10.030 | medicine/bronze | bronze / bronze | PDF (5.3 MB) | openalex:TSpace (University of Toronto) | yes |

## 3. Findings

1. **"Open access" is a statement about rights; "downloadable by a program" is a statement about the web
   server.** Of the 26 DOIs that the services call open, a plain client obtained **13 (50 %)**. The services
   themselves were right about the status every time (OpenAlex and Unpaywall agreed on all 36 answers, and no
   closed paper had a PDF link) — what fails is the last step.
2. **What refuses.** The refusals are *sites*, not *kinds of paper*: Wiley (5 of 5 attempts, 403), MDPI (3 of 3,
   403 — even for a fully gold journal), Oxford (403), the Royal Statistical Society's Wiley host (403),
   PubMed Central's `articles/PMC…/pdf/` (6 of 6: an HTML interstitial), Europe PMC's render endpoints (5 of 5,
   403), IOP (an HTML page), an Elsevier hybrid article (HTML page), two repositories (a Handle resolver and a Radboud
   repository, 403), a Medknow journal (403). These are bot walls (Cloudflare and the like) that look at the TLS
   handshake and JavaScript, **not at the `User-Agent`: the Chrome UA changed nothing in 28 retries.** Pretending
   to be a browser is therefore pointless as well as dishonest, and the app will not do it.
3. **What serves.** Springer (with the cookie bounce), PLOS, BioMed Central, Cambridge, MIT Press, arXiv, DergiPark,
   university repositories (Toronto's TSpace, Dundee's Discovery in the first run), and the small open journals
   that make up much of the "bibliometric analysis" literature. Downloads were quick and small (median 0.9 MB,
   2 s; the largest 5.3 MB in 12 s).
4. **OpenAlex keeps its own copy of most of what the sites refuse.** `has_content.pdf` was true for **23 of the
   26 open DOIs — including every MDPI, Wiley and Oxford article that answered 403** — and
   `content_urls.pdf` points at `https://content.openalex.org/works/{W}.pdf`. With direct downloads *and* that
   cache, **23 of 26 (88 %)** open DOIs are obtainable; the three that are not are the SSRN preprint (no PDF
   link anywhere), one Medknow article and one Elsevier hybrid article. **The content API requires an API key**
   (verified: 401 *"Content downloads require an API key. Get one free at openalex.org/users"* without one);
   a free key has a budget of $1 a day and a download costs $0.01, so **about 100 PDFs a day** cost nothing —
   enough for a reading list, not for a corpus. **Verified with a free key in a second pass: all 23 cached
   copies downloaded as PDFs** (median 0.9 MB in 0.8 s, one 34 MB file in 1.5 s), all 23 open in the engine with
   a page count; the whole pass cost $0.24 of the day's $1. The answers to the failure cases are exact JSON
   (401 *"API key required"*, 401 *"API key not found"*, 404 *"Work not found in content index"*), and a
   work whose `has_content.pdf` was still false (the Heliyon article) nevertheless came back as a PDF — the
   flag lags the index, so the app tries the cache for every open work when a key is set, at $0.01 per hit and
   nothing for a 404. Two more lessons from that pass: a `HEAD` request costs the same $0.01 as a download
   (never probe with HEAD), and one 34 MB transfer was cut by this network at 26 MB on the first try — the
   engine check before attaching (page count > 0) is what catches a truncated file, and a retry completed it.
5. **OpenAlex is metered now, and `mailto` no longer counts.** Single-work lookups by DOI are free and unmetered
   (36 of 37 answered instantly, the 37th is the arXiv DOI that no service indexes). List-and-filter calls
   (`filter=doi:a|b|c`, the batch form the importer uses) cost $0.10 per 1 000 and *search* calls $1 per 1 000,
   against an anonymous budget of **$0.10 a day shared by everyone behind the same IP address**, and anonymous
   *search* was refused outright during the first run (*"Anonymous search is temporarily rate-limited while the
   search cluster is under elevated load … or use a free API key"*, 429 with `retryAfter`). A free key raises the
   budget tenfold and makes it personal. The app already has an *OpenAlex API key* setting (`openalexKey`) and
   sends it as `api_key`; the importer's semantic and full-text searches are what suffers without it.
6. **Unpaywall** needs an `email` parameter but no registration (422 without one, 200 with any address);
   **Semantic Scholar** answered a burst of 12 without a key; **Europe PMC** and **Crossref** ask nothing. One
   contact e-mail in the settings covers all of them; only OpenAlex benefits from a key.
7. **arXiv DataCite DOIs** (`10.48550/arXiv.2303.08774`) are unknown to OpenAlex, Unpaywall and Semantic Scholar
   (404 from all three). The arXiv id is in the DOI itself; the resolver must derive `https://arxiv.org/pdf/{id}`
   from it directly.
8. **Redirect chains** must be followed to the end and judged by content: the Springer `idp.` hop ends in a PDF,
   the Nature `idp.nature.com` hop ends in the HTML article page. A host-name rule gets one of them wrong
   whichever way it is written; the `%PDF-` rule gets both right.

## 4. What this changes in the design (`docs/OA-FETCH-PLAN.md`, revised)

- **Three channels in this order**, per record:
  1. **OpenAlex's cached copy** — when an OpenAlex key is set and the work is open (`has_content.pdf` marks the
     sure cases; the flag lags the index, and a miss is a free 404): `content.openalex.org/works/{W}.pdf`, one
     request, no bot walls, the copy OpenAlex indexed for the work's best open location. The item records
     `source.host = "OpenAlex (cached copy)"` and the version/licence OpenAlex reports for that location.
  2. **The open locations themselves** (publisher and repositories, published > accepted > submitted), redirects
     followed with cookies, the first chunk must begin with `%PDF-`, otherwise the connection is dropped at once
     — an HTML answer costs one small read, not a download.
  3. **arXiv**, from an OpenAlex/Semantic Scholar arXiv id or from a `10.48550/arXiv.` DOI.
- **Without a key** the app works through channels 2 and 3 and, for a refused paper that OpenAlex has cached,
  says so precisely: *"The publisher's site refused the automatic download. OpenAlex has a copy: add a free
  OpenAlex API key in Preferences and try again, or open the link in the browser and drop the file here."* That
  turns the most common failure (MDPI, Wiley, Oxford, PMC) into a one-time action.
- **Budget awareness.** Every OpenAlex answer carries `X-RateLimit-Remaining`, `X-RateLimit-Reset` and, on 429,
  `Retry-After`; the bulk job reads them, backs off (2, 4, 8 s), and when the daily budget is gone it finishes
  the run with channels 2–3 only and reports *"N more can be fetched from OpenAlex's cache after midnight UTC"*
  instead of failing every remaining record. 401 on the content API means the key is missing or wrong and is
  said as such.
- **No browser impersonation, no host-name heuristics.** The `User-Agent` stays `VOSStudio/<version>` with the
  contact e-mail; a refusal is reported as a refusal with the site named, and the *Open the OA link* button hands
  the same URL to the user's browser, where cookies and JavaScript make it work.
- **Expected yield to tell the user** (from this sample; the notice texts say "usually"): about half of the open
  papers directly, about nine in ten with a free OpenAlex key, none of the closed ones — and the closed ones are
  recognised before any download is tried, so a 500-record run spends its time only on the papers that can come.
- **Costs**: for a 500-record list, 10 batch lookups ($0.001), up to 500 cached downloads ($5 — five days of the
  free budget, or the paid balance), direct downloads free. The bulk job says how many will come from the cache
  before it starts.

## 5. Not verified here

- Behaviour from a home or campus network, and WinHTTP's TLS handshake versus Python's — Cloudflare may treat
  them differently in either direction. The client-side rule (judge the bytes, name the site, offer the browser)
  is right in both cases.
- Semantic Scholar under sustained load (its unauthenticated pool is shared), Unpaywall's daily cap (100 000),
  and the OpenAlex budget numbers over a full day; all three are handled by the same back-off and the same
  honest notice.
