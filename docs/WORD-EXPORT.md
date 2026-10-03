# Word export (`.docx`)

The document written in the writer (1.11: Export › *Word document*, Assistant › Report › *Save as Word…*, the
`writer docx` command, or the assistant tool `export_report` with `format: docx|both|all`) is produced by our own
WordprocessingML writer, `src/core/docx.cpp` (no external library). Since 1.11 the writer takes the editor's
document model (`src/core/doc.h`: `docToDOCX`) — paragraphs of formatted spans, tables, figures, rules, page breaks
and the table of contents — and the Markdown path described below is what the assistant's text goes through on
its way into that document. This page lists what the writer understands, how the document is built
and how it was checked, so that changes to the writer can be verified the same way.

## What goes in

The assistant writes sections in Markdown. The writer converts this subset faithfully; anything else stays literal
text rather than disappearing:

| Markdown | Word |
|---|---|
| Consecutive lines | one paragraph (hard wraps are joined); a blank line ends it |
| `#` … `######`, and a line that is only `**bold**` | `Heading 2` … `Heading 4` inside a section (the smallest `#` level used in a section maps to the level below the section heading; a first heading that repeats the section title is dropped) |
| `- ` / `* ` / `+ ` items, `1.` / `1)` items, indented sub-items, continuation lines | real Word lists (bullet → dash → square; decimal → letter → roman); each numbered list restarts at its first number |
| `> quote` | `Quote` style (italic, left rule) |
| ```` ``` ```` fences, 4-space / tab indented blocks | `Consolas` block on a light background, `keepLines` |
| `---` / `***` / `___` | paragraph rule |
| pipe tables with a `---` separator row (outer pipes optional, `:---:` alignment, `\|` escaped pipes, `<br>` in cells), also 2+ rows that all start with `\|` | Word table: shaded bold header row repeated on each page, thin borders, right-aligned numeric columns, rows that do not split; 8+ columns use a smaller font |
| a paragraph `Table n.` / `Figure n:` … | `Caption` style (a table caption placed above its table is kept with it) |
| `**bold**`, `*italic*`, `_italic_`, `` `code` ``, `~~strike~~`, `<b> <i> <u> <s> <sub> <sup> <code> <br>` | character formatting; unbalanced markers stay literal (`p < 0.05*`, `snake_case_names`) |
| `[text](url)`, `[text](url "title")`, bare `https://…`, `www.…`, `doi.org/…`, `10.xxxx/…`, `<https://…>` | clickable hyperlinks (trailing `.`, `,` and unbalanced `)` stay outside the link) |
| `&amp; &lt; &nbsp; &#8217; …`, `\*`, `\_`, `\|` | the character itself |
| UTF-8 text, and text that was UTF-8 decoded twice or is Latin-1 | repaired to proper UTF-8; control characters removed |

References (`ReportDoc::references`) are written as-is with `[n]` numbers and a hanging indent; only URLs and DOIs
in them become links (no Markdown, so titles with `*` or `_` are safe).

## What comes out

One ZIP with `[Content_Types].xml`, `_rels/.rels`, `word/document.xml`, `styles.xml`, `numbering.xml`,
`settings.xml`, `fontTable.xml`, `footer1.xml`, `word/_rels/document.xml.rels`, `word/media/figureN.png`,
`docProps/core.xml` and `app.xml` — the same set Word writes for a simple document.

- Styles as Word names them (`Title`, `Subtitle`, `Heading 1`–`4`, `List Paragraph`, `Quote`, `Caption`,
  `Bibliography`, `Hyperlink`, `Table Grid`, a `Report Meta` and a `Table Spacer` style of our own), so the Styles
  pane, the Navigation pane, *Insert Table of Figures* and *Cross-reference › Figure* work.
- Figures are inline pictures (`wp:inline`, alt text, `noChangeAspect`), scaled to the text width (or the page height
  minus the caption for tall figures, so figure and caption stay on one page), with a `Figure n.` caption whose
  number is a `SEQ Figure` field.
- Tables: `tblGrid` + `tcW` widths that sum to the text width, `tblHeader` on the first row, `cantSplit` on every
  row, a spacer paragraph after each table (two adjacent tables would otherwise merge, and a table must not be the
  last thing in the body).
