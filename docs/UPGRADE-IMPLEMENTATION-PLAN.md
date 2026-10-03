# VOSStudio upgrades — approved implementation plan

| Status | Approved; implementation proceeds in independently testable phases |
|---|---|
| Approved | 2026-10-02 |
| Scope | Two collaborating workspaces, PDF-reader upgrades, a visual-first LaTeX writer, and responsive performance for 10k+ node networks. |

> Implementation is underway. The first phase establishes workspace routing and shared-project navigation, followed by focused reader, writer, and large-network work. The plan records the approved target; individual acceptance items still require implementation and validation.

## Executive recommendation

Keep one project and one set of data, but present it through two purpose-built workspaces. **Visualization** should foreground data, map building, exploration, analysis, and figures. **Bibliography** should support day-to-day reference management with a familiar Zotero-like workflow: organize and search papers, manage collections and metadata, review and annotate PDFs, keep notes, and cite papers while writing. It does not need a network-visualization destination; network exploration belongs in Visualization. Switching workspaces must not duplicate or detach the corpus, map, PDF library, citations, or document: they already live under the shared `Project`/application state.

Treat the writer as a visual editor first. A structured document model should generate LaTeX; Tectonic should compile that LaTeX to the PDF used for preview and export. A source view can be available as an advanced option, but ordinary writing, equations, citations, figures, and templates must not require users to type code.

Deliver the work in independently testable phases. First establish the workspace shell, then improve PDF interactions, then extend the writer model and Tectonic pipeline, and separately profile and optimize 2D/3D performance. Preserve existing `.vosproj` files and make any unsupported LaTeX constructs visible rather than silently dropping them.

## 1. Current-state audit

| Area | Verified baseline | Consequence for the plan |
|---|---|---|
| Shared project | `Project` owns the corpus, network/map, PDF library, and saved document; the app loads the document into `wdoc`. PDF annotations and notes are project data. | Implement workspaces as views over the same project, not separate data stores. Bridge actions should use stable record/asset IDs rather than copied project data or transient row indexes. |
| Navigation | The left rail now has explicit Visualization/Bibliography workspaces, separate navigation trees, independently scrolling destinations, expansion/collapse, and tooltips. Page width/DPI variants still need Windows manual verification. | Keep workspace routing over the same Project, test narrow widths and scaling, and preserve return context when crossing workspaces. |
| Data navigation | Data now has Overview/Sources/Search/Clean sections, with a compact summary and source management separated from search and cleanup. | Continue tuning density and validate the grouped flow with real corpus sizes. |
| PDF reader | PDFium work runs on a worker. The existing annotation model now persists freehand Ink paths, optionally reads/writes standard PDF InkList data, supports drag-to-move and Ctrl+Z, and burns project drawings when printing. PDFium integration tests still skip without the optional runtime, and Windows UI behavior is not yet manually verified. | Continue reader polish and validate saved-file round trips and print output with the pinned PDFium build and on Windows. |
| PDF text | `TextPage.lines` still relies mainly on PDFium newline characters. Selection normalization now decodes UTF-8 by code point and handles common hyphen-like line-wrap breaks before lowercase letters; the geometry-based visual-line model remains outstanding. | Build visual lines from character geometry and keep copy, annotations, quotes, and search on one normalized selection result. |
| Touchscreen | The Win32 shell now registers for `WM_TOUCH`, converts single-contact taps/drags into the existing UI input path, supports drag-to-scroll and multi-contact pinch zoom, and expands sparse UI hit targets without overriding an exact neighbouring hit. Reader and Writer page surfaces also have touch-drag panning. The Windows target and physical touchscreen path have not been compiled or exercised here; the OS touch keyboard is not explicitly summoned by the app. | Compile on Windows, then test taps/drag cancellation, nested scrolling, map/3D gestures, Reader/Writer zoom and pan, multi-touch cancellation, mouse regression, and text entry with the Windows touch keyboard on physical hardware. Do not mark touchscreen support complete until these checks pass. |
| Reading filters | “Dark page” remains an explicit opt-in inversion; separate Original, Warm paper, and Grayscale modes now apply as display-only filters and never change source PDF bytes or print/export colors. | Continue validating contrast, photo handling, and filter switching under zoom/rotation. |
| Writer | `Document` models rich paragraphs/spans, tables, references, and vector scene figures; PDF, DOCX, HTML, and visual-model-to-LaTeX bundle export exist. Inline equations can be inserted/edited in an in-app LaTeX editor; a bounded math parser generates Word OMML and clipboard MathML for supported expressions. Tectonic-only compile/recompile, side-by-side PDF preview, and system-viewer actions are implemented. The official pinned Tectonic executable is embedded in the Windows app and extracted on demand; Tectonic's separate TeX support bundle is fetched/cached by Tectonic. The equation canvas preview is still linear rather than full typesetting, unsupported math falls back to visible source text in DOCX, and the Windows UI/Word interoperability paths remain uncompiled and untested here. Richer equation rendering, publisher templates, LaTeX import, and formatting-faithful Word interchange remain outstanding. | Continue visual math authoring/rendering and Word fidelity; keep code optional, preserve unsupported content visibly, and validate the Tectonic runtime and all Windows paths. |
| Network performance | `vosLayout()` keeps exact repulsion through 512 nodes and uses a Barnes–Hut quadtree/octree above that. Its default opening angle is now 0.8 after exact-oracle checks and 10k/20k 2D/3D smoke measurements. Leiden's minimum-cluster-size merge now scans adjacency rather than rescanning all edges per node. The 3D renderer no longer rebuilds link instances for camera-only motion and submits one segment for straight/bundled links instead of 14 discarded segments. A default 10k 3D solve still takes about 19.7 s here (three starts, automatic 150 iterations), so this is not the final performance target. The 3D job still starts independently from random positions; a 2D-seeded warm start or multilevel/coarse-to-fine solve needs quality comparison. Screen-space picking/selection indexes, label-overlap acceleration, full render profiling, and LOD remain. | Preserve exact small-graph behavior, compare warm-start/multilevel quality against current 3D results on varied graphs, and profile every visualization stage on Windows/GPU hardware. Keep selection exact and avoid silently dropping saved nodes or links. |
| Build | The latest complete host test run passes all 28 test programs, including ID-aware import/export format fixtures, whole-project record/library migration, LaTeX/OMML tests, and 10k layout checks. `pdf_test` skips because the optional Linux PDFium library is absent. No MinGW/Windows cross-compiler is installed, so the Windows UI—including the new `WM_TOUCH` path—has not been built. CI now pins and checksums PDFium and Tectonic, requires both for the app build, and uploads the Windows executable and portable ZIP after a successful push. | Keep the optional Linux PDFium integration test explicit and run the Windows build in CI; validate the target and touch UI on Windows hardware when available. |

