# The writer (1.15)

The writer is a document editor inside VOSStudio: the report is written and edited on a page in the main area,
saved with the project, and exported as **PDF**, **Word (.docx)** and **HTML** from the same document. The
assistant's report tools (`add_section`, `add_chart`, `write_report`, `edit_section`, `get_report`,
`export_report`) read and write that same document, so what the AI drafts, what you edit and what the files
contain is one thing.

Open it from **Write** in the left rail (its panel: outline, quick inserts, exports, citation style, statistics),
the **Write** menu, the pen button in the top bar, `Ctrl+Shift+W`, the palette (*Writer*), the `writer` command,
or *Open in Writer* on the Assistant's Report card. `Esc` (with nothing selected) or the ✕ button returns to the
map; the document stays, and the Write panel keeps offering it.

## New in 1.15

- **The selection toolbar is two rows.** After a mouse selection the small toolbar at the pointer stacks its
  buttons in two rows (style and paragraph controls above; link, cite, copy and the rest below) instead of one long
  line, so everything is within a short reach of the pointer.
- **Quotes and citations from the reader.** *Quote* on a highlighted passage in the PDF reader (Read stage)
  inserts it as a *Quote* paragraph followed by a native citation of the linked record; *Cite* inserts the citation
  at the caret. The reader stays in front; the Write rail brings the document back. See `docs/PDF-READER.md`.

## New in 1.14

Shaped by a session with the 1.13 writer and a real Word 365.

- **Copy and paste to Word, the Word way.** A plain `Ctrl+C` no longer appends a reference list (a bibliography in
  the middle of a document is never wanted); instead the sources of the cited papers are added to the open Word
  document's own *Manage Sources* list by automation as you copy, so the pasted `CITATION` fields resolve at once
  and the bibliography and its style are managed in Word (*References › Bibliography*, *Style*). `Ctrl+Shift+C` is
  now **Paste into Word at the cursor**: sources, paste, field update, Word to the front — Word is started if it
  is not running. *Write › Copy with reference list* remains for the rare case where the list should travel too.
  Setting: *Settings › General › Add cited sources to the open Word document when copying*.
- **Figures copy as vector graphics.** A selected figure (click it, `Ctrl+C` / `Ctrl+X` — the block itself was not
  copied before) is placed on the clipboard as an Enhanced Metafile (EMF+), a PNG and a bitmap; Word, PowerPoint,
  Excel and Inkscape paste the scalable drawing. In a copy of text and figures the vector figures travel as Word's
  own VML picture syntax pointing at the `.emf`, with the PNG as the fallback for every other application. Figures
  set to *PNG* per figure stay pictures.
- **Zoom beyond fit-to-width.** The zoom no longer stops when the page fits the area: it continues to 300 % with a
  horizontal scroll offset (a horizontal scrollbar under the pages, `Shift`+wheel, `Ctrl`+wheel zooms around the
  pointer in both axes). A page wider than the window opens fitted; the *fit* button beside the zoom slider
  returns to the fit-to-width zoom at any time.
- **The mini toolbar.** After any selection made with the mouse (drag, double- or triple-click), Word's compact
  format bar appears above the pointer: paragraph style, font, size with grow / shrink, bold / italic / underline /
  strikethrough / highlight, text colour, bullets and numbering, citation and link. It also rides on top of the
  context menu on a right-click in text. It fades when the pointer moves away, a key is pressed, the page scrolls
  or the selection goes; its lists keep it open. Narrow windows drop the font and style boxes first.
- **Right-click keeps a fresh selection.** A right-click inside the selection, or within three seconds and a hand's
  width of where a selection was just finished, keeps it (Word's behaviour: the pointer usually rests just outside
  the selected text); otherwise it moves the caret as before.

## New in 1.13