- `settings.xml` sets compatibility mode 15 (no *Compatibility Mode* in the title bar), `footer1.xml` prints
  *Page X of Y*, `docProps/core.xml` carries title, subtitle, author (the application) and creation time.
- Element order follows the schema everywhere (`pPr`, `rPr`, `tblPr`, `tcPr`, `sectPr`, styles, numbering levels);
  tabs are `<w:tab/>`, never a tab character in `<w:t>`; every `r:id` used has a relationship; `docPr` ids are
  unique; text is XML-escaped and free of characters Word rejects.

## How it was checked

- `tests/docx_test.cpp` (part of `make test`): ~90 checks on the ZIP container, the inline and block converters,
  heading mapping, lists and `numbering.xml`, tables, links, escapes and whole documents; with two arguments it
  writes a typical report and a stress document containing every construct above (`make docx-samples` →
  `build/host/sample.docx`, `build/host/stress.docx`).
- Schema validation with the Open XML SDK (the rules Word applies), via the CLI packaging at
  <https://github.com/mikeebowen/OOXML-Validator>: both samples give `[]` (no errors) for Office 2013, 2016, 2019,
  2021 and Microsoft 365. The CI workflow runs this on every push. For comparison, the 1.10.0 writer's output had
  9 schema errors (misordered `pPr`/`rPr` children) and the layout problems listed in the README.
- `python-docx` loads both samples (paragraph and table counts as expected); LibreOffice renders them as intended
  (visual check of every page); `xmllint` accepts every part.
- Microsoft Word was not available on the build machine. Open a real report in Word once after changing the
  writer: it should open without a repair prompt, without *Compatibility Mode*, and the Navigation pane should list
  the headings.

## Native citations and vector figures (1.12)

The writer's citations are exported as Word's own citation fields, so the reader can restyle them in Word:

- Every cited paper becomes a `b:Source` in `customXml/item1.xml` (a `b:Sources` part with `SelectedStyle` set to
  the document's citation style: APA, IEEE, Harvard, Chicago, MLA, Vancouver, … mapped to Word's `.xsl` names),
  with `b:Tag`, `b:SourceType` (JournalArticle / Book / Misc), authors as `b:Person` (Last / First), title, journal,
  year, volume, issue, pages, DOI and URL.
- Each in-text citation is `w:sdt` with `w:citation` in its properties around a `CITATION tag \l 1033` field whose
  cached result is the text the writer shows (so the document reads correctly before Word updates fields; the
  multi-paper form is `CITATION a \l 1033 \m b \m c`).
- The reference list is a `w:sdt` with `w:docPartObj` / `w:docPartGallery Bibliography` around a `BIBLIOGRAPHY`
  field whose cached result is the writer's own reference paragraphs (style `Bibliography`). *References › Style*
  in Word changes both, and *Update Citations and Bibliography* regenerates them from the sources.