### Relevant code map

- Shell/navigation: `src/win/app.h`, `src/win/app.cpp`, `src/win/menus.cpp`, `src/win/pages.cpp`.
- Shared project and bibliography: `src/core/project.h/.cpp`, `src/core/library.h/.cpp`, `src/win/papers.cpp`, `src/win/readlib.cpp`.
- Reader/PDFium: `src/win/reader.cpp`, `src/core/pdfdoc.h/.cpp`, `src/core/pdfium.h/.cpp`.
- Writer: `src/core/doc.h/.cpp`, `src/core/docmd.cpp`, `src/core/docclip.cpp`, `src/core/docx.cpp`, `src/core/docexport.cpp`, `src/win/writer.cpp`, `docs/EDITOR.md`, `docs/WORD-EXPORT.md`.
- Analysis/layout/rendering: `src/core/analysis.cpp`, `src/core/algo.cpp`, `src/win/netview.cpp`, `src/win/netview.h`.
- Build/packaging: `Makefile`, `tools/fetch-pdfium.sh`, `installer/`, `.github/workflows/`.

## 2. Two-workspace product structure

### 2.1 Shared shell and workspace navigation

Add an explicit **Visualization / Bibliography** workspace switcher to the left navigation. The selected workspace gets its own compact, grouped tool list; the global command palette, app settings, and assistant remain reachable from either workspace. Keep the current project, selection context, and open document state alive when switching.

Suggested navigation (labels can be refined in review):

| Visualization workspace | Bibliography workspace |
|---|---|
| **Data** — import, dataset health, sources | **Papers** — searchable library, collections, metadata, and duplicate cleanup |
| **Build** — create/rebuild a map | **Review** — reading queue, review status, tags, and notes |
| **Map** — network, overlay, density, timeline, geography, 3D, matrix | **Read** — linked PDFs and annotations |
| **Analyse** — clusters, items, network measures, stability | **Write** — visual-first writing pad, citations, and LaTeX preview |
| **Trends** — year-based analysis | — |
| **Actors** — authors, sources, organisations, countries | — |
| **Publish** — figure composition and export | — |

The Bibliography workspace should be library-first and support a complete daily reference workflow: add/import papers, search and filter, organize with collections/tags, correct metadata, resolve duplicates, track reading/review status, open attached PDFs, add notes/annotations, and cite from Writer. “Zotero-like” here describes the workflow and information architecture; it does not promise compatibility with Zotero’s database, API, or library files unless that is explicitly requested. See the [Bibliography implementation plan](BIBLIOGRAPHY-UPGRADE-IMPLEMENTATION-PLAN.md) for the current-state audit, persistence/migration policy, phased scope, and UI acceptance checklist.

The map’s visualization modes should remain subviews of Visualization **Map**, not seven more primary-navigation destinations. The Bibliography workspace has no standalone Network page. A **Show on map** or **Analyze selection** action from a paper or collection should switch to the relevant Visualization tool, preserve the originating Bibliography state, and make it easy to return.

### 2.2 Responsive behavior

Implement three navigation states, selected from available logical width and DPI rather than hard-coded screen pixels:

1. **Expanded:** readable labels and grouped headings when there is room.
2. **Compact:** icon rail with full tooltips and the current workspace visibly marked.
3. **Narrow:** a dismissible drawer/flyout so the workspace switch and every tool remain reachable without shrinking the content below its usable minimum.

The navigation column scrolls independently of page content. Keep the active destination visible after workspace switches, provide a stable keyboard path through workspace and tool items, and avoid relying on color alone for active state. Test window resizing and 100%, 150%, and 200% Windows display scaling. This is desktop responsiveness; it does not imply a mobile application.

### 2.3 Less-dense dataset navigation

Keep the Data page’s capabilities, but replace its “everything in one long page” feel with four named destinations: **Overview**, **Sources**, **Search**, and **Clean**. Put the import/add-file action and main counts first; keep detailed field coverage and OpenAlex controls within their own sections. Show source provenance and records flow together under Sources, and spelling/thesaurus tools under Clean. This tabbed split is implemented structurally; long candidate/file lists and further progressive disclosure remain to be reviewed and validated. Do not remove information or silently reset the user’s current dataset.

This plan interprets “less-dense dataset navigation” as reducing the density of the existing Data/Build workflow and making its destinations easier to scan. It does not assume a new multi-project or multi-dataset manager.

### 2.4 Cross-workspace contracts