- **Zoom is a transform, the layout is fixed.** The pages are laid out once at 100 % (`writerK_ = s · 96/72`) and
  drawn through a Direct2D scale; a zoom step re-lays out nothing (line breaks and pages never move, as in Word),
  the displayed scale eases on a clock (`1 − e^(−dt/45 ms)`, frame-rate independent), `Ctrl`+wheel takes fractional
  steps (`1.1^wheel`, precision touchpads and pinch), and a **zoom slider** in the status bar covers 40–300 % on a
  log scale with a notch at 100 % (click the percentage for 100 %). Figure bitmaps are stretched while the zoom
  moves and re-rendered once it has rested for 150 ms; they live in an LRU with a 96 MB pixel budget and are
  released whenever the writer is not the main area.
- **Cheaper frames.** `writerRelayout` returns at once when the document version, block count, page geometry and
  citation style are unchanged (before, every frame re-hashed every paragraph and re-paginated); the status-bar
  word / figure counts and the selection word count are cached per version; the undo history is bounded by 24 MB
  (never fewer than 20 steps) as well as by 200 steps.
- **Word round trip**: `Ctrl+C` copies Word-ready HTML (styles, lists, tables, pictures, native `CITATION` fields
  with the sources, `SEQ` captions) beside the text; `Ctrl+Shift+C` *Copy with bibliography* appended the reference
  list as a `BIBLIOGRAPHY` field (replaced in 1.14 by *Paste into Word at the cursor*); a single figure is also copied as a picture; `Ctrl+V` reads Word's / a browser's
  HTML back as blocks (headings, lists, tables, inline formats). *Open in Word* in the Export menu writes the
  `.docx` and launches Word. Details in `docs/WORD-EXPORT.md`.
- **Split view** (outside the writer): the pane picker's *Insert into the document* button drops any chart of the
  dashboard into the document at the caret.

## New in 1.12

- **Pages.** The document is paginated on screen exactly as the PDF prints it (`writerRelayout` → the paginator):
  paragraphs split between lines with at least two lines on each side, tables between rows, figures and rules move
  whole, headings keep three lines of their text with them, `Page break` opens a new page, the running head and
  page numbers are drawn, the status bar says *Page x of n* and the outline shows each heading's page. Coordinates:
  every block knows its *places* (page, local range, y on the page); `writerDocY` / `writerDocToLocal` translate
  between block-local and page-column positions, so the caret, selection, hit testing and *scroll to caret* all
  work across page boundaries.
- **Smooth zoom.** `Ctrl`+wheel zooms around the mouse (status-bar −/+ and `Ctrl` +/−/0 around the middle). The pages
  are scaled with a Direct2D transform frame by frame (1.13: at every zoom, the layout is fixed at 100 %).
- **Fonts, sizes and colours** per selection (`Span.font/size/color`): the toolbar's font box lists the installed
  families (DirectWrite system collection), the size box offers the usual sizes (`Ctrl+Shift+>` / `<` step through
  them; *Style size* removes the override), the colour button opens a palette (*A* = automatic). The Properties
  pane has the same controls plus the document fonts (body and heading), base size and line spacing (1.0 / 1.15 /
  1.5 / 2.0).
- **Toolbar** in groups that reflow into two or three rows when the window is narrow; the right-hand block (panes,
  zoom, Export, close) always sits on the last row. Table and figure tools appear when the caret is in one.
- **Properties pane** (scrollable): figure (width 40–100 %, alignment, export format, caption above/below,
  border, label, alt text), table (header row, column alignment, rows/columns), paragraph (style, alignment, list
  level), text (font, size, colour), the citation at the caret, page (paper, margins, fonts, size, spacing, running
  head, numbered headings, TOC, page numbers, default figure format), citations (style, count), document (author,
  keywords) and statistics.
- **Citations.** `Ctrl+Q`, the toolbar, the Insert menu or the context menu open the picker: every loaded paper,
  searchable by title / author / source / year / DOI, scrollable, with check boxes for a multi-paper citation
  (*Insert citation (n)*); `Enter` inserts the first match. Citations are keyed fields (`Span.cite`); the in-text
  text and the reference list are regenerated in the chosen **citation style** (APA 7, IEEE, Harvard, Chicago
  author-date, MLA, Vancouver, …) — toolbar, Write panel, Properties pane or Write › Citation style. Hand edits of
  a generated reference are kept. In Word they are native citations (see `docs/WORD-EXPORT.md`).
