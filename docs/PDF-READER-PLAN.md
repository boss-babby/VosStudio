# An in-application PDF reader — design (not built)

Status: **design only**, written for 1.13.0 at the user's request. Nothing in this document is implemented; the
file exists so that the reader can be built later without re-deriving the decisions, and so that its consequences
for the rest of the application (memory, project format, agent tools, writer) are known in advance.

## 1. Why a reader, and why not a separate tool

The map tells the researcher *which* papers matter; reading them today means leaving the application (Zotero,
Acrobat, a browser) and losing the link between the record on the map, the passage read and the sentence written
about it in the writer. A reader inside the application closes that loop: a highlight made while reading becomes
a quotation with a native citation in the writer in one gesture, a figure clipped from a paper becomes a figure
block, notes are searchable next to the record, and the map can be navigated *from* the reading ("which cluster is
this paper in", "what cites it"). Zotero is the reference for the interaction design (highlight colours, sticky
notes, tags, annotation export), not for the architecture — VOSStudio stays a single native executable.

## 2. Engine

| candidate | licence | render | text layer (positions) | annotations | verdict |
|---|---|---|---|---|---|
| **PDFium** (Chromium's engine) | BSD-3 | yes, fast, anti-aliased | `FPDFText_*`: characters with boxes, search, links | create/edit `FPDFAnnot_*`, save `FPDF_SaveAsCopy` | **chosen** |
| Windows.Data.Pdf (WinRT, in the OS) | none needed | yes | **no** text layer | no | render-only preview fallback at most |
| MuPDF | AGPL (or commercial) | excellent | yes | yes | licence incompatible with a free MIT-licensed app that may later be dual-used |
| Poppler | GPL | good | yes | limited | licence; heavy dependency tree (cairo, freetype, fontconfig) |
| Foxit / PDFTron | commercial | — | — | — | out for a free application |

PDFium is shipped as `pdfium.dll` (the maintained builds at <https://github.com/bblanchon/pdfium-binaries>,
`win-x64`, ~8 MB, BSD) **loaded at run time with `LoadLibrary`**: the executable stays self-contained, a missing
DLL means the reader says *"PDF reading needs pdfium.dll next to VOSStudio.exe — download"* and everything else
works. A static MinGW link of PDFium is not realistic (it is built with GN/clang and depends on Skia-adjacent
code); the DLL route is what every non-Chromium consumer of PDFium does. The installer (`installer/VOSStudio.iss`)
would offer the DLL as an optional component.

Threading: PDFium is not thread-safe across documents; the plan uses **one worker thread that owns all PDFium
calls** with a job queue (render page tile, extract text, search, save), results posted to the UI thread as
bitmaps / structs. This is also what keeps the UI at its current smoothness — the UI thread never waits on a page.

### 2a. Why not an engine of our own (as the writer is)

The writer is our own code because a document editor's core — a paragraph model, a layout, an undo history — is a
few thousand lines that we want to control completely. A PDF *reader* is a different kind of object: it must accept
thirty years of files written by every producer, and almost all of its size is in the format, not in the viewer.
A complete engine means: the object syntax and xref/stream repair for damaged files; the filters (Flate, LZW,
ASCII85/Hex, RunLength, **CCITT, JBIG2, JPX** — the last three carry the scanned literature); encryption (RC4, AES,
public-key); the content-stream interpreter with the full graphics state, clipping, transparency groups, soft
masks, blend modes, patterns and the seven shading types; **a font engine** — Type 1, CFF, TrueType, OpenType, Type 3,
CID-keyed and composite fonts, embedded and non-embedded (with substitution), encodings and CMaps, plus hinting
and anti-aliasing at small sizes; colour spaces incl. ICC-based, Lab, Separation/DeviceN with tint transforms;
annotations with their appearance streams and the forms machinery; text extraction with correct Unicode mapping
(ToUnicode, glyph-name tables) and reading order; incremental saving. Each of these has a specification chapter and
a long tail of non-conforming producers; PDFium, MuPDF and Poppler have each spent well over a decade on it and
still receive rendering fixes weekly. A **subset engine** (Flate + TrueType/CFF + basic graphics) renders clean
born-digital PDFs and would take a few months, but a research corpus is full of the other kind — scans, old Type 1
journals, JPX-compressed publisher files — and a reader that fails on a third of the library is worse than none.

**Memory would not be better with our own engine.** In any viewer, memory is dominated by the page bitmaps at
screen resolution, the decoded images and the parsed fonts — not by the engine's code. Those are governed by the
policies in §6 (tiles, an LRU budget, releasing off-screen pages, decoding images lazily), which we control fully
with PDFium as well: `FPDF_RenderPageBitmap` draws into *our* bitmap, page objects are closed when we say
(`FPDF_ClosePage`), and PDFium's own caches are per open page. Chrome's reader runs on it with tabs of hundreds of
pages inside a memory-conscious browser.

**Edge's loading speed** is not a bar PDFium fails to clear: until 2023 Edge's PDF viewer *was* PDFium (Chromium's
viewer), and its 2023–2025 replacement is Adobe's engine — both decades-old, mature engines, neither a fresh
implementation. What made Edge feel fast are viewer-level choices we make in our own UI layer regardless of the
engine: parsing lazily (PDFium loads objects on demand and supports linearized files through `FPDFAvail_*`, so the
first page paints before the file has been read), rendering the visible pages first at the visible scale, a
low-resolution first pass followed by a sharp tile, and never blocking the UI thread on the engine (the worker
thread above). Annotation richness is likewise a UI question: PDFium's `FPDFAnnot_*` API creates highlight, ink,
square and text annotations with appearance streams, edits them and saves the file; the tools, the sidebar, the
linking to the corpus and the assistant are ours whether the engine is PDFium or hand-written.

So: PDFium for the format (Chrome-class speed and robustness for a ~8 MB DLL, BSD), our own code for everything
the user sees — the same division that makes the writer's export use our own `.docx` writer but not our own PDF
parser.

## 3. Data model (project side, engine-independent)

```
struct PdfAttachment { int id; string path; string sha1; string title; int recordIdx = -1;   // corpus record, -1 = unlinked
                       int pages; long bytes; double addedAt; int lastPage; double lastZoom; };
struct Annot { int id; int attachment; int page; enum Kind { Highlight, Underline, Note, Area, Ink } kind;
               vector<Quad> quads;     // text highlights: character boxes merged per line, page space (pt)
               Rect area;              // Area / Note anchor
               uint32_t color;         // Zotero palette: yellow, red, green, blue, purple, magenta, orange, grey
               string text;            // the highlighted text (extracted once, so search and export need no PDF)
               string comment; vector<string> tags; double created, modified; string author; };
```

Stored in the `.vosproj` under `"pdf": {"attachments": [...], "annots": [...]}` (JSON, like `"document"`); the
PDFs themselves are **not** copied into the project — a per-project *attachments folder* (default
`<project>.files\`) plus absolute-path fallback, with a relink dialog when a file has moved (hash match first).
Optionally, annotations are also **written into the PDF as standard annotations** (`/Highlight` with `/QuadPoints`,
`/Text` notes, `/Square` areas) with `FPDF_SaveAsCopy` to a sidecar or in place; that makes them visible in
Acrobat / Zotero and survives the project. Import goes the other way: existing highlights in a PDF are read with
`FPDFPage_GetAnnot*` on first open and offered as annotations.

Linking to corpus records: by DOI found in the PDF text (first two pages, regex `10\.\d{4,9}/\S+`), then by
title similarity against the corpus (normalised, trigram score ≥ 0.85), then manually (a *Link to record* picker,
the papers table with a paper-clip column). A folder watch (`ReadDirectoryChangesW`) on the attachments folder
picks up PDFs dropped there.

## 4. The reader UI

A **Read** page in the left rail (after Write). Main area:

- **Tabs** across the top for open PDFs (the record title, the linked-paper chip, close). Left strip:
  thumbnails / outline (PDF bookmarks) / annotations list / search results, switchable. Right: the inspector's
  Properties pane shows the linked record (authors, year, source, citations, cluster colour, *Show on map*), the
  selected annotation (colour, comment, tags), and reading progress.
- **Continuous scroll**, page-fit / width-fit / free zoom (`Ctrl`+wheel, the writer's clocked easing and the same
  status-bar slider — shared code), two-page mode, rotate. Rendering is tiled (512 px tiles at the current
  scale) so zooming to 400 % costs only the visible tiles.
- **Text selection** from the text layer (`FPDFText_GetCharBox` cached per page as a flat array; hit test by
  binary search on the y-sorted lines), double click = word, triple = paragraph, copy as text with the citation
  appended ("… (Doe et al., 2021)") when *Copy with citation* is on.
- **Annotation tools** (toolbar + `H`/`U`/`N`/`A` keys): highlight, underline, sticky note, area (rectangle
  clip), colour palette, tags; a floating mini bar on selection (colours, *Add note*, *Cite in writer*, *Copy*).
  Highlights are drawn as multiplied rectangles over the page bitmap (not baked into it), so they are instant.
- **Search** in the document (`Ctrl+F`, `FPDFText_FindStart`, hits listed with context, marked on thumbnails), and
  **library search** across all attached PDFs (a per-attachment plain-text cache, `<project>.files\text\<sha1>.txt`,
  extracted once by the worker; a simple inverted index over words is enough for a few thousand papers).
- The **map stays live**: the reader is a main-area page like the writer, so *Show on map*, the Papers table, the
  assistant and the writer remain one click away; in the split view a pane can hold the reader too.

## 5. What it enables at the top level

1. **Cite while you read**: select a passage → *Cite in writer* inserts a Quote block (or an inline quotation)
   with the native citation of the linked record (the writer's keyed citations, hence Word `CITATION` fields on
   export / copy) and the page number (`\p 12` suffix). The one feature that justifies the reader on its own.
2. **Annotated bibliography in one step**: *Write › Insert › Annotated bibliography* builds a section per read
   paper — reference, the highlights in reading order (grouped by colour, e.g. yellow = finding, green = method,
   red = limitation), the notes — a literature-review draft that is actually grounded in the texts.
3. **Figure clipping**: an area annotation → *Insert as figure* renders the region at 300 dpi to a Figure block
   with a "Source: Doe et al. (2021), Fig. 3, p. 7" caption and the citation; the same region can be pasted to
   Word as a picture.
4. **Metadata completion**: DOI extraction → Crossref / OpenAlex lookup fills missing abstracts, references and
   open-access links for the corpus record (the existing OpenAlex code path), and attaches the PDF automatically
   when an OA link exists (*Fetch open-access PDFs* for a selection on the map).
5. **Full-text search and full-text maps**: the extracted text feeds a term map of the *read* documents (a new
   Build source, "full text of attached PDFs"), and the search box can find papers by what they say, not only by
   title / abstract / keywords.
6. **AI reading assistant**: the live assistant gets `open_pdf`, `read_pages(range)`, `search_pdf`, `list_highlights`,
   `add_highlight(page, text)` tools; it can summarise a paper *with page citations*, compare the methods of the
   five most-cited papers of a cluster, or check that a sentence in the writer is supported by the passage it cites
   (open the source at the cited passage). Text is passed as text, never as screenshots; long papers are chunked
   by page.
7. **Topic lens**: with a map built, a paper's pages can be tinted by cluster keywords (the text layer knows where
   each term appears): the reader shows *why* the paper sits where it sits on the map, and the map can highlight the
   nodes whose terms the current page mentions.
8. **Reading workflow**: per-paper status (unread / reading / read / excluded) with reason, reading time, "to
   read" queue ordered by citations or by cluster, a progress bar in the papers table — the screening step of a
   systematic review, inside the tool that already draws the PRISMA flow.

## 6. Memory and performance rules (the constraints that matter most)

- The page bitmap cache is an **LRU with a pixel budget (64 MB default, settable)**: at 1.0× a Letter page at 96
  dpi is ~3.2 MB, so ~20 pages; visible pages ± 2 are kept, thumbnails are 160 px wide (~0.1 MB each) in a separate
  8 MB LRU; on `WM_SIZE` / zoom the old tiles are shown stretched until the new ones arrive (the writer's rule).
- `FPDF_LoadDocument` maps the file; PDFium's own memory for a 30-page paper is 5–30 MB. **At most 4 documents stay
  loaded**; tabs beyond that keep their state (page, zoom, annotations) and reopen lazily.
- Text layers are extracted per page on demand and cached as compact arrays (`x0,y0,x1,y1,cp` per character, ~20
  bytes/char, a dense page ≈ 100 KB); dropped with the page from the LRU, the plain text stays in the on-disk cache.
- Everything PDFium happens on the worker; the UI thread draws bitmaps and rectangles only, so a slow scan-heavy
  PDF never stalls the map or the writer. Rendering priority: visible tiles, then thumbnails, then prefetch.
- The DLL is loaded on first use and unloaded (`FPDF_DestroyLibrary` + `FreeLibrary`) when the last document has
  been closed for 60 s, so an unused reader costs nothing.
- Measured targets (to be checked with the perf HUD): first page visible ≤ 150 ms after open for a typical
  article; scroll and zoom at the frame rate of the map; working set growth ≤ 80 MB with four papers open.

## 7. Phases and effort

| phase | scope | estimate |
|---|---|---|
| 0 | `pdfium.dll` loader (function-pointer table), worker thread, render one page to a bitmap, Read page with continuous scroll / zoom / thumbnails, attachments in the project, drop-to-attach, DOI/title linking | 2–3 weeks |
| 1 | Text layer, selection, copy (with citation), search in document, outline, links | 1–2 weeks |
| 2 | Annotations (highlight / underline / note / area, colours, tags, list, persistence in the project), *Cite in writer*, *Insert as figure* | 2 weeks |
| 3 | Write annotations into the PDF (`FPDF_SaveAsCopy`), import existing ones, annotated bibliography, per-paper status and reading queue | 1–2 weeks |
| 4 | Library full-text search and text cache, metadata completion via DOI, OA fetching | 1–2 weeks |
| 5 | Assistant tools (`open_pdf`, `read_pages`, `search_pdf`, highlights), topic lens, full-text build source | 2 weeks |

Phase 0 + 1 already give a useful reader; phases 2–3 give the Zotero-class workflow; 4–5 are the parts nothing
else offers in combination with the map. Each phase ends with the same checks as the writer: host tests for the
project-side model (no PDFium needed: the annotation / linking / bibliography code takes text and boxes), a
compile of the Windows layer, and a memory measurement with the perf HUD.

## 8. Open questions to settle before phase 0

- Ship `pdfium.dll` inside the installer by default (adds ~8 MB) or download on first use (needs a consent
  prompt, and offline installs lose the reader)? Proposal: optional installer component, plus download-on-demand.
- Where annotations live by default: project only (safe, private) or also in the PDF (portable, alters the
  user's files)? Proposal: project only, with *Save annotations into PDF* explicit per document.
- Whether the text cache should be encrypted or kept out of the project folder for licensed PDFs (some publisher
  licences forbid derived text stores): keep it in `%LOCALAPPDATA%` rather than next to the project.