Use the shared project as the integration boundary and make bridge actions explicit:

- From a map node, cluster, or analysis result: **Open paper**, **Open PDF**, **Review these papers**, or **Analyze this selection**. Preserve the originating map camera, view, filters, and selection so returning restores context.
- From a paper, collection, or selected group in Bibliography: **Show on map** and **Analyze selection** open the corresponding tools in Visualization. Resolve papers by stable linked record keys and report when the current map has no corresponding node; do not add a Network destination to Bibliography.
- From a PDF selection: **Quote**, **Cite**, or **Add as figure**. Preserve page attribution and the linked bibliography citation.
- From any map, chart, or analysis view: **Insert figure in Writer**. Default to a deterministic snapshot; offer refresh/relink as a deliberate action so later map edits do not silently alter a manuscript.
- From Writer: insert citations from the shared bibliography, and insert maps, charts, or PDF-derived figures without losing source metadata.

Acceptance checks must exercise the everyday reference flow (add/import → organize → review/annotate → note → cite), plus cross-workspace actions in both directions. Save and reopen the project and verify that collections, paper keys, citation links, selected records, and figure assets still resolve; returning from Visualization should restore the prior Bibliography state.

## 3. PDF reader plan

Keep the current PDFium worker and bounded tile/page caches. Extend the existing reader instead of moving PDF work onto the UI thread.

### 3.1 Text selection and hyphenated wraps

1. Build visual line/paragraph groups from each character’s PDF page-space box and baseline, including rotated pages and columns; do not depend only on explicit newline code points.
2. Keep character-index, code-point, UTF-8-byte, and rectangle mappings consistent so a visual selection and its highlight rectangles describe the same text.
3. Normalize line wraps using Unicode code points. Remove an explicit soft hyphen at a wrap; handle Unicode letters and punctuation; insert spaces for ordinary line breaks; preserve a visible hard hyphen when the text is ambiguous rather than joining unrelated words. Make any higher-confidence hyphenation heuristic deterministic and testable.
4. Route copy, annotation text, notes export, PDF quote insertion, and selection search through the same normalization function.

Test PDFium-generated and real-world pages with lowercase and uppercase continuations, non-Latin text, combining marks, soft hyphen U+00AD, hyphen/minus/en/em dashes, true compound words, ligatures, columns, page rotations, and scans without selectable text. Verify that the copied passage, visible selection, highlight, and writer quote agree.

### 3.2 Rendering and reading filters

- Keep tile rendering and the previous-resolution fallback, but derive target pixels from effective zoom and display DPI. Prioritize visible-page tiles after zoom settles; use a sufficiently sharp visible-page preview while higher-resolution tiles arrive; retain a lower-cost preview for offscreen thumbnails. Do not let stale filter/rotation/zoom jobs overwrite a newer page generation.
- Keep cache limits and eviction observable. Tune tile size and preview resolution from tests on high-DPI screens rather than simply increasing the whole-page bitmap size.
- Add display-only non-inverting filters such as warm paper/sepia, blue-light reduction, and grayscale. Keep original PDF color as the default and ensure relative light/dark order and useful contrast remain intact. The filter must never mutate the original PDF, annotation colors, saved PDF, copied area, or printed/exported output.
- Treat the application theme separately from page color. Review whether the existing luminance-reversing “Dark page” remains as a clearly separate opt-in mode or is replaced; it must not be applied by the new non-inverting filter controls.

### 3.3 Annotation interaction and compact toolbar

- Expose **Strikeout** in the selection toolbar, context actions, and a shortcut. The project model (`kind == 2`) and PDFium strikeout subtype already exist; this is primarily a missing UI path plus end-to-end verification.
- Add a **Draw** tool based on page-space ink paths. Support freehand strokes with color/width, edit/delete/undo, rotation-safe coordinates, and project save/load. Write/read standard PDF Ink annotations so saved marks are visible in other PDF viewers. Add line/rectangle/arrow shape tools only if confirmed in review; they are not required to ship the first freehand-ink slice.
- Replace the current two-row, text-labeled selection mini bar with one compact icon-first toolbar. Use accessible tooltips and keyboard equivalents; show the active color/code clearly; use an overflow or vertical layout at narrow widths; clamp placement to the page viewport. Keep common actions (highlight, underline, strikeout, comment, copy, quote, cite) discoverable without covering the selected passage.
- Set cursor by interaction context: text I-beam in selection mode, hand over links, pen over ink mode, crosshair for area/shape selection, grab/grabbing for pan, and arrow over ordinary controls. Do not override the cursor while a text input or popup owns the pointer.

Round-trip tests must cover project JSON, existing PDF annotation import, write/save/reopen, rotation, printing/burning, and the PDF reader’s visual overlay. Test at least one external viewer because internal appearance alone is not proof that a standard annotation was written correctly.

## 4. Visual-first LaTeX writer

### 4.1 Editor experience

- Start the writer’s side pane **closed**. Reopen it through an explicit Outline/Properties/Find control and remember the user’s preference. On narrow windows, use a drawer or overlay instead of consuming the page canvas by default.
- Keep the page-based visual editing surface. Add a paired **PDF Preview** that is produced by Tectonic and is the authority for final LaTeX layout. Provide a clear compile state, progress/cancel, warnings/errors, and a one-click jump to the affected visual block where the source mapping is known.
- Keep a generated-source view optional and advanced. Normal tasks—formatting, equations, citations, figures, templates, and export—must be completable without typing LaTeX.

### 4.2 Semantic document model and media

Continue using `vs::Document` as the canonical editable model and add versioned semantic types rather than storing only rendered pages or only generated TeX:

