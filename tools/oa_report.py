#!/usr/bin/env python3
"""Turns the JSON written by oa_probe.py into the Markdown tables of docs/OA-FETCH-TEST.md.

    python3 tools/oa_report.py docs/oa-probe-results.json > /tmp/tables.md
"""
import collections, json, sys, urllib.parse


def host(url):
    h = urllib.parse.urlsplit(url).netloc.lower()
    return h[4:] if h.startswith("www.") else h


def kind(source):
    s = source.lower()
    if "arxiv" in s:
        return "arXiv"
    if "pmc" in s or "europepmc" in s:
        return "PMC"
    if "best(" in s or "publisher" in s:
        return "publisher"
    return "repository"


def status_of(row):
    lab = row["label"]
    if lab.startswith("fixed"):
        return "fixed"
    return lab.split("/")[1]


def main():
    js = json.load(open(sys.argv[1]))
    rows = js["rows"]
    order = ["gold", "hybrid", "bronze", "green", "closed", "fixed"]
    print("### Yield by open-access status (%d DOIs, %s)\n" % (len(rows), js["when"][:10]))
    print("| status | DOIs | PDF downloaded directly | of which publisher / repository / arXiv / PMC | cached copy at OpenAlex (content API) | either | neither |")
    print("|---|---:|---:|---|---:|---:|---:|")
    tot = collections.Counter()
    for st in order:
        rs = [r for r in rows if status_of(r) == st]
        if not rs:
            continue
        got = [r for r in rs if r["got"]]
        k = collections.Counter(kind(r["got"]["source"]) for r in got)
        cached = [r for r in rs if r["meta"].get("openalex_content_pdf")]
        either = [r for r in rs if r["got"] or r["meta"].get("openalex_content_pdf")]
        print("| %s | %d | %d | %d / %d / %d / %d | %d | %d | %d |" % (st, len(rs), len(got), k["publisher"], k["repository"], k["arXiv"], k["PMC"], len(cached), len(either), len(rs) - len(either)))
        tot.update({"n": len(rs), "got": len(got), "cached": len(cached), "either": len(either)})
    print("| **all** | %d | %d | | %d | %d | %d |" % (tot["n"], tot["got"], tot["cached"], tot["either"], tot["n"] - tot["either"]))
    oa = [r for r in rows if status_of(r) not in ("closed",) and ((r["meta"].get("openalex") or {}).get("is_oa") or (r["meta"].get("unpaywall") or {}).get("is_oa"))]
    oa_got = [r for r in oa if r["got"]]
    oa_either = [r for r in oa if r["got"] or r["meta"].get("openalex_content_pdf")]
    print("\nOf the %d DOIs that at least one service reports as open access, %d (%.0f %%) were downloaded directly and %d (%.0f %%) are downloadable directly or from OpenAlex's cache.\n"
          % (len(oa), len(oa_got), 100.0 * len(oa_got) / max(1, len(oa)), len(oa_either), 100.0 * len(oa_either) / max(1, len(oa))))

    # per host
    print("### What each site answered to a plain HTTP client (every attempt, application User-Agent)\n")
    print("| host | attempts | PDF | HTTP 403 | other HTTP error | HTML page instead | login page | network / size |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    byhost = collections.defaultdict(collections.Counter)
    for r in rows:
        for a in r["attempts"]:
            h = host(a["url"])
            c = byhost[h]
            c["n"] += 1
            v = a["app"]
            if v == "PDF":
                c["pdf"] += 1
            elif v == "HTTP 403":
                c["403"] += 1
            elif v.startswith("HTTP"):
                c["http"] += 1
            elif v == "html-page":
                c["html"] += 1
            elif v == "login-page":
                c["login"] += 1
            else:
                c["net"] += 1
    for h, c in sorted(byhost.items(), key=lambda kv: (-kv[1]["n"], kv[0])):
        print("| %s | %d | %d | %d | %d | %d | %d | %d |" % (h, c["n"], c["pdf"], c["403"], c["http"], c["html"], c["login"], c["net"]))

    # UA effect
    ua_only = [(r["doi"], a["source"]) for r in rows for a in r["attempts"] if a["app"] != "PDF" and a.get("browser") == "PDF"]
    ua_tried = sum(1 for r in rows for a in r["attempts"] if "browser" in a)
    print("\n### Does a browser User-Agent help?\n")
    print("%d failed attempts were repeated with a Chrome User-Agent; %d of them then produced a PDF%s.\n"
          % (ua_tried, len(ua_only), (": " + ", ".join("%s (%s)" % x for x in ua_only)) if ua_only else ""))

    # services
    print("### What the services said\n")
    n = len(rows)
    ok = collections.Counter()
    agree = disagree = 0
    for r in rows:
        m = r["meta"]
        for k in ("openalex", "unpaywall", "semanticscholar", "europepmc"):
            if m.get(k) and "error" not in m[k]:
                ok[k] += 1
        a, b = (m.get("openalex") or {}).get("status"), (m.get("unpaywall") or {}).get("status")
        if a and b:
            if a == b:
                agree += 1
            else:
                disagree += 1
    print("| service | answered | notes |")
    print("|---|---:|---|")
    print("| OpenAlex (single work by DOI) | %d / %d | free of charge and unmetered; `has_content.pdf` names the works with a cached copy |" % (ok["openalex"], n))
    print("| Unpaywall v2 | %d / %d | needs an `email` parameter, no registration |" % (ok["unpaywall"], n))
    print("| Semantic Scholar Graph | %d / %d | no key; shared pool |" % (ok["semanticscholar"], n))
    print("| Europe PMC | %d / %d | found in PMC for the medical DOIs; its own PDF endpoints refused the client |" % (ok["europepmc"], n))
    print("\nOpenAlex and Unpaywall agree on the OA status of %d DOIs and disagree on %d.\n" % (agree, disagree))
    cands = [r["candidates"] for r in rows]
    print("Candidate PDF links per DOI: median %d, max %d; %d DOIs had none (closed or arXiv-only DOIs).\n" % (sorted(cands)[len(cands) // 2], max(cands), sum(1 for c in cands if c == 0)))

    # sizes / timing
    got = [r["got"] for r in rows if r["got"]]
    if got:
        sizes = sorted(g["bytes"] for g in got)
        ms = sorted(a["app_ms"] for r in rows for a in r["attempts"] if a["app"] == "PDF")
        print("Downloaded PDFs: median %.1f MB, largest %.1f MB; median time %.1f s, slowest %.1f s.\n" % (sizes[len(sizes) // 2] / 1e6, sizes[-1] / 1e6, ms[len(ms) // 2] / 1e3, ms[-1] / 1e3))

    # the content API, when the run had a key
    c = js.get("content")
    if c:
        rs = c["results"]
        got = [r for r in rs if r["verdict"] == "PDF"]
        print("### OpenAlex's cached copies, downloaded with a free API key (%s)\n" % c["when"][:10])
        print("%d works had `has_content.pdf`; **%d of %d downloaded as a PDF** and %d of them opened in the engine with a page count (%s)."
              % (len(rs), len(got), len(rs), sum(1 for r in rs if r.get("pages")), c.get("engine_check", "")))
        ms = sorted(r["ms"] for r in got)
        sz = sorted(r["bytes"] for r in got)
        if got:
            print(" Median %.1f MB in %.1f s (largest %.1f MB); budget before %s, after %s (the daily budget of a free key is $1; a download costs $0.01, a HEAD request too)."
                  % (sz[len(sz) // 2] / 1e6, ms[len(ms) // 2] / 1e3, sz[-1] / 1e6, c.get("budget_before", {}).get("daily_remaining_usd"), c.get("budget_after", {}).get("daily_remaining_usd")))
        print("\n| case | answer |")
        print("|---|---|")
        for e in c.get("errors", []):
            body = "".join(ch if 32 <= ord(ch) < 127 else "." for ch in e["body"]).replace("|", "/")[:110]
            print("| %s | HTTP %s %s `%s` |" % (e["case"], e["status"], e["ctype"], body))
        print("\n| DOI | work | direct download had | cached copy | pages | title |")
        print("|---|---|---|---|---:|---|")
        for r in rs:
            note = (" — " + r["note"]) if r.get("note") else ""
            print("| %s | %s | %s | %s, %.1f MB, %.1f s%s | %s | %s |" % (r["doi"], r["work"], "succeeded" if r["direct"] else "failed", r["verdict"], r["bytes"] / 1e6, r["ms"] / 1e3, note, r.get("pages", "?"), (r.get("title") or "").replace("|", "/")[:60]))
        print()

    # per-DOI list
    print("### Every DOI\n")
    print("| DOI | cell | OpenAlex / Unpaywall status | result | source | OpenAlex cache |")
    print("|---|---|---|---|---|---|")
    for r in rows:
        m = r["meta"]
        st = "%s / %s" % ((m.get("openalex") or {}).get("status") or "?", (m.get("unpaywall") or {}).get("status") or "?")
        if r["got"]:
            res, src = "PDF (%.1f MB)" % (r["got"]["bytes"] / 1e6), r["got"]["source"]
        elif not r["attempts"]:
            res, src = "no PDF link", "—"
        else:
            res, src = "; ".join(sorted(set(a["app"] for a in r["attempts"]))), "—"
        print("| %s | %s | %s | %s | %s | %s |" % (r["doi"], r["label"].replace("fixed: ", ""), st, res, src.replace("|", "/"), "yes" if m.get("openalex_content_pdf") else "no"))


if __name__ == "__main__":
    main()