- Figures are inline pictures with a PNG blip; when a figure's format is *vector* (the default), the same drawing
  is also written as SVG (`asvg:svgBlip` in the picture's extension list) — Word 2016 and newer draw the SVG, older
  versions the PNG. The document default and each figure's override are set in the writer.

The Open XML SDK validator reports `[]` for documents with citations, the bibliography and SVG figures
(`cite-apa.docx`, `cite-ieee.docx`, `figure-options.docx` in the samples). Microsoft Word itself was not available
here: after installing 1.12, open one exported report in Word, check that a citation shows a field shading on
click, that *References › Style* changes it, and that *Update Citations and Bibliography* rebuilds the list.

## The clipboard: copy from the writer, paste into Word (1.13)

`src/core/docclip.cpp` turns the copied fragment into the HTML dialect Word writes itself, and the Windows layer
puts it on the clipboard as `HTML Format` (CF_HTML, with the byte-offset header) next to `CF_UNICODETEXT` (and
`CF_DIB` when the selection is one figure). Word takes the HTML; everything else takes the text or the picture.

What the HTML contains:

- Paragraph classes with `mso-style-name` in a `<style>` block, so Word maps them onto its built-in styles:
  `MsoNormal`, `MsoTitle`, `MsoSubtitle`, `MsoCaption`, `MsoQuote`, `MsoBibliography`, `h1`–`h4` (*Heading 1–4*),
  `pre` (*HTML Preformatted*); real `<ul>` / `<ol>` (type disc / circle / square, 1 / a / i by level; `start` for
  continued numbering); `table.MsoTableGrid` (*Table Grid*) with `border-collapse`, per-cell widths in pt and a bold
  first row for the header; alignment as `align=` + `text-align`; `<b> <i> <u> <s> <sub> <sup>`, `font-family`,
  `font-size`, `color`, `background:yellow; mso-highlight:yellow`, `<a href>`.
- Pictures as PNG files in `%TEMP%\VOSStudio\clip\clip_imageNNN.png` referenced by `file:///` URLs (Word reads
  them at paste time, as it does for its own clipboard HTML); width/height in px from the figure's width and the text
  width. Captions carry `Figure ` + a `SEQ Figure \* ARABIC` field, table captions a `SEQ Table` field.
- **Citations** as Word fields written the way Word's own HTML writes fields — conditional comments that only Word
  reads: `<!--[if supportFields]><span style='mso-element:field-begin'></span> CITATION Doe21 \l 1033 <span
  style='mso-element:field-separator'></span><![endif]-->(Doe & Roe, 2021)<!--[if supportFields]><span
  style='mso-element:field-end'></span><![endif]-->` (multi-paper: `CITATION a \m b \m c \l 1033`). Browsers and
  other editors see only the result text.
- **The sources** (1.14 — see the next section): a `<link rel=dataStoreItem …>` to the `b:Sources` part is still
  emitted, but Word 365 was seen to ignore it in *clipboard* HTML (it honours it in *Web Page* files only), so the
  sources now go into Word's list through Word itself, by automation, at copy time.
- **The reference list**: when the selection includes the writer's generated reference paragraphs, they are the
  result of one `BIBLIOGRAPHY` field (field-begin in the first entry, field-end in the last). A plain copy never
  appends a bibliography (a reference list in the middle of a document is not wanted); *Write › Copy with reference
  list* is the explicit way to get a *References* heading plus that field for exactly the sources the selection
  cites.

The other direction (`docFragmentFromHtml`): Word's clipboard HTML — `MsoListParagraph` with `mso-list:l0 level2
lfo1` (level from `levelN`; numbered when the bullet text Word hides in `<![if !supportLists]>` starts with a
number or letter followed by `.` / `)`), `MsoCaption`, `MsoTitle`, `MsoQuote`, `MsoBibliography`, headings,
`MsoTableGrid` tables (a `<th>` or all-bold first row is the header, per-cell `text-align`), `<o:p>` and field
comments dropped, entities decoded, whitespace collapsed as a browser would — becomes blocks (`DocFragment`) that
`Editor::paste` inserts. Browser HTML (`<div><p><strong>…`) takes the same path; plain text still goes through the
Markdown detector.

Covered by `tests/clip_test.cpp` (structure of the emitted HTML, CF_HTML offsets, a round trip through the importer,
a Word 365 clipboard sample, a browser sample). Verified in Word 365 by a user (1.13): the formatting and the
`CITATION` fields paste as intended; `rel=dataStoreItem` is **not** read from clipboard HTML — hence 1.14 below.

## Sources into Word's list, and vector figures on the clipboard (1.14)

**Sources by automation** (`src/win/word.cpp`). Word's *Manage Sources* list is not reachable through the clipboard;
it is reachable through Word's own object model. On every copy that carries citations, the writer asks the running
Word (`GetActiveObject("Word.Application")`, late-bound `IDispatch`, no type library or SDK headers) for its
`ActiveDocument` and calls `Bibliography.Sources.Add(<b:Source …>)` for each cited source that the document does not
have yet (existing sources are recognised by their `Tag`, the same three-letters-plus-year tags the HTML field codes
use, so the pasted `CITATION Roe19` fields resolve at once). The XML per source is exactly what the `.docx` export
writes (`docBibSourceItems`). The status line names the Word document the sources went to; if Word is not running,
a plain copy stays quiet (the `.docx` route still has everything). *Write › Paste into Word at the cursor*
(`Ctrl+Shift+C`, also in the context menu) does the whole round trip: adds the sources, starts Word if needed
(`CoCreateInstance`, visible, a new document), `Selection.Paste`, updates the fields in the pasted range so the
citations show in Word's current style, and brings Word to the front. Word busy in a dialog (`RPC_E_CALL_REJECTED`,
`RPC_E_SERVERCALL_RETRYLATER`) is reported, not retried. The behaviour is a setting (*Settings › General › Add cited
sources to the open Word document when copying*). The user then inserts the bibliography and picks the style in
Word — the list is Word's.

**Figures as vector graphics.** A selected figure copies on its own (`Editor::copy` treats a block selection as a
one-block fragment). Figure-only copies put **`CF_ENHMETAFILE`** first (a GDI+ metafile, `EmfTypeEmfPlusDual`, frame
in points, drawn from the `Scene` primitives — rectangles, sphere-shaded circles, paths with dashes and round caps,
text with faces and halos, images — `src/win/emf.cpp`), then a `PNG` format and `CF_DIB` for everything else, and
deliberately **no HTML** (Word ranks HTML above pictures, and HTML can only carry a picture by file). Word,
PowerPoint and Excel paste the metafile as a scalable drawing. Mixed text + figure copies stay HTML; vector figures
in them travel as Word's own VML picture syntax — `<!--[if gte vml 1]><v:shape …><v:imagedata src="file:///…
clip_imageNNN.emf"/></v:shape><![endif]--><![if !vml]><img src="…png"><![endif]>` — so Word takes the `.emf` and
every other consumer the PNG. A figure whose per-figure format is *PNG* stays a picture in both routes.