- Template-aware document metadata and semantic paragraph roles (title, abstract, section levels, keywords, acknowledgements, references, captions, lists, quotes, tables).
- Inline and display equation nodes. Store a structured math tree plus a normalized TeX representation/alt text so the visual controls, LaTeX generator, and Word exporter can share meaning.
- General media assets with stable IDs, type, title/alt text, source, dimensions, and original bytes. PNG can be placed directly; PDF can be included as a selected page/crop and remain vector in LaTeX; SVG needs safe in-app conversion or a verified PDF/PNG fallback instead of requiring shell escape. Word/copy should use SVG/PNG/EMF or high-resolution fallback as supported.
- Keep project-owned writer assets separate from the existing PDF library’s linked-in-place source PDFs. Prefer content-addressed project assets with a manifest and cleanup of unreferenced files; review whether the portable project should be a sidecar asset directory or a single archive before committing to a storage-format change.

Add a document schema version and migrations for old `document` JSON. Existing documents without the new fields must load unchanged. Cross-workspace figures and references use stable IDs/keys, not record-vector indexes.

### 4.3 Visual formatting and equation tools

Add structured toolbar groups for text style, heading level, alignment, lists, tables, links, captions, citations, and page setup. Add a visual equation builder for common academic structures: symbols/Greek letters, superscripts/subscripts, fractions, roots, sums/integrals, brackets, matrices/cases, and common operators/relations. Users build these through templates, palettes, and keyboard navigation; a source field may be offered as an optional escape hatch.

Prototype the equation preview renderer before selecting a dependency. The Tectonic PDF remains the faithful document preview; a lightweight local math renderer or equation-builder preview provides immediate editing feedback without forcing a full-document compile for each keystroke.

### 4.4 Tectonic preview and export

- Use a pinned, verified Tectonic Windows executable in a cancellable background process, not a shell command assembled from document text. Validate the exact supported CLI/version before implementation; the official documentation describes a stable V1 CLI and labels the V2 interface as prototype.
- Compile generated source in a per-job temporary working directory. Disable shell escape and compile imported/untrusted source in Tectonic’s untrusted mode. Stage only the document and approved assets in that directory; impose a compile timeout/cancel path; capture diagnostics; atomically publish the resulting PDF. Do not make a security claim beyond these implemented controls.
- Tectonic can retrieve support files on demand and has a local cache. Make network/package download explicit and report what is being fetched; support offline compilation from cached resources. Pin the engine and package bundle for repeatable output. Keep the app usable when Tectonic is unavailable: visual editing and non-LaTeX exports remain available, with a clear install/repair prompt for compile/LaTeX export.
- Reuse the PDFium reader’s tiled PDF display for the generated preview where practical. Export the same compiled PDF that the preview shows; do not re-typeset a different PDF for export.
- The Tectonic codebase is MIT-licensed, but the TeX bundle, fonts, classes, and templates have their own notices. Inventory and ship notices for every bundled runtime/template component.

### 4.5 Publisher templates

Start with a curated, versioned template catalog, not an unverified “all journals” collection. Candidate families for compatibility spikes are IEEEtran (journal/conference), ACM `acmart`, Elsevier `elsarticle`, and Springer Nature `sn-jnl`; Springer LNCS is a separate conference/proceedings template. Validate each against Tectonic, including its class options, bibliography tooling, required fonts/assets, current author instructions, and redistribution rights. A template manifest should record source URL, revision/date, license, class/options, citation/bibliography mode, engine requirements, and known unsupported features.

The GUI should distinguish publisher/journal family and document purpose (e.g. conference vs journal), retain the user’s content when changing templates, and report formatting that cannot be represented by the visual model. Template updates must be reviewable and testable; do not promise that every venue-specific template is supported by the first release.

### 4.6 LaTeX import, Word export, and copy

- Import `.tex` and project archives containing `.tex`, `.bib`, and figures. Parse a documented subset into the visual document model: title/author/abstract, sections, common text formatting, lists, tables, citations/references, links, figures, labels, and common math environments.
- Compile the original imported source through Tectonic before conversion when possible. Preserve the original source, preamble, bibliography, and assets. Unsupported macros/environments must be marked as opaque/source-preserved content with warnings; never silently delete them or claim arbitrary LaTeX can be losslessly converted into a visual document.
- Generate `.bib` from the shared reference model with stable citation keys. Configure the citation commands and bibliography style per template. Treat Biber or other external bibliography tools as an explicit compatibility/dependency decision rather than assuming every template works with the bundled engine.
- Map supported semantic content to Word OOXML styles and Office Math ML; preserve tables, captions, links, citations, and media as faithfully as the target permits. Copy uses the existing Word-oriented HTML/clipboard path, extended for equations and all supported assets. PDF placed media will need an image fallback for Word, while preserving the original asset in the project.
- For imported content that cannot map semantically to Word, show a conversion report and offer a non-lossy alternative (keep LaTeX/PDF or copy a rendered fallback) rather than flattening the entire document without consent.

Acceptance must include schema validation of generated DOCX, open/visual inspection in Microsoft Word, copy/paste into Word, and round-trip tests for equations, tables, embedded media, and native citation fields. Schema-valid DOCX alone is not sufficient proof of visual fidelity.

## 5. Responsive 10k+ visualization performance

### 5.1 Measure before changing algorithms

Add reproducible synthetic fixtures for 1k, 10k, and 25k nodes with sparse and denser edge counts. Measure 2D and 3D separately, including map build, layout, clustering, rendering, density, label placement, hit testing, selection, and memory. Record median and p95 input-frame latency, layout duration, allocations, GPU uploads, and cache hits. Use the existing performance HUD where possible and add scoped timers where it cannot identify the bottleneck.