- **Context menu** (right click, `Shift+F10`, the menu key): a mini format bar (B / I / U / highlight, size,
  clear), cut / copy / paste / paste plain, links, citations, sub-pages for paragraph style, font, insert, table
  and figure, select word / paragraph / all, move block up / down, delete.
- **Figures**: width 40 / 50 / 60 / 75 / 100 %, left / centre / right, caption above or below, thin border, label
  *Figure / Fig. / Chart / Map / Diagram / Plate* or no label, alternative text, and the export format (vector SVG
  or PNG picture) per figure or as the document default.
- **New document** (Write panel, `Ctrl+Shift+N`, Write menu) clears the text, figures and bibliography after a
  confirmation; it is one undo step.

## The model

`src/core/doc.h`: a document is a list of blocks — paragraphs of formatted spans (Title, Subtitle, Meta, Heading 1–4,
Body, Bullet, Number, Quote, Code, Caption, Reference), tables of paragraphs, figures (vector scenes drawn by the
application: charts and maps), horizontal rules, page breaks and a table of contents — plus page setup (paper,
margins, font, base size, numbered headings, TOC, page numbers), author and keywords. Every change goes through
`Editor` (`src/core/doc.cpp`, `docmd.cpp`), which has undo/redo, selection, formatting, lists, tables, figures, the
clipboard, find/replace and citations. The three exporters (`docexport.cpp`, `docx.cpp`) and the Markdown importer
work on the same structures. The core has no Windows dependency; `tests/doc_test.cpp` covers it.

`src/win/writer.cpp` is the platform layer: DirectWrite layouts (one per paragraph, cell or caption, cached by a
content signature so typing re-lays out only the paragraph being edited), hit testing, the caret and selection,
the keyboard map, the toolbar, side panes, menus and dialogs.

## What it does

**Text and formatting** — bold, italic, underline, strikethrough, subscript, superscript, code (monospace),
highlight, links (Ctrl+K; Ctrl+click opens), clear formatting; alignment left / centre / right / justify;
paragraph styles from the style box; bulleted and numbered lists with three levels (Tab / Shift+Tab), numbering
that continues or restarts; quotes; code blocks; captions; references.

**Structure** — headings 1–4 (Ctrl+Alt+1…4, Ctrl+Alt+0 = body), optional numbered headings (1, 1.1, 1.2),
table of contents block, page breaks (Ctrl+Enter), horizontal rules, moving whole blocks up and down
(Alt+Shift+↑/↓), an outline pane that lists headings, figures and tables and jumps to them.

**Tables** — insert with any size (Ctrl+T), Tab / Shift+Tab between cells (Tab after the last cell adds a row),
insert or delete rows and columns, header row on/off, per-column alignment (left, centre, right, numbers right),
a *Table n.* caption when a Caption paragraph sits next to a table.

**Figures** — any chart of the loaded data (publications per year, top sources/authors/countries/organisations,
most cited, citation classes, trend topics, bursts, thematic evolution, three-field plot, country collaboration,
production over time, Bradford, Lotka) and the current map view (network, overlay, density, timeline, geo) or the
strategic diagram, at 50 / 75 / 100 % of the text width, with a numbered caption (*Figure n.*). Figures are vector
scenes: the PDF keeps them as vectors; Word and HTML get 200-dpi PNGs.

**Citations** — *Insert › Citation* (Ctrl+Q) offers the papers selected in the papers table, the previewed paper
and the most cited ones: it writes `[n]` at the caret and adds the APA reference (with its DOI link) under a
*References* heading, reusing the number when the paper is already cited. The assistant's `[R12]` citations become
the same numbered references.

