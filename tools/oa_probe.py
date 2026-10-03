#!/usr/bin/env python3
"""Open-access PDF fetching: a live probe of the services and of real downloads (docs/OA-FETCH-PLAN.md).

Answers, against the live services, the questions that decide the design:
  1. Which services need an e-mail / a key, and does the e-mail have to be registered anywhere?
  2. For a stratified sample of real DOIs (gold / hybrid / bronze / green / closed, three topics), which service
     knows a PDF URL, and does a plain HTTP client (like the application's WinHTTP) actually get a PDF from it?

Usage:  python3 tools/oa_probe.py [--out results.json] [--per 3] [--mail you@example.org] [--quick]
No credentials. Sequential, polite (small sleeps, one download at a time, 25 MB cap, 40 s timeout).
"""
import argparse, http.cookiejar, json, os, re, ssl, sys, time, urllib.parse, urllib.request, urllib.error
from http.client import IncompleteRead

APP_UA = "VOSStudio/1.16.0"                       # what WinHTTP sends from the application
BROWSER_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
              "Chrome/128.0.0.0 Safari/537.36")
LOGIN_HINTS = ("login", "idp.", "auth", "sso", "shibboleth", "signin", "sign-in", "openathens", "account")
MAX_BYTES = 25 * 1024 * 1024
TIMEOUT = 40
CTX = ssl.create_default_context()


def log(*a):
    print(*a, file=sys.stderr, flush=True)


class Stop(Exception):
    pass