Treat “responsive” as a measurable gate: normal navigation, pan/orbit, zoom, hover, and selection must not wait for a layout job; target interactive frame timing should be agreed from representative hardware after the baseline is captured. Heavy builds remain background, report progress, and cancel promptly. Avoid a blanket promise of a particular frame rate on every GPU.

### 5.2 Layout and analysis

- Preserve exact layout for small graphs and as a correctness oracle. For large graphs, prototype a Barnes–Hut-style spatial tree: quadtree in 2D and octree (or a measured equivalent) in 3D for long-range repulsion, with exact near-field interactions and exact edge attraction over the sparse edge list.
- The VOS layout accepts several attraction/repulsion exponents and currently computes both gradients and energy with all-pairs loops. Validate the approximation for supported exponent settings, pinned nodes, random seeds, and layout quality; compare against exact runs on small/medium graphs. Decide how the line-search/energy acceptance check is approximated—do not leave an O(N²) energy pass that erases the tree speedup.
- Keep deterministic behavior where current seed semantics promise it. Calibrate the tree opening threshold/error against layout quality, not only speed. Consider multilevel/coarsened initialization if tree-only optimization does not provide a good first layout.
- Audit the 16-million pair-update memory guard and the UI’s 10–20k item limit. Surface exactly when a dense input is limited and which links are omitted; tune or redesign the budget from measured memory rather than silently presenting an incomplete network as complete.

### 5.3 Renderer and interaction

Keep GPU-instanced node drawing. Profile and reduce CPU work before replacing the renderer:

- Split invalidation for node depth order, node data/positions, link data, and camera matrices. 3D orbit may require node depth sorting, but it should not rebuild/upload unchanged links every frame.
- Add a screen-space spatial index for 2D hover/hit testing and rectangle selection; use projected-depth-aware bins for 3D. Update it only when positions, camera, or visibility changes.
- Cache density-neighborhood work, link instance data, and label layouts by explicit data/style/position keys. Cull offscreen links/nodes where safe and use zoom-aware link/label level of detail; always draw selected/hovered incident links above the background budget.
- Keep selection/filter semantics exact even when low-zoom rendering uses visual LOD. Never silently remove records or links from the saved network to meet a draw budget.

**Renderer hot-path updates (source implemented; Windows compile/GPU profiling pending):**

- `App::canvasSignature()` no longer walks node labels/attributes, positions, flags, cluster names, or override entries on cache checks. It uses scalar/style inputs, storage identity/shape, and the existing dirty flags. `updateFlags()` likewise uses explicit input revisions for selection/search/pin/map/geo changes instead of rehashing every node each frame; ordinary color/label style edits no longer force an unnecessary full flag rebuild. This trades implicit whole-map detection for explicit invalidation; selection/search/pin/map/recluster/score-data mutation sites and the Geo index now bump the revision. Editable cluster names mark the style/canvas dirty; map replacement clears the Geo signature, and corpus version is part of it so same-size affiliation/cleaning updates cannot leave stale country classifications. The linked-pane `computeFlags()` path also invalidates each pane's visibility-dependent caches when Geo/Timeline visibility flags actually change. Score-dependent Timeline positions are keyed by their selected score and refreshed after score/style data revisions in both the main and linked views.
- `NetView::hitNode()` and `nodesInRect()` use a projected screen-cell index rebuilt only when positions, camera, viewport, scale, view kind, or style data changes. Hit-radius candidate lookup preserves the previous nearest/depth tie behavior; rectangle selection still filters exact projected node centers and returns node indices in ascending order.
- Density work now caches the kernel maximum and world-space neighborhood grid by positions, visibility, weights, and kernel sigma. Density-label sampling visits only nearby cells. Splat instances are kept in a reusable dynamic D3D buffer and rebuilt only when their position/visibility/weight/color inputs change. Average item distance is generation-cached and is not computed for non-density plots. Switching between views with identical positions skips the no-op position animation, avoiding repeated density-cache rebuilds during a camera-only transition.
- Label-collision checks use an occupancy grid to narrow candidates but still run the original exact rectangle-intersection test, so cell coarseness cannot suppress non-overlapping labels. Cluster hull points are grouped in one node pass rather than rescanning all nodes once per cluster. 3D camera centroid/radius bounds are cached by display-position generation.
- These are source changes, not measured performance claims. The next profiling run should capture p50/p95 build/layout, CPU submission, GPU frame, hit-test and box-selection times across 2D/3D network, density, matrix, timeline/chart and geo fixtures at 1k/10k/20k/50k, then compare the same hardware and settings against baseline.

Performance acceptance: compare baseline and optimized builds on the same fixtures; require no regression for small graphs, correct 2D/3D node/link identities, responsive interaction during background layout, prompt cancellation, bounded memory, and transparent behavior for dense networks that exceed configured budgets.

## 6. Persistence, compatibility, and integration rules

- Keep legacy `.vosproj` files loadable. Add explicit versions/migrations for new writer assets/math and ink annotation paths; test open-save-open on projects from before and after the change.
- Continue to keep PDFs linked in place unless the user explicitly imports/copies a writer asset. Do not copy or modify a library PDF as a side effect of annotating it. “Save annotations into PDF” stays explicit.
- Keep document citations connected through stable bibliography keys/DOIs where possible. Relink with a visible warning if a record is missing; do not bind new content to a reused array index.
- Keep third-party runtime, class, font, and template licenses/provenance with the distribution. For imported TeX, compile on a background worker in a unique temporary directory with shell escape disabled by default; document compiler package/network behavior and do not imply that disabling shell escape is an OS-level sandbox.
- The navigation preference (last workspace, expanded/compact state, writer pane preference) belongs in app settings unless review identifies a project-specific need. Page camera, reader page/zoom, writer caret, and filters should remain attached to their own workspace/document state.