**Page setup pane** — A4 / US Letter, narrow / normal / wide margins, Calibri / Cambria / Arial / Georgia / Times
New Roman, 10 / 11 / 12 pt, numbered headings, table of contents, page numbers, author and keywords (document
properties), plus word / block / figure / table / reference counts and the estimated page count.

**Find and replace pane** — Ctrl+F / Ctrl+H, F3 / Shift+F3, match case, replace one or all; covers headings,
paragraphs, table cells and captions.

**Clipboard** — Ctrl+C / X / V; copying inside the application keeps formatting, tables and figures (rich
fragment); the same copy carries Word-ready HTML (styles, lists, tables, pictures, native citations with their
sources added to the open Word document's source list) for Word and a plain-text form for everything else; a
selected figure copies as a vector metafile + PNG; Ctrl+Shift+C pastes the selection into Word at its cursor;
*Write › Copy with reference list* appends the reference list (a Word bibliography field); HTML from Word or a
browser is pasted as blocks; other text is pasted as paragraphs,
or parsed as Markdown when it looks like Markdown (headings, lists, tables, `**bold**`); Ctrl+Shift+V pastes plain
text.

**Navigation** — arrows, Ctrl+arrows (words / paragraphs), Home / End (visual line), Ctrl+Home / End, Page Up /
Down, Shift extends the selection, double-click selects a word, triple-click a paragraph, dragging selects across
blocks, mouse wheel scrolls, Ctrl+wheel and Ctrl + / − / 0 zoom (the page also shrinks to fit a narrow window).

**Undo** — Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z); typing coalesces; the assistant's changes are undo steps too.

**Right-click** — cut / copy / paste, link, select paragraph, table row/column operations, figure width, move or
delete the block.

**Status bar** — current style or table position, block number, selected words, word / figure / page counts,
whether the document has unsaved edits.

## Exports

`Export` in the toolbar (or `writer pdf|docx|html|all [path]`, the palette, the Assistant's Report card, the
`export_report` tool):

- **PDF** — paginated by `docToPages`: title block, optional table of contents with dot leaders and page numbers,
  numbered headings, lists, quotes, code, tables with repeated header rows, vector figures with captions, hanging
  references with links, page numbers, page breaks. Fonts: Helvetica / Times families (the PDF names the standard
  14 fonts, so it needs no embedding).
- **Word** — `docToDOCX`: Word's own styles (Title, Subtitle, Heading 1–4, List Paragraph, Quote, Caption,
  Bibliography, Hyperlink), real numbering, tables with header rows and alignment, pictures with alt text and
  `SEQ Figure` captions, TOC field (Word fills it on *Update field*), page numbers, page setup, document
  properties; validated with the Open XML SDK validator (see `docs/WORD-EXPORT.md`).
- **HTML** — `docToHTML`: one self-contained file with inline CSS and base64 PNG figures, semantic headings,
  lists, tables, figure/caption elements and a reference list.

### LaTeX PDF preview

The Writer toolbar's **Compile** / **Recompile** action builds a snapshot of the visual document's generated LaTeX
with the Tectonic executable embedded in VOSStudio; no separate installation or `PATH` entry is required. Tectonic
is the only supported compiler (there is no `pdflatex`, `xelatex` or `lualatex` fallback). The build uses Tectonic's
untrusted mode, keeps its log, and requests one rerun for references. Tectonic manages its own TeX support
bundle/cache and may need an internet connection to fetch support files on the first compile; later builds use its
per-user cache, so the first compilation is not guaranteed to be instantaneous. A successful build opens the optional **PDF preview** beside the editor; the
preview scrolls through all pages and is refreshed only when explicitly recompiled. The PDF toolbar button opens the
latest build in the system's separate PDF viewer. Edits mark the preview out of date but never replace the document
or its source bundle. Compile errors are shown in the preview and reported with the compiler log tail; generated
files stay in a unique temporary preview folder.

### Equations

