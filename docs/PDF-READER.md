# Reading papers in VOSStudio: the Read stage, the PDF reader and the reading library (1.17.0)

The map says *which* papers matter. **Read** is the stage where you actually read them without leaving the
application: attach the PDFs, read them in a fast native reader, highlight and *code* passages (aim, method,
finding, theory, gap, quotation, question), keep notes, a status and a rating per paper, and send quotations and
citations straight into the report in **Write**. What the reading produced — the notes of every paper and a
coding matrix — exports as Markdown and CSV for the synthesis.

`docs/PDF-READER-PLAN.md` is the design that preceded this document; this one describes what is built.

## 1. The workflow, stage by stage

The left rail is now the order of a systematic analysis: **Data → Build → Look → Analyse → Trends → Actors →
Read → Write → Publish** (Assistant below the line). *Read* sits between the analysis and the writing on purpose:
the analysis produces a reading list (the papers of a cluster, the most cited, the bursts), the reading produces
evidence (coded passages, notes), and the writing cites it.

| step | where | what happens |
|---|---|---|
| attach | Read › **Attach PDFs…**, drop `.pdf` files on the window, the **PDF** column of the papers table, *Attach the PDF…* on a record's preview, or an AI request | the file joins the project's **reading library** (linked in place: the file is not copied). Its first pages are read on the engine thread for a DOI and a title, and it is **linked to the matching record** (DOI, else normalised title, year ±1). Unlinked papers can be linked by hand in the reader's *Paper* pane. |
| get (1.18) | Read › **Get PDFs…**, the **Get PDFs** button / PDF cells of the papers table, *Read › Get open-access PDFs…*, the assistant's `get_pdfs` | the **open-access copies** of the records with a DOI and no file are downloaded into `<project> attachments\` (OpenAlex's cached copies with a free key, the publishers' and repositories' open PDFs, arXiv), verified, attached and linked. Papers that are not open or whose site refused are listed under **Without a PDF** with the reason and the manual path. `docs/OA-FETCH.md`. |
| read | click a paper in the reading list, the book icon in the papers table, *Read the PDF* on a record | the reader fills the main area; the left panel stays the reading list. Opening a paper marks it *Reading*. |
| mark | select text on the page | a small two-row toolbar appears at the pointer: the seven **codes** (colour dots, keys `1`–`7`), plain highlight (`H`), underline (`U`); then *Note* (`N`), *Copy* (`Ctrl+C`), *Quote* (`Ctrl+Q`) and *Cite* (`Ctrl+E`). Right-click a page for a free-standing note. Every mark can carry a comment. |
| note | the **Notes** pane | status (*To read / Reading / Read / Excluded*), a 1–5 relevance rating, tags (with the project's existing tags one click away) and free reading notes. |
| quote & cite | *Quote* / *Cite* on the toolbar or a highlight | *Quote* inserts the passage as a *Quote* paragraph in the writer followed by a native citation of the linked record; *Cite* inserts the citation at the writer's caret. The reader stays in front; **Write** shows the result. |
| synthesise | Read › **Synthesis** | **Notes (Markdown)**: every paper with its status, rating, tags, notes and highlights grouped by code. **Coding matrix (CSV)**: one row per paper, one column per code with the coded passages, plus status, rating and tags — the input of a thematic synthesis. |
| keep | the project file | the library, annotations and notes are saved in the `.vosproj` (key `library`); the PDF files are not modified. **⋯ › Save highlights and notes into the PDF** writes them into the file as standard annotations (Acrobat, Zotero and every other viewer show them); the project keeps its copy and marks them *in file*. |

The reading list itself shows where the work stands: a progress bar (read + excluded / total, reading in accent),
filter chips (*All / To read / Reading / Read / Excl.*), a search over titles, authors, tags, notes **and the text of
the highlights**, sorting (recent, title, year, status), and per paper the status dot, counts of highlights and
notes, tags, rating, a *file missing* flag and *next up* (the first paper still to read).

## 2. The reader

- **Continuous vertical pages**, the whole document scrollable; page box and `←`/`→` for jumps; `Space`/`PgDn`,
  `Shift+Space`/`PgUp`, `Home`/`End`, arrows; the outline pane for bookmarks; internal links and URLs are live.
- **Zoom** `Ctrl`+wheel around the pointer, `Ctrl` `+`/`−`, `Ctrl+0` fit width (the default; wide pages fit), `Ctrl+1`
  100 %, `Ctrl+2` 200 %, a zoom list on the toolbar. Panning with the middle button or `Space`+drag.
- **Scrolling**: wheel, keys, and thin **scrollbars** at the right and bottom edges of the page area (drag the
  thumb, click the track to page). Sideways, when the page is wider than the view: `Shift`+wheel, a touchpad's
  two-finger sideways swipe, `←`/`→`, the bottom bar, or panning.
- **Text selection** by drag (from a word or from white space), double-click a word, triple-click a line, `Shift`+click
  to extend. `Ctrl+C` copies with the line breaks joined and end-of-line hyphenation removed. The status strip shows
  the count of selected characters and the keys.
- **Highlights, underlines, notes** are drawn over the page from the project; hover shows the code and comment.
  **Click one and its card opens next to it**: the comment field (stored as you type), the seven code dots to
  re-code the passage, *Quote*, the bin. `Enter` or *Done* or a click anywhere on the page closes the card,
  `Shift+Enter` breaks a line, `Esc` closes it, `Delete` removes the mark. The toolbar's *Comment* (or `N`) makes a
  highlight and opens its card at once; right-click a page › *Add a note here* does the same for a free note.
  Right-click a mark for *Edit the comment / Copy text / Quote / Cite / change the code / Delete*. Annotations
  already inside the file are listed in the *Paper* pane and rendered by the engine.
- **Areas** (1.16) for what has no selectable text — scanned pages, figures, tables, equations: press `A` (the
  *Area* toolbar toggle) and drag a rectangle, or hold `Alt` and drag at any time. The mini toolbar that follows
  codes the area (the seven dots, keys `1`–`7`), opens its comment card, **copies it as a picture** (the engine
  renders the region at 200 dpi, with the file's own annotations left out), **puts it in the document as a
  figure** (a raster figure captioned with the paper and page, followed by a citation when the PDF is linked) or
  deletes it; right-click an area for the same. The text under the rectangle, when the page has a text layer, is
  kept as the passage, so quotes, the notes export (`[area]`) and the coding matrix treat it like a highlight. In
  the PDF file it is a *Square* annotation. Areas are drawn as a tinted rectangle with a comment marker at the
  corner when they have a comment. OCR of scanned text is not included.
- **Dark page** (1.17; `D`, the sun/moon toolbar button, right-click › *Dark page*, *Read › Dark page*). The
  pages are drawn light-on-dark for reading in the dark or on a dark theme — independent of the application's
  theme, remembered across sessions (`readerDark` in the settings). It is not a plain inversion: the page is
  rendered as usual and then each pixel's *lightness* is inverted with its hue kept (BT.709 luma, chroma scaled
  into gamut), so paper becomes `#1c1c1c`, black ink `#e6e6e6`, a blue link a pale blue, a red warning a soft
  red; charts, diagrams and scanned pages invert like text. **Photographs are left as they are** (only dimmed to
  86 %): before a page is drawn the engine lists its image objects once (`Document::images`, about 2 ms per page,
  cached with the page), and `looksLikePhoto` classifies each from a 96×96 subsample — much white or few distinct
  colours means a plot, a logo or a scan, which is inverted; a colourful or continuous-tone image is kept. The
  extra cost is about half a millisecond per 512² tile; tiles of both modes are cached under separate keys, so
  switching back is instant while the tiles of the other mode leave through the ordinary eviction. Text
  selection, highlights and notes are drawn over the dark page as before; area copies, figures for the document
  and *Save into PDF* always use the original colours. Known trade-off: a yellow highlight printed into the file
  becomes dark olive under the inversion (its lightness, not its hue, is inverted).
- **Turned pages** (1.19; `R` clockwise, `Shift+R` anticlockwise; *View › Turn clockwise / anticlockwise /
  Upright again*; right-click a page › *Turn clockwise*; *Read › Turn the view clockwise*; `rdview
  turn|turnback|upright`). The rotation is a pure view transform of this document, kept with the PDF in the
  project (`rot` on the library item, only when set). The engine renders the tiles and previews turned
  (`Document::render` composes the quarter turn into the render matrix, so a turned tile costs the same as an
  upright one; tiles are keyed by the rotation); everything the engine reports — text boxes, links, the file's
  annotations, search hits — stays in the upright page frame, and the reader maps it: overlays go through
  `pdf::rotateBox`, the mouse comes back through `pdf::unrotatePoint`, so selection, highlights, underlines
  (drawn along the turned baseline), notes, areas, hits, links and the comment card all work on the turned page.
  Area pictures and figures come out as viewed (a turned scan reads upright in the document). Bookmarks and hits
  scroll to the page top on a quarter-turned page (a height has no meaning there). Nothing in the file changes;
  *Save into PDF* writes upright coordinates as before.
- **Two pages, facing** (1.19; *View › Two pages*, *Read › Two pages*, `rdview two`; `readerTwoUp` in the
  settings). Pages sit in rows of two, top-aligned, each row centred; **First page alone** (`rdview cover`,
  `readerCover`) puts page 1 alone on the right, as a book opens, so that the pairs that face each other are the
  ones facing in print. Fit width fits the widest pair; `←`/`→`, the page buttons and the page box move a row at
  a time; the position, the zoom anchor and the memory budgets work per page as before. A lone page (the cover,
  or the last of an odd count) keeps the column the width of a spread, so the layout does not jump.
- **Print** (1.19; `Ctrl+P` in the reader, *View › Print…*, the *More* menu, right-click › *Print…*, *Read ›
  Print…*, `rdview print`). A card asks for the pages (*All*, *This page*, *Pages…* with `1-3, 7, 12-`), whether to
  print the project's **highlights, underlines, strikeouts, notes and areas** (on by default when the paper has
  any; the file's own annotations always print, as the engine draws them), *Fit each page to the printable area*
  and *Turn landscape pages to fit the paper*; then the system's printer dialog chooses the printer, the paper and
  the copies. The job (`src/win/rdprint.cpp`) opens **its own copy of the document** on the engine thread, so the
  reader can close, switch papers or release its memory while a long print runs; each page is rendered at the
  printer's resolution (300 dpi at most, about 30 megapixels at most for posters), turned as the reader shows it
  and once more when a landscape page meets portrait paper, the marks are burnt in by `pdf::burnMarks`
  (highlights multiplied like a marker, lines in a darker shade of the colour, notes as a small square, areas as
  an outline), and the page goes to the printer as a 24-bit bitmap (`StretchDIBits`), centred in the printable
  area. One page renders at a time, at a lower priority than the visible tiles, so the reader stays fluid. Copies
  are left to the driver when it does them, else printed collated by the job. Cancel from the job bar aborts the
  document. A vector path (`FPDF_RenderPage` onto the printer DC) would give smaller spool files and is the
  natural upgrade; it was not chosen because it cannot be verified here and rasterising keeps the marks simple.
- **Find** (`Ctrl+F`, pane) searches the whole document on the engine thread from the current page onward, with
  *match case* and *whole words*; matches are highlighted on the pages, listed with their context, `Enter`/`F3`
  and `Shift+F3` step through them.
- **Panes** (right side of the reader, not the inspector): *Notes*, *Highlights* (filter by code; a click goes to
  the mark, the buttons comment, quote and delete), *Find*, *Outline*, *Paper* (the linked record with *Details / Cite / Change…*, a
  record picker for unlinked papers, PDF metadata, the file with *Explorer / Open in… / Locate…*, and the
  annotation state with *Save into PDF* and *Detach*).
- **Password-protected files** ask for the password in place; a missing file offers *Locate…*.
- `Esc` closes the comment card, then clears the selection, then the find, then returns to the reading list.

## 3. Why it feels instant (and why the rest of the application does not notice)

Long lists (1.16): the *Outline*, *Highlights* and *Find* panes and the reading list on the Read page measure
their row heights once (per document, width, scale and — for highlights — the annotations' text) and draw only
the rows inside the scroll view. Before, a book's outline with thousands of bookmarks was measured and laid out
in full every frame (three text layouts per entry) and dragged the whole window to a few frames per second while
the pane was open. The reading list also stops calling the file system for every row every frame; file existence
is cached and re-checked every few seconds.

- **One engine thread** (`pdf::Worker`, `src/core/pdfdoc.*`) owns every PDFium call: render, text, links,
  search, annotation reading and writing, saving. The UI thread only draws cached bitmaps. Tasks carry a priority
  (previews first, then the tiles nearest the middle of the view, then text, links, search, prefetch) and a cancel
  flag: tiles that scroll out of the prefetch band before they render are skipped.
- **Tiles, not pages.** Each page is rendered in 512-px tiles at the *target* scale, requested only once the zoom
  has rested for 120 ms; while the zoom animates, the tiles of the previous scale and a 220-px preview of the page
  are stretched by the GPU, so zooming never waits for a render. The old-scale tiles are dropped only when the
  new scale covers the view. **Memory is bounded and does not grow with scrolling**: tiles are capped at 48 MB
  and, above 16 MB, any tile that has been out of the view for 1.5 s is dropped; page text and previews are kept
  for 24 pages around the view; the engine keeps 3 loaded pages. Measured on Linux with a 40-page paper with
  figures, the engine holds 15–20 MB while scrolling and 11 MB after the document is closed.
- **Eased zoom and scroll** on a clock (`exp(−dt/τ)`, frame-time clamped), the document point under the pointer
  held still through a zoom, as in the writer. Nothing animates when nothing changes: the reader draws a frame only
  on input, easing or an arriving tile.
- The reader is a `readerOpen` gate in the main area like the writer's: the map, the papers table and the
  inspector are simply not drawn while it is open; the writer may stay open underneath (*Quote* and *Cite* write
  into it) and the rail's *Write* brings it back. Closing the reader (Esc, *Library*) keeps its position and zoom;
  the tiles and previews are freed at once, the document itself 45 s later — returning within that time is
  immediate, after it the file is reopened at the same place (a few tens of milliseconds).
- **Memory, in tiers** (1.19; `App::readerIdleTick`, `Worker::releaseLibrary`). The question was whether the
  reader's memory could be given back more often — after a second or two of idleness — without stutter. The
  answer is that *while the reader is open* it already is: the tile budget (48 MB hard, 16 MB soft with 1.5 s
  idle eviction), the 24-page text and preview windows and the engine's 3-page LRU keep it small whatever the
  document, and nothing there waits for an idle moment. What is not freed while reading is the engine's own
  heap (PDFium's allocator keeps its arenas; there is no purge call) and the document's parsed objects, and those
  are deliberately kept, because the user is reading. The tiers after the reader closes:
  1. **At once:** the tiles and previews (GPU memory) go, and the engine drops the document's page objects
     (`dropPages`) — the parsed document stays, so returning to the paper is instant.
  2. **45 s later** (`kReleaseAfterS`): the document is closed (`readerResetDoc`).
  3. **90 s after that** (`kTrimAfterS`), only in a quiet moment — no key, click, wheel or mouse move for 2 s and
     no job running — the engine library is destroyed (`FPDF_DestroyLibrary`, the only way its allocator returns
     its memory; the next open initialises it again, in milliseconds, transparently), the GPU's transient
     allocations are trimmed (`IDXGIDevice3::Trim`) and the process working set is trimmed once
     (`SetProcessWorkingSetSize(-1, -1)`, what the application already does when it is minimised).
  Why not more often, or on a timer: a working-set trim is not a free: the pages come back through soft page
  faults, one hitch per touched page, which is exactly the "lag after a pause" a periodic trim would produce; and
  the 45 s / 90 s grace periods are the difference between glancing at the map and coming back to the paper
  instantly, and a real change of activity. Switching to the map, the table or the writer *does* close the
  reader (it is a gate), so the tiers run whenever the user leaves the paper, not only when the PDF changes. A
  print job holds the tiers back while it runs (it has its own copy of the document, but the engine library must
  stay).

## 4. Engines inside the executable

The Windows build embeds both binary engines in `VOSStudio.exe` as deflate-compressed `RCDATA` resources. The
pinned release downloads and SHA-256 checksums are documented in `third_party/pdfium/README.md` and
`third_party/tectonic/README.md`; a normal Make/CI build fails rather than producing an artifact with a missing engine.

- **PDFium** (`pdfium.dll`, Chromium's PDF engine, BSD-3-Clause; release `chromium/8076` from
  *bblanchon/pdfium-binaries*) is extracted on first use into
  `%LOCALAPPDATA%\VOSStudio\pdfium\<size>-<crc32>\pdfium.dll` and loaded from there. Per-build folders let
  another running copy keep using the DLL during an update. PDFium's license notices are included.
- **Tectonic** (official x64 Windows executable, version 0.17.0, MIT) is extracted on first Writer PDF compile into
  `%LOCALAPPDATA%\VOSStudio\tectonic\<size>-<crc32>\tectonic.exe`; users do not need to install it or add it
  to `PATH`. Its TeX support bundle is separate from the executable: Tectonic downloads missing support files from
  its default bundle and caches them per user. The first compile may therefore need an internet connection;
  subsequent builds generally reuse cached files, though Tectonic can periodically check the remote bundle. The
  support bundle itself is not embedded.

*Help › Third-party notices* writes the bundled PDFium notices and Tectonic MIT license to a text file and opens it.
`tools/packres` and `src/core/inflate.*` pack and restore the executable resources.

## 5. Data model (project file)

```
"library": { "nextId": 4, "items": [ { "id": "p1", "path": "C:\\…\\smith2021.pdf", "record": "<refKey>", "title": …,
  "doi": …, "author": …, "year": 2021, "pages": 14, "size": 812344, "status": "reading", "rating": 4,
  "tags": ["core"], "notes": "…", "lastPage": 3.42, "zoom": 0, "added": "…", "opened": "…", "nextAnnot": 7,
  "annots": [ { "id": "a1", "kind": "highlight", "page": 2, "quads": [[x0,y0,x1,y1], …], "color": "#FFE066",
                "code": "Finding", "text": "the quoted passage", "note": "…", "start": 1204, "count": 188,
                "created": "…", "modified": "…", "inFile": false }, … ] } ] }
```

Since 1.18 an item fetched by *Get PDF* also carries `"source": {url, host, kind, version, license, oaStatus, fetched}`
and the library a `"fetch"` object keyed by record key with the outcome of the last attempt per record
(`docs/OA-FETCH.md` §5).

Kinds: highlight, underline, strikeout, note (sticky, `rect`), area. Coordinates are display points of the page
as shown (rotation applied). `record` is the bibliography key (`refKeyFor(doi, title, year)`), so the link survives
re-imports of the same data; *Match to records* relinks a whole library.

## 6. The assistant

Five tools join the agent (text and Live): `get_pdfs` (1.18: fetch the open-access PDFs of the records without
one and report what could not be fetched and why), `reading_list` (the library with ids, status, counts), `open_pdf`
(a paper by library id, record id or title words, optionally at a page), `set_reading` (status, rating, tags, a
note — a *change* tool, so it asks), `paper_notes` (a paper's reading as Markdown: notes and every coded
highlight — the input for "summarise what I marked in these three papers"). `open_page` knows *read*.

## 7. Verification and limits

Verified on Linux against `libpdfium.so` (the engine wrapper, the inflater, the library model, exports, the lazy
engine start inside the worker, matching, search, annotation round trips, the memory profile of a scrolling pass:
`make test`, 21 programs). The Windows user interface (drawing, input, tiles on Direct2D, the resource path,
clipboard, dialogs) compiles cleanly with MinGW; 1.15.0 was run on Windows by its user and 1.15.1 fixes what that
run found (a crash when a second PDF was opened, comments that could not be saved, no sideways scrolling, memory
that grew with scrolling and was not released). The 1.15.1 changes themselves were reviewed and compiled, not run
on Windows — the comment card, the scrollbars and the release timing are the parts to look at first.

1.19.0 (turned pages, two pages, printing, the memory tiers) was verified on Linux where it could be: the
rotation of the render matrix (a quarter turn is the transposition of the upright page, a half turn the point
reflection, offsets on the turned page, the point/box helpers round-trip), the burnt marks (a highlight
multiplies, an underline sits under the turned quad, an area frames), the library release and re-initialisation
of the engine, the page layout (rows, the cover alone, mixed sizes, one page, no pages), the page-range parser and
the project round trip of the rotation. The Windows side — the *View* menu, the turned overlays and mouse mapping,
the two-page scrolling, the Print card, `PrintDlgW`, `StartDoc`/`StretchDIBits` — compiles cleanly with MinGW and
was reviewed, not run: no Windows, no printer here. The print job is the part to try first on a real machine
(a PDF printer such as *Microsoft Print to PDF* is enough), then a turned page with highlights and a comment card.

The dead-button trap, for future work on this code base: `Ui::behave` gives a press to the *first* widget that
claims it in a frame, so a button drawn *inside* a `listRow` never receives clicks — the row already took them.
Rows with buttons use `rowWithStrip()` (readlib.cpp), whose hit area stops before the button strip; overlays over
the page area (the mini toolbar, the comment card, the scrollbars) take their input before the page area's
`behave`, or the page area skips its `behave` while the pointer is over them.

1.16.0 (area annotations, list virtualisation, the file-existence cache) was again compiled and reviewed, not run
on Windows: the area gesture, its mini toolbar and the clipboard/figure paths are the parts to try first.

1.17.0 (the dark page) was verified on Linux through the library API on real PDFs — image classification, the
transform's colours (paper 28/28/28, ink 230, blue link pale blue), the kept photograph, the tile timings —
and `tests/pdf_test.cpp` covers `images()`, `looksLikePhoto` and `darkenPage` on a generated page with four
images (a noisy RGB "photograph", a plot, a grey scan and one placed by a form matrix). The Windows side
(toolbar button, key, menus, dark backdrop, previews and tiles keyed by the mode) compiles cleanly and was
reviewed, not run.

Not in this version (next): thumbnails strip; copying hand-attached files into the attachments folder (fetched
files already live there); full-text search across the whole library from the reading list (today the list search
covers titles, notes and highlights); ink annotations; OCR of scanned pages.