## 7. Phased delivery and exit gates

| Phase | Scope and deliverable | Dependency | Exit gate |
|---|---|---|---|
| **0 — Confirm and prototype** | Approved scope and terminology; benchmark the current 10k pipeline; prototype Tectonic launch/cache, media storage, and equation preview choices. Research and planning are documented; performance and runtime prototypes remain pending. | User approval recorded; remaining decisions are listed below | Written decisions for the open questions below; baseline report and small compile/import/media proof-of-concept. |
| **1 — Workspace shell (in progress; Bibliography organization slice implemented, Windows UI validation pending)** | Workspace switcher, responsive/collapsible/scrollable left navigation, less-dense Data landing, and library-first Bibliography workflow. The current slice persists collections, shared record tags and saved views; adds status/PDF/collection/tag/query filtering and selected-record organization; preserves Reader return context; and saves organization-only projects. Network/map tools remain in Visualization and can be opened from Bibliography actions. See the [Bibliography implementation plan](BIBLIOGRAPHY-UPGRADE-IMPLEMENTATION-PLAN.md). | Phase 0 | Resize/DPI/keyboard checks; both nav trees work; everyday paper-management flow succeeds; project data and map/PDF/writer state survive switching and return navigation. |
| **2 — PDF reader** | Geometry-aware text lines and hyphenation, adaptive sharp rendering, non-inverting reading filters, cursor mapping, compact icon toolbar, visible strikeout, freehand ink and standard PDF round-trip. | Phase 0; independent of writer work | Reader/unit tests, PDFium-backed tests, project save/reopen, rotation/print checks, external-viewer annotation inspection, high-DPI manual smoke. |
| **3 — Writer model and visual tools (LaTeX export/equation preview slice in progress)** | The visual document exports a tested LaTeX source bundle through an optional advanced action. Inline equations can be inserted/edited via a visual-first editor with a LaTeX source field; bounded syntax maps to Word OMML and clipboard MathML. A linear Unicode preview is immediate; compiled PDF preview uses the pinned Tectonic executable embedded in the app, with Tectonic alone as compiler. Its TeX support bundle is fetched/cached by Tectonic, not embedded. A full visual equation builder/rendering, broader equation grammar, versioned general media model, broader formatting tools, and template-aware metadata remain. | Phase 0 storage/equation decisions | Existing documents migrate unchanged; new content round-trips; common authoring remains visual-first. Windows compile/runtime and Word visual/copy validation remain pending. |
| **4 — Tectonic runtime and templates** | The official pinned Tectonic executable is checksum-verified at build time, embedded in VOSStudio.exe, and extracted into a versioned LocalAppData folder on demand; PDFium is embedded likewise. The separate Tectonic support-bundle/cache policy, diagnostics, PDF preview/export validation, and tested publisher templates remain. | Phase 3; Phase 0 engine spike | Repeatable sample builds, cancellation/error UI, cache/offline behavior, package notices, and template manifest complete. |
| **5 — Import and Word interoperability** | Subset LaTeX/ZIP importer with source preservation and warnings; generated `.bib`; faithful supported DOCX/copy including equations/media. | Phases 3–4 | Fixture matrix with unsupported-content reporting; OOXML schema validation plus Microsoft Word visual/copy checks. |
| **6 — Large-network performance (layout, interaction and renderer-cache slices in progress)** | `vosLayout()` uses a Barnes–Hut quadtree/octree above 512 nodes; its opening angle is calibrated to 0.8. Leiden min-size cleanup is adjacency-based. 3D camera-only motion no longer rebuilds link instances, straight/bundled links use one GPU segment, and 3D bounds are cached. Canvas signatures/flag invalidation, projected hit/box-selection indexing, density neighborhoods/splats, label collision checks, and cluster-hull grouping now avoid repeated whole-map or quadratic work. 10k/20k 2D/3D layout smoke tests run. Renderer p50/p95 profiling, GPU/device validation, visual LOD, dense-network benchmarks, and explicit performance limits remain. | Phase 0 baseline | Benchmarks at agreed sizes/fixtures; varied exact-oracle/layout-quality tests; no small-graph regression; responsive Windows interaction and documented hardware envelope. |
| **7 — Integrated release hardening** | Cross-workspace acceptance suite, project migration tests, Windows UI smoke, packaging/licensing/offline setup, regression/performance report. | Phases 1–6 | Clean build/test; no unresolved data-loss or navigation regressions; user-visible limitations documented. |

Each phase should be reviewable independently. If a phase changes the `.vosproj` schema or introduces a new runtime, land its migration/notice/tests with that phase rather than postponing compatibility work.

## 8. Validation plan

### Current build/test status