class Redirects(urllib.request.HTTPRedirectHandler):
    """Records the chain. Login-looking hops are only noted: Springer's idp.springer.com/authorize hop is a cookie
    bounce that ends in the PDF, so the verdict is taken from the final bytes, never from the host names (run 1 of
    this probe stopped at such hops and under-counted Springer)."""
    def __init__(self):
        self.chain = []
        self.loginish = False

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        self.chain.append((code, newurl))
        host = urllib.parse.urlsplit(newurl).netloc.lower()
        path = urllib.parse.urlsplit(newurl).path.lower()
        if any(h in host or h in path for h in LOGIN_HINTS):
            self.loginish = True
        if len(self.chain) > 8:
            raise Stop("too many redirects")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(url, ua=APP_UA, accept="*/*", limit=MAX_BYTES, timeout=TIMEOUT, headers=None):
    """GET; returns dict(status, ctype, bytes, head(first 8 bytes), chain, err, final)."""
    r = Redirects()
    opener = urllib.request.build_opener(r, urllib.request.HTTPSHandler(context=CTX),
                                         urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    req = urllib.request.Request(url, headers={"User-Agent": ua, "Accept": accept, **(headers or {})})
    out = {"url": url, "status": 0, "ctype": "", "bytes": 0, "head": b"", "chain": [], "err": "", "final": url, "ms": 0}
    t0 = time.time()
    try:
        with opener.open(req, timeout=timeout) as resp:
            out["status"] = resp.status
            out["ctype"] = (resp.headers.get("Content-Type") or "").split(";")[0].strip()
            out["final"] = resp.geturl()
            buf = bytearray()
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                if not buf:
                    out["head"] = bytes(chunk[:8])
                buf += chunk
                if len(buf) > limit:
                    out["err"] = "over the size cap"
                    break
            out["bytes"] = len(buf)
            out["body"] = bytes(buf)
    except Stop as e:
        out["err"] = str(e)
    except urllib.error.HTTPError as e:
        out["status"] = e.code
        out["ctype"] = (e.headers.get("Content-Type") or "").split(";")[0].strip() if e.headers else ""
        try:
            out["body"] = e.read(4096)
        except Exception:
            out["body"] = b""
        out["err"] = "HTTP %d" % e.code
    except (urllib.error.URLError, TimeoutError, IncompleteRead, OSError, ValueError) as e:
        out["err"] = type(e).__name__ + ": " + str(e)[:120]
    out["chain"] = r.chain
    out["loginish"] = r.loginish
    out["ms"] = int((time.time() - t0) * 1000)
    return out


def get_json(url, ua=APP_UA, headers=None):
    f = fetch(url, ua=ua, accept="application/json", limit=4 * 1024 * 1024, timeout=30, headers=headers)
    if f["status"] == 200 and f.get("body"):
        try:
            return json.loads(f["body"].decode("utf-8", "replace")), f
        except ValueError:
            f["err"] = "not JSON"
    return None, f


def classify(f):
    """What a download attempt produced."""
    if f["err"] and f["status"] == 0:
        return "network: " + f["err"][:40]
    if f["status"] == 200:
        if f["err"]:
            return "truncated: " + f["err"][:40]  # the connection dropped mid-body: not a usable file
        if f["head"].startswith(b"%PDF-"):
            return "PDF"
        if "html" in f["ctype"] or f["head"].lstrip().lower().startswith((b"<!doc", b"<html")):
            return "login-page" if f.get("loginish") else "html-page"
        return "not-pdf (%s)" % (f["ctype"] or "?")
    return "HTTP %d" % f["status"] if f["status"] else "error"


# ----------------------------------------------------------------------------------------------- 1. the services
def probe_services(mail):
    res = {}
    doi = "10.1371/journal.pone.0166578"
    log("== services: e-mail / key requirements")
    # Unpaywall
    for label, q in [("no e-mail", ""), ("syntactically odd e-mail", "?email=a@b.c"),
                     ("unregistered e-mail (.invalid TLD)", "?email=nobody@nowhere.invalid"),
                     ("unregistered ordinary e-mail", "?email=" + urllib.parse.quote(mail))]:
        f = fetch("https://api.unpaywall.org/v2/%s%s" % (doi, q), accept="application/json", limit=1 << 20, timeout=30)
        body = (f.get("body") or b"")[:160].decode("utf-8", "replace").replace("\n", " ")
        res["unpaywall: " + label] = {"status": f["status"], "note": body}
        log("  unpaywall %-36s -> %s %s" % (label, f["status"], body[:90]))
        time.sleep(0.4)
    # OpenAlex
    for label, q in [("no mailto", ""), ("unregistered mailto", "?mailto=" + urllib.parse.quote(mail))]:
        f = fetch("https://api.openalex.org/works/doi:%s%s" % (doi, q), accept="application/json", limit=1 << 20, timeout=30)
        res["openalex: " + label] = {"status": f["status"], "note": f["err"] or "ok"}
        log("  openalex %-36s -> %s" % (label, f["status"]))
        time.sleep(0.4)
    # Semantic Scholar: single call, then a burst of 12 to see the shared pool's throttling
    f = fetch("https://api.semanticscholar.org/graph/v1/paper/DOI:%s?fields=title,openAccessPdf,externalIds" % doi,
              accept="application/json", limit=1 << 20, timeout=30)
    res["semanticscholar: no key"] = {"status": f["status"], "note": f["err"] or "ok"}
    log("  semanticscholar no key -> %s" % f["status"])
    codes = []
    for i in range(12):
        g = fetch("https://api.semanticscholar.org/graph/v1/paper/DOI:%s?fields=title" % doi, accept="application/json",
                  limit=1 << 20, timeout=30)
        codes.append(g["status"])
    res["semanticscholar: burst of 12 (no key)"] = {"status": codes[-1], "note": "codes " + ",".join(map(str, codes))}
    log("  semanticscholar burst -> %s" % codes)
    # CORE v3 without a key
    f = fetch('https://api.core.ac.uk/v3/search/works?q=doi:"%s"&limit=1' % doi, accept="application/json", limit=1 << 20, timeout=30)
    res["core.ac.uk v3: no key"] = {"status": f["status"], "note": (f.get("body") or b"")[:120].decode("utf-8", "replace")}
    log("  core v3 no key -> %s" % f["status"])
    # Europe PMC, arXiv, Crossref
    f = fetch("https://www.ebi.ac.uk/europepmc/webservices/rest/search?query=DOI:%s&format=json&resultType=lite" % doi,
              accept="application/json", limit=1 << 20, timeout=30)
    res["europepmc: no key"] = {"status": f["status"], "note": f["err"] or "ok"}
    log("  europepmc no key -> %s" % f["status"])
    f = fetch("http://export.arxiv.org/api/query?id_list=2303.08774&max_results=1", limit=1 << 20, timeout=30)
    res["arxiv api: no key"] = {"status": f["status"], "note": f["err"] or "ok"}
    log("  arxiv api no key -> %s" % f["status"])
    f = fetch("https://api.crossref.org/works/%s" % doi, accept="application/json", limit=1 << 20, timeout=30)
    res["crossref: no mailto"] = {"status": f["status"], "note": f["err"] or "ok"}
    log("  crossref no mailto -> %s" % f["status"])
    return res


# ----------------------------------------------------------------------------------------------- 2. the sample
FIXED = [
    ("10.1016/j.joi.2017.08.007", "fixed: bibliometrix (Elsevier)"),
    ("10.1007/s11192-009-0146-3", "fixed: VOSviewer (Springer)"),
    ("10.1371/journal.pone.0166578", "fixed: PLOS ONE"),
    ("10.1038/nature12373", "fixed: Nature bronze"),
    ("10.48550/arXiv.2303.08774", "fixed: arXiv DOI"),
    ("10.1162/qss_a_00019", "fixed: QSS (MIT Press)"),
]
TOPICS = [("bibliometric analysis", "bibliometrics"), ("graphene oxide membranes", "materials"),
          ("randomized controlled trial depression", "medicine")]
STATUSES = ["gold", "hybrid", "bronze", "green", "closed"]


def sample_dois(mail, per):
    """Crossref's random sample per topic (free, not metered), then OpenAlex's single-work lookup (free) for the OA
    status; up to `per` DOIs per topic x status cell. (OpenAlex search calls are metered since 2026 and answered 429
    during run 1.)"""
    out = []
    for term, tlabel in TOPICS:
        url = ("https://api.crossref.org/works?query.bibliographic=%s&filter=type:journal-article,from-pub-date:2018,until-pub-date:2024"
               "&sample=40&select=DOI&mailto=%s" % (urllib.parse.quote(term), urllib.parse.quote(mail)))
        js, f = get_json(url)
        if not js:
            log("  sample %s failed: %s" % (tlabel, f["err"]))
            continue
        cells = {st: 0 for st in STATUSES}
        for it in (js.get("message") or {}).get("items", []):
            d = (it.get("DOI") or "").lower()
            if not d or all(cells[st] >= per for st in STATUSES):
                continue
            w, f2 = get_json("https://api.openalex.org/works/doi:%s?select=open_access" % urllib.parse.quote(d))
            st = ((w or {}).get("open_access") or {}).get("oa_status")
            if st in cells and cells[st] < per:
                cells[st] += 1
                out.append((d, "%s/%s" % (tlabel, st)))
            time.sleep(0.15)
        log("  %s: %s" % (tlabel, ", ".join("%s %d" % kv for kv in cells.items())))
        time.sleep(0.5)
    return out


def resolve(doi, mail):
    """What each service says about the DOI. Returns dict(service -> {..}) and the ordered candidate URLs."""
    r = {}
    cands = []  # (url, source, version)

    def add(url, src, ver):
        if url and url not in [c[0] for c in cands]:
            cands.append((url, src, ver))

    # OpenAlex
    js, f = get_json("https://api.openalex.org/works/doi:%s?select=open_access,best_oa_location,locations,ids,primary_location,has_content&mailto=%s"
                     % (urllib.parse.quote(doi), urllib.parse.quote(mail)))
    if js:
        oa = js.get("open_access") or {}
        r["openalex_content_pdf"] = bool((js.get("has_content") or {}).get("pdf"))
        best = js.get("best_oa_location") or {}
        locs = [l for l in js.get("locations", []) if l.get("is_oa") and l.get("pdf_url")]
        r["openalex"] = {"is_oa": oa.get("is_oa"), "status": oa.get("oa_status"), "best_pdf": best.get("pdf_url"),
                         "n_pdf_locations": len(locs)}
        if best.get("pdf_url"):
            add(best["pdf_url"], "openalex:best(%s)" % ((best.get("source") or {}).get("display_name") or "?"), best.get("version"))
        order = {"publishedVersion": 0, "acceptedVersion": 1, "submittedVersion": 2, None: 3}
        for l in sorted(locs, key=lambda l: order.get(l.get("version"), 3)):
            add(l["pdf_url"], "openalex:%s" % ((l.get("source") or {}).get("display_name") or "?"), l.get("version"))
        ids = js.get("ids") or {}
        r["openalex"]["id"] = (ids.get("openalex") or "").rsplit("/", 1)[-1]
        if ids.get("pmcid"):
            r["openalex"]["pmcid"] = ids["pmcid"].rsplit("/", 1)[-1]
    else:
        r["openalex"] = {"error": f["err"] or ("HTTP %d" % f["status"])}
    time.sleep(0.3)
    # Unpaywall
    js, f = get_json("https://api.unpaywall.org/v2/%s?email=%s" % (doi, urllib.parse.quote(mail)))
    if js:
        best = js.get("best_oa_location") or {}
        r["unpaywall"] = {"is_oa": js.get("is_oa"), "status": js.get("oa_status"), "best_pdf": best.get("url_for_pdf"),
                          "n_locations": len(js.get("oa_locations") or [])}
        if best.get("url_for_pdf"):
            add(best["url_for_pdf"], "unpaywall:best(%s)" % best.get("host_type"), best.get("version"))
        for l in js.get("oa_locations") or []:
            if l.get("url_for_pdf"):
                add(l["url_for_pdf"], "unpaywall:%s" % l.get("host_type"), l.get("version"))
    else:
        r["unpaywall"] = {"error": f["err"] or ("HTTP %d" % f["status"])}
    time.sleep(0.3)
    # Semantic Scholar
    js, f = get_json("https://api.semanticscholar.org/graph/v1/paper/DOI:%s?fields=isOpenAccess,openAccessPdf,externalIds" % doi)
    if js:
        pdf = js.get("openAccessPdf") or {}
        ext = js.get("externalIds") or {}
        r["semanticscholar"] = {"is_oa": js.get("isOpenAccess"), "pdf": pdf.get("url"), "arxiv": ext.get("ArXiv"), "pmc": ext.get("PubMedCentral")}
        if pdf.get("url"):
            add(pdf["url"], "s2:openAccessPdf(%s)" % (pdf.get("status") or "?"), None)
        if ext.get("ArXiv"):
            add("https://arxiv.org/pdf/%s" % ext["ArXiv"], "arxiv:pdf", "submittedVersion")
    else:
        r["semanticscholar"] = {"error": f["err"] or ("HTTP %d" % f["status"])}
    time.sleep(0.3)
    # Europe PMC
    js, f = get_json("https://www.ebi.ac.uk/europepmc/webservices/rest/search?query=DOI:%s&format=json&resultType=core"
                     % urllib.parse.quote(doi))
    if js:
        res = (js.get("resultList") or {}).get("result") or []
        if res:
            w = res[0]
            urls = [u for u in ((w.get("fullTextUrlList") or {}).get("fullTextUrl") or []) if u.get("documentStyle") == "pdf"]
            r["europepmc"] = {"pmcid": w.get("pmcid"), "isOpenAccess": w.get("isOpenAccess"), "inPMC": w.get("inPMC"),
                              "pdf_urls": [u.get("url") for u in urls]}
            for u in urls:
                add(u["url"], "europepmc:%s" % u.get("availabilityCode"), None)
            if w.get("pmcid"):
                add("https://pmc.ncbi.nlm.nih.gov/articles/%s/pdf/" % w["pmcid"], "pmc:articles-pdf", None)
        else:
            r["europepmc"] = {"found": False}
    else:
        r["europepmc"] = {"error": f["err"] or ("HTTP %d" % f["status"])}
    return r, cands


def try_download(url, source):
    """App UA first; a browser UA only when the app UA failed (to learn whether the UA is what matters)."""
    a = fetch(url, ua=APP_UA, accept="application/pdf,*/*;q=0.8")
    ka = classify(a)
    rec = {"url": url, "source": source, "app": ka, "app_ms": a["ms"], "app_bytes": a["bytes"], "app_final": a["final"], "app_chain": len(a["chain"])}
    if ka != "PDF":
        time.sleep(0.6)
        b = fetch(url, ua=BROWSER_UA, accept="application/pdf,text/html;q=0.9,*/*;q=0.8")
        rec["browser"] = classify(b)
        rec["browser_bytes"] = b["bytes"]
    return rec


# ----------------------------------------------------------------------------------------------- 3. the cache
def probe_content(path, key):
    """Channel 1 of the design: OpenAlex's cached copies, downloaded with the user's key (environment variable
    OPENALEX_API_KEY, sent as a bearer header so that no URL in the record carries it). Reads the JSON written by
    a probe run, adds a "content" section, writes it back. Costs $0.01 per cached work."""
    js = json.load(open(path))
    rows = js["rows"]
    res = {"when": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "results": []}
    auth = {"Authorization": "Bearer " + key}
    rl, f = get_json("https://api.openalex.org/rate-limit", headers=auth)
    res["budget_before"] = {k: (rl.get("rate_limit") or {}).get(k) for k in ("daily_budget_usd", "daily_remaining_usd", "resets_in_seconds")} if rl else {"error": f["err"]}
    log("== OpenAlex content API with a key: budget %s" % res["budget_before"])
    # the error shapes first: no key, a wrong key, a work without content
    for label, url, hdr in (("no key", "https://content.openalex.org/works/W2550066059.pdf", None),
                            ("wrong key", "https://content.openalex.org/works/W2550066059.pdf", {"Authorization": "Bearer not-a-key"}),
                            ("no cached copy (W4402851183)", "https://content.openalex.org/works/W4402851183.pdf", auth),
                            ("unknown work (W1)", "https://content.openalex.org/works/W1.pdf", auth)):
        f = fetch(url, accept="application/pdf,*/*;q=0.8", headers=hdr, limit=64 * 1024)
        body = (f.get("body") or b"")[:160].decode("utf-8", "replace")
        res.setdefault("errors", []).append({"case": label, "status": f["status"], "ctype": f["ctype"], "body": body, "verdict": classify(f)})
        log("  %-32s -> %s %s %s" % (label, f["status"], f["ctype"], body.replace("\n", " ")[:100]))
        time.sleep(0.4)
    n = 0
    for r in rows:
        m = r["meta"]
        if not m.get("openalex_content_pdf"):
            continue
        wid = (m.get("openalex") or {}).get("id")
        if not wid:
            w, f2 = get_json("https://api.openalex.org/works/doi:%s?select=id" % urllib.parse.quote(r["doi"]), headers=auth)
            wid = ((w or {}).get("id") or "").rsplit("/", 1)[-1]
        if not wid:
            continue
        f = fetch("https://content.openalex.org/works/%s.pdf" % wid, accept="application/pdf,*/*;q=0.8", headers=auth)
        v = classify(f)
        rec = {"doi": r["doi"], "work": wid, "verdict": v, "status": f["status"], "bytes": f["bytes"], "ms": f["ms"], "direct": bool(r["got"])}
        if v == "PDF":
            rec["eof"] = b"%%EOF" in (f.get("body") or b"")[-2048:]
            with open("/tmp/oa_content_%s.pdf" % wid, "wb") as fh:  # for the engine check (tools/oa_report.py does not need them)
                fh.write(f["body"])
        res["results"].append(rec)
        n += 1
        log("  [%2d] %-36s %-14s %s  %7d KB %5d ms" % (n, r["doi"][:36], wid, v, f["bytes"] // 1024, f["ms"]))
        time.sleep(0.5)
    rl, f = get_json("https://api.openalex.org/rate-limit", headers=auth)
    res["budget_after"] = {k: (rl.get("rate_limit") or {}).get(k) for k in ("daily_budget_usd", "daily_remaining_usd", "resets_in_seconds")} if rl else {"error": f["err"]}
    got = sum(1 for x in res["results"] if x["verdict"] == "PDF")
    log("== cached copies: %d of %d downloaded as PDF; budget after %s" % (got, len(res["results"]), res["budget_after"]))
    js["content"] = res
    with open(path, "w") as fh:
        json.dump(js, fh, indent=1, default=str)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/oa_probe.json")
    ap.add_argument("--per", type=int, default=3, help="DOIs per topic x status cell")
    ap.add_argument("--mail", default="oa-probe@vosstudio.invalid")
    ap.add_argument("--quick", action="store_true", help="only the fixed DOIs")
    ap.add_argument("--max-cands", type=int, default=4)
    ap.add_argument("--content", action="store_true", help="only the content-API check on an existing --out file (needs OPENALEX_API_KEY in the environment)")
    a = ap.parse_args()
    if a.content:
        key = os.environ.get("OPENALEX_API_KEY", "").strip()
        if not key:
            sys.exit("set OPENALEX_API_KEY (free at https://openalex.org/settings/api)")
        probe_content(a.out, key)
        return
    results = {"when": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "mail": a.mail}
    results["services"] = probe_services(a.mail)
    dois = list(FIXED)
    if not a.quick:
        log("== sampling DOIs from OpenAlex")
        dois += sample_dois(a.mail, a.per)
    log("== %d DOIs" % len(dois))
    rows = []
    for i, (doi, label) in enumerate(dois):
        log("[%d/%d] %s  (%s)" % (i + 1, len(dois), doi, label))
        meta, cands = resolve(doi, a.mail)
        row = {"doi": doi, "label": label, "meta": meta, "candidates": len(cands), "attempts": [], "got": None}
        for url, src, ver in cands[: a.max_cands]:
            if "arxiv.org" in url:
                time.sleep(3)
            rec = try_download(url, src)
            rec["version"] = ver
            row["attempts"].append(rec)
            log("     %-38s app=%-18s %s" % (src[:38], rec["app"], ("browser=" + rec["browser"]) if "browser" in rec else ""))
            if rec["app"] == "PDF":
                row["got"] = {"source": src, "version": ver, "bytes": rec["app_bytes"], "by": "app-ua"}
                break
            if rec.get("browser") == "PDF" and not row["got"]:
                row["got"] = {"source": src, "version": ver, "bytes": rec["browser_bytes"], "by": "browser-ua"}
                break
            time.sleep(1.0)
        rows.append(row)
        time.sleep(0.5)
    results["rows"] = rows
    with open(a.out, "w") as f:
        json.dump(results, f, indent=1, default=str)
    # summary
    log("\n== summary")
    by = {}
    for r in rows:
        cell = r["label"].split(":")[0] if r["label"].startswith("fixed") else r["label"].split("/")[1]
        by.setdefault(cell, [0, 0, 0])
        by[cell][0] += 1
        if r["got"]:
            by[cell][1] += 1
            if r["got"]["by"] == "browser-ua":
                by[cell][2] += 1
    for k, v in by.items():
        log("  %-14s %d DOIs  -> %d PDFs (%d only with a browser UA)" % (k, v[0], v[1], v[2]))
    cached = sum(1 for r in rows if r["meta"].get("openalex_content_pdf"))
    either = sum(1 for r in rows if r["got"] or r["meta"].get("openalex_content_pdf"))
    log("  OpenAlex content API has a cached PDF for %d of %d (direct download or cached copy: %d)" % (cached, len(rows), either))
    log("written " + a.out)


if __name__ == "__main__":
    main()