Insert or edit an inline equation from **Insert › Equation** or **Ctrl+Alt+M**. The dialog keeps the LaTeX source,
shows an immediate readable preview, and applies the formula at the caret (or replaces the selected equation). The
full document can be compiled with Tectonic for the faithful typeset PDF preview. Word export and Word-oriented
clipboard HTML produce native OMML/MathML for the supported math grammar; unsupported macros remain visible as
source text and the editor flags the Word fallback rather than claiming it was typeset. Editing and insertion do not
require the TeX source view for ordinary writing, although equation entry itself accepts LaTeX. The in-canvas quick
preview is not full TeX typesetting; use the compiled PDF for final layout.

## Keyboard map

| Keys | Action |
|---|---|
| Ctrl+B / I / U | bold / italic / underline |
| Ctrl+Shift+D | strikethrough |
| Ctrl+Shift+, / Ctrl+Shift+. | subscript / superscript (also Ctrl+Shift+− / Ctrl+Shift+=) |
| Ctrl+` | code |
| Ctrl+M | highlight |
| Ctrl+K | link… |
| Ctrl+Space | clear formatting |
| Ctrl+L / E / R, Ctrl+Shift+J | align left / centre / right / justify |
| Ctrl+Shift+8 / Ctrl+Shift+7 | bulleted / numbered list |
| Tab / Shift+Tab, Ctrl+] / Ctrl+[ | list level (in a table: next / previous cell) |
| Ctrl+Alt+1…4, Ctrl+Alt+0 | Heading 1…4, Body |
| Enter / Shift+Enter / Ctrl+Enter | paragraph / line break / page break |
| Ctrl+T | insert table… |
| Ctrl+Alt+M | insert or edit an equation… |
| Ctrl+Q | insert citation… (picker with search and multi-select) |
| Ctrl+Shift+> / Ctrl+Shift+< | larger / smaller text |
| Ctrl+Shift+N | new document… |
| Shift+F10, menu key | context menu at the caret |
| Ctrl+F / Ctrl+H / F3 / Shift+F3 / Ctrl+G | find / replace / next / previous |
| F2 | select the current section's heading (rename) |
| Alt+Shift+↑ / ↓ | move block up / down |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Ctrl+A, Ctrl+C / X / V, Ctrl+Shift+V | select all, clipboard, paste plain |
| Ctrl+Shift+C | paste the selection into Word at its cursor (sources added to Word's list) |
| Shift+wheel | scroll sideways when zoomed past the width |
| Ctrl + / − / 0, Ctrl+wheel | zoom |
| Ctrl+Shift+P | export PDF |
| Ctrl+S, Ctrl+O, Ctrl+P, Ctrl+J, Ctrl+Shift+L, Ctrl+, | stay global (save, open, palette, assistant, Live AI, settings) |
| Esc | close menu → clear selection → close the writer |

## Project file

The document is saved in the `.vosproj` as `"document"` (`docToJson`: blocks, assets as compact scene blobs, page
setup). Projects without a document open with an empty one; the writer's *edited* state clears on save.

## Limits and honesty

- The on-screen pagination uses DirectWrite metrics and the PDF uses its own font metrics (standard 14 fonts), so
  a line may occasionally break differently in the PDF; the page structure (breaks, keep-with-next, widows) is the
  same model. Word paginates the .docx itself.
- Space before a paragraph is kept at the top of a page (Word drops it); a paragraph taller than a page that cannot
  split within the widow/orphan rule is cut at the page edge.
- Subscript and superscript are drawn smaller but on the baseline (DirectWrite layouts have no per-range baseline
  shift); the exports place them correctly.
- The document/export core is covered by portable unit tests. This workspace has no MinGW toolchain or Windows PDFium
  runtime, and its connection to GitHub release assets failed while trying to fetch the pinned Tectonic binary, so the
  Windows executable and preview have not been built or exercised here. The CI workflow downloads and verifies both
  engines, builds the Windows executable on each push, and publishes it as an Actions artifact.