- Fixed the missing recipe tab in `Makefile`’s `$(PDFIUM_WIN)/bin/pdfium.dll` rule and marked `pdfium` phony. `make -n pdfium` now parses and prints both fetch commands.
- Latest `make test` passed all 28 host test programs, including the Bibliography import/export field matrix and legacy identity/library migrations, equation/OMML tests, hybrid-export serialization checks, project save/reopen, 10k/20k 2D/3D layout smoke tests, the exact-objective oracle, sparse min-size Leiden, LaTeX bundles, display filters, and PDF-mark tests. The PDFium integration test reported an explicit skip because `third_party/pdfium/linux-x64/lib/libpdfium.so` is not available.
- Added the opt-in `FigureSpec::hybridExport` setting, saved in project JSON and exposed in Publish plus `figopt hybrid 0|1`. SVG/PDF figure, current-view and chart exports can rasterize contiguous non-text runs into cropped transparent embedded image layers at the configured DPI while leaving native text, links and original run order intact. PNG is unchanged and all-vector export remains the default. Core tests verify image/text interleaving and project persistence. The Windows Direct2D rasterizer/UI path is not compiled or profiled on this runner, so the hybrid raster composition still needs Windows validation.
- Implemented `pdf::joinSelectionLines()` and routed reader copy/selection text through it; coverage includes ASCII/Unicode hyphens, soft hyphens, CRLF, uppercase continuations, en dash, and whitespace.
- Added display-only warm-paper and grayscale bitmap filters, settings/cache identity, and reader controls; dark-page inversion remains a separate mutually exclusive opt-in. The whole-page preview resolution now scales to 55% of the available viewport width (quantized and capped at 520 px); stale preview completions are discarded, and tile cache keys include the active appearance. Core filter behavior is unit-tested; Windows UI rendering still needs a Windows compile/manual check.
- Split Data into Overview, Sources, Search, and Clean tabs (records flow remains in Sources); the current UI change has only source-level checks, not a Windows compile.
- Added `docToLatex()` to generate a TeX document, sanitized relative vector-PDF figure assets, and a companion BibTeX file from the visual `Document`; the advanced Writer/File export action writes the bundle without making source editing part of normal authoring. Host tests cover TeX escaping/injection resistance, text formatting, headings, lists, tables, citations/references, stable/collision-safe BibTeX keys, figure assets, and path sanitization. Writer now offers optional compile/recompile using the pinned, checksum-verified Tectonic executable embedded in VOSStudio.exe and extracted on demand, with untrusted mode, kept logs, one rerun, an in-app side-by-side PDF preview, and system-viewer opening. Tectonic's separate support bundle is still downloaded/cached by Tectonic on demand. An in-app LaTeX equation editor, immediate linear preview, bounded parser, Word OMML export, and clipboard MathML are also implemented and host-tested; unsupported commands stay visible and now produce a Word-fallback warning. The equation syntax tree is transient rather than persisted, and the canvas preview is not full TeX typesetting. Preview is off by default; generated files live in a unique temporary directory. This runner has no Tectonic executable or Windows cross-compiler, and its PDFium integration test is skipped, so Windows compilation and preview rendering remain unvalidated. Tectonic support-bundle/offline-cache policy, richer equation rendering/builder, templates, and LaTeX import remain future work.
- Added freehand Ink paths to project JSON, drawing-specific reader labels/cards/exports, mutually exclusive Draw and Area modes, persistent pen color/width, page-space rotation-safe capture, drag-to-move geometry editing, and Ctrl+Z undo for the latest drawing creation or move. The selection card still supports comments and deletion. The PDFium wrapper now optionally reads/writes standard InkList strokes and reads their width; `pdf_test` covers geometry/color/width on save-reopen plus rendered pixels, and the signature check includes the Ink APIs. The PDFium integration test was built but skipped because the Linux binary is unavailable. Printing now strips this project’s stale `/NM` copies from its private document and burns current highlights, areas, and freehand paths once; new PDF-mark tests cover stroke pixels and rotation without needing PDFium.
- Large-network layout keeps exact all-pairs repulsion through 512 nodes and uses a per-iteration Barnes–Hut quadtree/octree above that threshold. Edge attraction remains exact; the approximation supports 2D/3D, checks cancellation during target queries, and uses approximate repulsion energy for rescaling/start selection. The default opening angle moved from 0.55 to 0.8 after the 384-node exact-objective oracle passed in 2D/3D. With one start and 20 iterations on this runner, the sparse-ring smoke measured 0.99 s (10k 2D), 2.50 s (10k 3D), 2.47 s (20k 2D), and 5.81 s (20k 3D). Compared with the earlier 0.55 10k 3D run (4.74 s), the new 0.8 setting is about 47% faster on that fixture. These are smoke measurements, not hardware-independent latency guarantees or full default-iteration build times.
- Renderer audit found that 3D camera orbit forced both node and link instance uploads even though the GPU transforms static link endpoints with the updated view-projection matrix. 3D depth sorting is now invalidated by yaw/pitch only; link buffers rebuild only when link data/flags/style change. Straight and already-bundled links now submit one quad rather than 14 shader-discarded segment quads. This is source-implemented but awaits Windows/GPU profiling.
- The 3D viewer triggers a distinct `start3DLayout()` solve, rather than simply lifting the current 2D coordinates. After the theta change, a default 10k-node/30k-link 3D solve (three starts, automatic 150 iterations) took 19.7 s on this runner. The 10k/20k smoke tests are only one start and 20 iterations, so their times must not be presented as default build times. The next algorithm spike should compare current random multi-start against a 2D-position warm start with deterministic depth jitter and a multilevel/coarse-to-fine alternative; retain the faster path only if 3D layout quality is acceptable.
- Leiden's optional minimum-cluster-size pass previously scanned all edges for each node (O(VE)). It now builds adjacency once and processes only incident edges with reusable sparse accumulators; `algo_perf_test` covers a 6k-node sparse case.
- Library statistics and the CSV coding matrix now keep text highlights, area selections, drawings, and notes in separate counts/columns. Host coverage exercises the kind-5 JSON path and export counts.
- The latest `make -j2 CROSS=x86_64-w64-mingw32-` attempt stopped before compilation because this runner has no `x86_64-w64-mingw32-g++` executable. The current Windows UI changes therefore still need a full compile/link on a runner with MinGW-w64. `wine`, `Xvfb`, `clang++`, and `cppcheck` are also unavailable here, so no Windows UI smoke/screenshot check was possible.
- `make pdfium` was attempted earlier but the GitHub release-asset connection failed with `curl: (35) SSL_ERROR_SYSCALL` before the PDFium binaries/headers could be fetched. The pinned `chromium/8076` header URL also returned HTTP 503; `FPDFAnnot_AddInkStroke` is an experimental API found in older upstream headers, but its exact pinned-header signature and the new write/render test have not been locally compiled or run. The binding remains optional so an older otherwise-compatible engine still loads; drawing export reports a clear error if that symbol is missing.
- No new application executable is being delivered. Host test/build artifacts are temporary and will be removed; the tracked `VOSStudio.exe` remains at its repository baseline.