**Not verified in Word here** (no Windows in the build sandbox): the COM calls follow the documented Word object
model and the metafile/VML syntax mirrors Word's own clipboard HTML, but neither has been exercised against a real
Word in this release; the paths are small and isolated (`word.cpp`, `emf.cpp`, the figure branch of `docclip.cpp`)
if adjustments are needed.

## Where the Word collaboration can go next

Ordered by value for the effort, all compatible with the current design:

1. **Citations back from Word.** The importer drops Word's field comments today; reading `CITATION <tag>` fields
   and mapping the tags (deterministic per source) back to `Document.refs` would make a paste *from* Word keep
   its citations as keyed citations. Sources Word knows but the document does not could be imported from the
   pasted `dataStoreItem` when Word writes one (it does in *Web Page* saves; clipboard behaviour to be checked).
2. **Linked figures.** Tag each exported picture (`docPr descr`) with the chart id, its arguments and the project
   path; *Write › Update figures in a Word document…* opens a `.docx`, finds the tagged pictures and replaces their
   PNG / SVG parts with fresh renders from the current data — a Word document that follows the map without
   re-inserting anything. The `.docx` writer already produces every part this needs; the reader side is a zip walk.
3. **`.docx` import into the writer** (a `w:document` → `Document` parser: paragraphs, runs, styles, tables,
   lists, pictures, `CITATION` sdts) — *edit in Word, bring it back*. The HTML importer covers the structure; this
   would cover fidelity.
4. **Reference-manager routes** for people who cite with Zotero / Mendeley / EndNote rather than Word's own
   sources: export the cited sources (or the corpus selection) as BibTeX / RIS / CSL-JSON, so their Word plug-in
   cites the same papers; the BibTeX export exists for the whole corpus, a *cited only* filter is small.
5. **A Word add-in** (Office.js task pane talking to the running application over a local port): insert a chart
   or the current map view at the cursor in Word, insert a citation from the loaded papers, refresh linked figures
   — the "live" end of the same collaboration. Larger: needs the local server and a manifest; worth it once 1–3
   are in.

## Changing the writer

Keep new elements in schema order (see the comment block at the top of `docx.cpp`), add a check to `docx_test.cpp`,
run `make test` and `make docx-samples`, validate the samples with the OOXML validator, and open them in Word.