### Future phase gates

- Host unit tests for document migrations, math AST/LaTeX generation, TeX import reporting, media serialization, ink paths, and text normalization.
- PDFium-backed tests for text geometry, soft/hard hyphens, drawing annotation save/reopen, rotation, and rendering at multiple scales; keep an explicit skipped status when the binary engine is unavailable.
- Windows manual smoke at common window sizes and DPI settings for nav, reader cursor/selection/toolbar, writer sidebar, clipboard, and PDF preview.
- Tectonic fixtures: clean network bootstrap, cached/offline rebuild, missing package, malformed source, unsupported macro, timeout/cancel, and reproducible output using a pinned engine/bundle.
- DOCX validation against Open XML schemas and visual inspection/paste tests in Microsoft Word for the supported feature matrix.
- Performance harness captures baseline and optimized p50/p95 frame timing, layout duration, cancellation latency, and working set for 2D/3D synthetic graphs.

## 9. Decisions for review

Proposed defaults are recommendations, not implementation decisions:

1. **Navigation terminology and Bibliography destinations:** approve the suggested trees or rename/reorder them. Should a separate Review destination exist, or should status/notes remain tabs inside Papers?
2. **Zotero-like workflow vs. interoperability:** this plan treats Zotero as a day-to-day workflow reference, not a promise of database/API compatibility. Is direct Zotero-library import/export also required?
3. **Dataset navigation:** confirm the interpretation above (a less-dense Data workflow, not multi-project management).
4. **Figure semantics:** default inserted map/chart to a fixed snapshot, with an explicit refresh/relink option, or require live-linked figures by default?
5. **Tectonic distribution — resolved:** embed the checksum-pinned official Windows executable in VOSStudio.exe and extract it on demand. Tectonic's separate support bundle remains managed by Tectonic (downloaded/cached as needed), so the package is not a fully offline TeX installation.
6. **LaTeX import boundary:** accept a clearly documented common-subset visual import with preserved source for unsupported constructs, or require broader import coverage before release?
7. **Embedded asset packaging:** accept a project-managed sidecar asset store for images/PDF pages, or require a single-file portable archive (which is a larger project-format change)?
8. **Equation preview:** approve a prototype spike to choose a local math renderer before the full writer phase; the compiled Tectonic PDF remains the authoritative preview either way.
9. **PDF drawing scope:** the reader keeps dark-page inversion as a separate, mutually exclusive opt-in from the new non-inverting warmth/grayscale filters. Should the first drawing release be freehand ink only, or also include line/rectangle/arrow tools?
10. **Performance target hardware:** identify a representative minimum Windows CPU/GPU/resolution if there is a target machine; otherwise use a documented mid-range reference device and report the measured envelope rather than a universal FPS promise.

## 10. Research references

The following sources inform the implementation choices; they are not a claim that every listed template or package will work in the first release. Template revisions and redistribution terms must be checked again when implementation begins.

- Tectonic overview, bundle downloads/reproducibility, Unicode engine, and code license: [Tectonic project](https://tectonic-typesetting.github.io/en-US/).
- Tectonic installation and prebuilt executable options: [Installation guide](https://tectonic-typesetting.github.io/book/latest/installation/).
- Tectonic CLI compile options, including offline-cache and untrusted modes: [Compile reference](https://tectonic-typesetting.github.io/book/latest/v2cli/compile.html); [V1 CLI reference](https://tectonic-typesetting.github.io/book/latest/ref/v1cli.html). The documentation labels the V2 interface as prototype: [V2 CLI overview](https://tectonic-typesetting.github.io/book/latest/ref/v2cli.html).
- Candidate publisher classes and package metadata: [IEEEtran on CTAN](https://ctan.org/pkg/ieeetran), [ACM `acmart` on CTAN](https://ctan.org/pkg/acmart?lang=en), and [Elsevier `elsarticle` on CTAN](https://ctan.org/pkg/elsarticle).
- Springer Nature’s official journal authoring template and author-support guidance: [Springer Nature LaTeX author support](https://www.springernature.com/gp/authors/campaigns/latex-author-support).
- Scalable force-directed layout references using spatial trees/multilevel methods: [Yifan Hu’s SFDP overview](https://yifanhu.net/SOFTWARE/SFDP/index.html) and [graph-tool SFDP documentation](https://graph-tool.skewed.de/static/doc/autosummary/graph_tool.draw.sfdp_layout.html).

---

**Implementation authorization:** the user has approved structural implementation and directed the work to proceed without another plan-review gate. Remaining questions below are product decisions to resolve before their dependent release choices, not a block on approved structural work. Keep the established boundary: Bibliography has no Network destination; Visualization owns network visualization and analysis.
