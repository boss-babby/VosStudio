# VOSStudio Native for Windows

A native Windows port of VOSStudio v4. It maps bibliographic data and builds science maps, clusters, trends and publication figures. It's a single self-contained `VOSStudio.exe` (about 5.5 MB). There's no runtime, no DLLs to ship and no installer.

![showcase](docs/showcase.png)

## Quick start

1. Run `VOSStudio.exe`. Windows 10 or 11 is required, with any GPU that supports D3D11 feature level 10.0 or higher. If there's no usable GPU, it falls back to the WARP software renderer.
2. **Try the sample** on the welcome screen, or drop Web of Science / Scopus / RIS / BibTeX / OpenAlex JSON / VOSviewer files onto the window.
3. **Build** (left rail) → choose the type, unit, counting method and threshold → **Build map** (`Ctrl+B`).
4. Switch views with `1`–`7` and restyle in **Look**. Explore **Analyse / Trends / Actors**, then export from **Publish** as SVG / PDF / PNG.

Press `Ctrl+K` (or `F1`) at any time for the **command palette**. Every action, look and view is searchable there.

## What's new in 1.8.0: time, comparison and living maps

- **Difference map (Trends → Compare).** Compares two periods and colours every item of the map by how its share of documents changed: red items grew, blue faded (diverging coolwarm scale). Lists of growing, appearing and fading items with the ratio. One click shows the colours on the overlay view; "Restore colours" returns.
- **Main path analysis (Trends → Main path).** The knowledge backbone of the field: search path count (Batagelj 2003, Liu & Lu 2012), global main path plus key-route paths, with the papers in the chain listed year by year. Click a circle for the document preview.
- **Resolution sweep (Analyse → Stability).** Runs clustering at 9 resolutions with several seeds, shows clusters (bars) and agreement between runs (ARI line), and suggests the most reproducible resolution. Click any bar to apply it; export the numbers as CSV.
- **Geo density choices.** The Geo density layer now offers three styles (Heat, Cluster density, Shaded countries) and four weights (Documents, Citations, Citations per document, Collaboration) in a floating card under the layer switch.
- **Living maps (Data → Updates).** For data fetched with Search OpenAlex, re-runs the search from the last check, places every new paper onto the clusters of the map by the keywords/authors/sources/cited works it shares, shows per-cluster bars and emerging keywords, and offers *Add to Data* and *Add and Rebuild*. Toggle *Check when the project opens* to get the alert automatically.
- **Halo behind labels.** Label halos now render as a smooth, continuous filled outline in every export (PNG, PDF, SVG), matching the on-screen look; the previous lumpy offset-stroke effect is gone. On-screen rendering uses the same 24-step matte for consistency.
- The agent can now call `compare_periods`, add a `main_path`, `resolution_sweep` and `difference_map` chart to the autonomous literature report, and include them in the PDF.

![main path](docs/screenshots/main-path.png)
![difference map](docs/screenshots/difference-map.png)
![sweep](docs/screenshots/resolution-sweep.png)
![geo density options](docs/screenshots/geo-density-options.png)
![living map](docs/screenshots/living-map.png)
![halo export](docs/screenshots/halo.png)

## What's new in 1.7.1: literature reports by the agent

- **From a topic to a finished report.** Ask the agent, for example, "Write a literature review on large language
  models in education since 2021". You don't need to load any data first. The agent searches OpenAlex, reads the most
  cited and most recent papers (titles, abstracts, keywords), adds the charts that support the text, builds and names a
  keyword map, writes the sections (introduction, research themes, trends, gaps, conclusion) and saves **one PDF**. The
  PDF has the text and figures in order, numbered figure captions and a numbered reference list (APA style) of the
  papers cited in the text.
- The text cites only papers the agent has read. Citations are checked against the loaded records, and keys that
  don't match a record are dropped. The PDF notes that the text was written by an AI model and names the data source,
  the number of records and the years covered.
- **Reports are saved** in `Documents\VOSStudio\Reports`. **Open Report** and **Show in Folder** appear under the
  agent's answer and in the Agent section.
- **Ask First or Automatic** (Assistant → Agent → Changes). *Ask First* (the default) waits for your approval before
  each change, including the OpenAlex search, which replaces the loaded records. *Automatic* lets the agent work through
  the whole task on its own. **Undo Agent Changes** now also restores the records that a search replaced.
- New agent tools: `search_openalex`, `read_papers`, `add_chart` (17 charts, plus network, overlay, density, timeline
  and geographic map figures), `add_section`, `get_report`, `export_report`. A run can now use up to 30 tool calls.
- **Fixed: the Geo density layer lost its labels in Publish.** In a Geo figure with the density layer, "Only 0 labels
  fit" appeared and the figure had no labels. The density layer hides the circles, and the label placement skipped
  every item without a circle. Labels are now placed at the country positions, centred, with a light halo so they stay
  readable on the density colours.
- Script command `agentauto 0|1`; `agentlog` also prints the path of the last report.

![A report written by the agent](docs/screenshots/report-pages.png)

A sample report, produced by a scripted test run against live OpenAlex data, is in `docs/sample-report.pdf`.

## What's new in 1.7.0: the agent

- **An agent in the Assistant.** Switch the composer from **Ask** to **Agent** (or choose *Work with the agent*) and
  describe a goal, for example "map the author keywords of the last five years, check that the clusters are stable and
  name them". The agent plans the steps and works with the application's own tools. It can read the data set, the
  clusters, items, trends and most cited documents. It can also build maps, re-cluster, check cluster stability, scan
  for term variants, update the thesaurus, name clusters, and switch views and pages. Each step appears in the
  conversation as it happens, with a short reason, and the run ends with an answer and a list of what was done.
- **You stay in control.** Reading and changing the view happen at once. Anything that changes the map, the settings or
  the thesaurus waits for **Allow**, **Allow All Changes** (for the rest of the task) or **Decline**. **Undo Agent
  Changes** restores the map, the analysis and clustering settings and the thesaurus to their state before the run, in
  one step. A run stops after 14 tool calls (30 from 1.7.1), and **Stop** ends it at any time. The agent works with every provider in
  Settings (OpenAI, Gemini, Claude, or OpenAI-compatible and local models). Like the other tools, it sends summaries
  and tool results, never the full records.
- **Stable cluster stability.** Cluster stability compared re-runs made with the current method against the map's
  clusters even when those clusters came from other settings (for example, a map clustered before 1.6 with
  modularity). This made robust maps look unstable. The check now recognises the settings behind the map's clusters.
  When they differ, it compares the re-runs with a fresh clustering and says so. Re-runs use the full clustering
  pipeline, as a real build does.
- **Clustering method choice** (Build → Clustering): *As VOSviewer* (VOS quality, the default) or *Modularity (1.5 and
  earlier)* to reproduce maps made with older versions. Projects remember the method; the Methods paragraph names it.
- **AI Clean terms from the Assistant.** *Clean the terms* is now a research tool and a start tile. The proposals open
  in Data → Clean terms for review.
- Script commands for automation: `agent`, `agentwait`, `agentapprove [all|no]`, `agentundo`, `agentlog`, and the
  1.6 commands (see Automation).

## What's new in 1.6.0: trust and cleaning

- **Validated against VOSviewer.** Ten networks, from 19 to 914 items and up to 142,591 links, were run through
  VOSStudio and VOSviewer 1.6.21 with the same settings. Layouts are identical (same map up to rotation and scale) on 9
  of 10, and clusters are identical on all seven smaller networks. On the large networks both tools find partitions
  of similar quality with the same method; which one scores higher varies by network. The method, the full table and a
  reproducible harness are in `docs/VALIDATION.md` and `validation/`.
- **Clustering now optimises the same function as VOSviewer.** The validation showed that VOSStudio's Leiden clustering
  maximised modularity, while VOSviewer maximises the VOS clustering quality function (Waltman, van Eck & Noyons, 2010).
  The two often agree, but not always. Clustering now uses the VOS quality function; the LinLog/modularity
  normalisation still uses modularity, as in VOSviewer. Re-clustering an older project can give slightly different
  clusters. The Methods paragraph names the objective and cites the paper.
- **Clean terms** (Data): **Scan** finds spelling variants, plurals, hyphenation, acronyms and typos. **AI** also
  proposes synonyms, abbreviations and generic terms, marked with an AI tag and a short reason. Proposals for terms
  that are not in the data are dropped. Nothing changes until you choose **Merge selected**, and each merge can be
  undone. Merges go into the thesaurus, so they apply to every map and are saved with the project. The Assistant can
  start the same review ("Clean").
- **Records flow** (Data): a PRISMA-style diagram of how records move from import to the map: files and searches,
  duplicates removed, records excluded (by reason), records analysed, and items found, meeting the threshold and shown in
  the map. Expand it to full size, or export it as SVG for the methods section.
- **Crash reports and recovery.** If VOSStudio closes unexpectedly, it writes a readable report and a minidump to
  `%LOCALAPPDATA%\VOSStudio\crashes` and saves the open project to a recovery file. At the next start it offers
  **Recover**, **Show Report** or **Discard**. Nothing is sent anywhere.
- **Fix:** VOSviewer map and network files that start with a byte order mark (VOSviewer writes one) are read
  correctly. Before, the first column was not recognised and an extra empty item appeared.
- The records flow diagram scales with the panel width and stays readable when expanded.

**Signed installer:** not included. Signing requires a code-signing certificate issued to the publisher. The build
is ready for it (`signtool sign /fd sha256 /tr <timestamp-url> /td sha256 VOSStudio.exe`) once a certificate is
available.

## What's new in 1.5.2: all links on selection, curved links in 3D

- **Clicking a node shows all of its links.** "Max. lines" (Look → Links) still keeps the full map readable. When a node is selected, every one of its links is drawn, including those beyond the limit, so each highlighted neighbour has its line. A selected node's links also stay at least one pixel wide. Exports of the current view match the screen.
- **Curved links in 3D.** The 3D view follows the link shape setting (Look → Links: Straight, Curved or Arc) like the network view. Curves are computed in 3D space: seen from the front they are the same curves as the network view, and they rotate naturally with the camera. The 3D panel in Publish figures and 3D exports use the same curves.
- The inspector shows the average publication year without a thousands separator (2020.4, not 2,020).
- `docs/NEXT-LEVEL.md`: a critical review of the application and a roadmap, including how to turn the AI assistant into an agent.

## What's new in 1.5.1: fixes

- **Command palette:** typing in the search field works again (the field now receives keyboard focus when the palette opens). The same fix applies to the `/` shortcut for the top search box.
- **View switcher:** always centred in the window. Opening or closing a side panel no longer moves it or changes it between labels and icons; only the window width does.
- **Settings:** the dropdowns (AI provider, model list, and so on) now open on top of the dialog instead of closing it. Dialogs have their own layer, independent of menus and lists. `Esc` closes an open list first, then the dialog. The window buttons stay usable while a dialog is open.
- **Figure preview:** redesigned to match the app. The bar has the same colours as the main window, with the title and print size on the left, zoom controls (−, zoom level, +, Fit, Actual Size) in the centre, and SVG, PDF and PNG export plus Done on the right, clear of the window buttons. The figure sits on a neutral backdrop with a paper shadow. Keys: `0` fit, `1` actual size, `+` and `-` zoom, `Esc` close.
- **Publish panel:** the live preview is rendered once into an image and redrawn only when the figure changes, so scrolling stays smooth with every view added as a panel. Panning the full-screen preview reuses the same image. With all seven panels, the Publish page's own drawing cost per frame fell about 12× on the test machine.
- Geo panels in figures render cleanly in the preview. Stray horizontal lines could appear across the map on some systems; figure shapes are now tessellated at a finer internal scale, and sub-visible island specks and near-duplicate points are dropped, which also makes SVG and PDF files smaller.

## What's new in 1.5: a desktop-grade interface, semantic search and large data sets

**Interface.** The whole shell was redrawn in a clean, macOS-like style:
neutral greys, one accent colour, grouped settings lists instead of cards, and much less text.

- A new **start screen** with New Project, Open…, Open Sample and your recent projects.
- A segmented view switcher, a quiet sidebar, title-case headings, 28 px controls and text buttons without icons.
- The Data page is grouped into Import, Summary, Field coverage, Files and Search OpenAlex.
- The Build page uses one grouped list for the analysis type and flat tokens for the unit.
- Short empty states, such as "No Map" and "Build a map to see clusters and item statistics."
- Status bar and texts no longer mention graphics APIs, frame rates or other internals, and em-dashes are gone from the UI.
- Map controls follow the canvas brightness, so a white VOSviewer canvas inside the dark theme keeps readable buttons.
- New **Settings** window (`Ctrl+,`, the file menu or the palette) with the OpenAlex API key, contact e-mail and theme.

**Semantic OpenAlex search.** Data → Search OpenAlex → **Semantic** finds works by meaning rather than exact words.
It uses the OpenAlex embedding search (`search.semantic`) and adds two strategies you can choose:

| Strategy | What it does |
|---|---|
| Expand and re-rank | Takes the semantic hits as seeds, follows their topics and citations to collect a larger pool, then keeps the works whose title and abstract are closest to your description (TF-IDF cosine). *Keep* sets how strict the filter is. |
| Multi-query | Runs up to eight differently worded descriptions and merges the hits, ranked by how many queries found each work. |

Keyword search is unchanged and remains the default.
Semantic search works without a key on the small OpenAlex demo allowance. A free key (openalex.org/settings/api) raises that allowance; add it under Settings.
Requests are throttled to one per second, and 401/403/429 answers are reported in plain language.

**Exports.**
- **Transparent background** option for PNG, SVG and PDF figures and view exports.
- **Multi-page PDF.** In Publish, *PDF: one page per plot* writes each selected view on its own page instead of a grid.
  Every chart card also offers *All N charts, one per page (PDF)*.

**Large data sets.** 10,000 to 50,000 records load, build and save smoothly:

| 50,000 WoS records (2-core test machine) | 1.4 | 1.5 |
|---|---|---|
| Co-occurrence of keywords | out of memory | 11 s |
| Co-occurrence of terms | out of memory | 25 s |
| Bibliographic coupling of documents | out of memory | 18 s |
| Co-citation of references | out of memory | 21 s |
| Save / open project | out of memory | 2.5 s / 2.8 s |

This comes from a sparse pair-weight engine, two-pass extraction that runs in parallel, a memory guard for the densest analyses, streamed project files, cached corpus statistics, cached chart drawings, and a faster layout that runs several starts in parallel.

**One unified window.** The system title bar is gone: the app's own top bar is the title bar, in the app's light or dark theme.
Drag it to move the window, double-click it to maximise, and use the minimise, maximise and close buttons at the top right.
Snapping, Windows 11 snap layouts (hover the maximise button), the window shadow and rounded corners all keep working.
The project name and an *Edited* marker now sit at the left of the status bar.

**Geo layers.** The Geo view has a layer switch at the top right (also under Look → Geo view):

| Layer | Map of countries (co-authorship of countries) | Any other map (records per country) |
|---|---|---|
| Network / Countries | Cluster colours on nodes, links and countries | Documents per country, bubbles and collaboration arcs |
| Overlay | Overlay colours (e.g. average publication year) on nodes, links and countries, with a colour bar | Average publication year of each country's documents, 5th to 95th percentile |
| Density | A smooth density surface of the countries weighted by size; labels stay | A density surface of documents over the world |

Every layer is exported by *Export current view* (SVG, PDF, PNG) and by the Geo panel in Publish, including its colour key.
The density kernel follows Look → Density → kernel width.

**AI assistant.** A dedicated **Assistant** page (sparkle icon at the bottom of the sidebar, `Ctrl+J`) connects VOSStudio to an AI provider of your choice:

| Provider | Default model | Key |
|---|---|---|
| OpenAI | gpt-5-mini | platform.openai.com/api-keys |
| Google Gemini | gemini-3.8-flash | aistudio.google.com/apikey |
| Anthropic Claude | claude-haiku-4-5 | console.anthropic.com |
| OpenAI-compatible | your choice | OpenRouter, Groq, DeepSeek, Mistral, or a local Ollama / LM Studio server (no key needed) |

Set it up under Settings → AI Assistant: choose the provider, paste the key, optionally pick a model (*Load List* fetches the models your key can use) and press *Test Connection*.
Keys are encrypted with Windows (DPAPI) for your user account and are never written into project files.

The assistant sees a compact summary of your work: the data set (years, top sources, keywords, countries), the map (clusters with their top items, statistics), the most cited records, keyword trends and your current selection.
Chips above the message box choose what is shared, with an estimate of the size.
Answers stream in and are formatted (headings, lists, tables, code). Each answer can be copied, and the whole conversation exported as Markdown.
The conversation is stored in the project.

Ready-made tasks: research landscape, literature review draft, gaps and future directions, emerging topics, name the clusters, explain the selection, summarise a document, plan an OpenAlex search, figure caption and results, methods paragraph.

It is also built into the other pages:
- **Analyse → Clusters:** *Name Clusters with AI* writes short names and summaries onto the map (one undo step).
- **Inspector:** *Explain with AI* for the selected item.
- **Document preview:** *Summarise with AI*.
- **Data → Search OpenAlex:** *Plan the Search with AI…* turns a research question into keyword and semantic queries that you apply with one click.
- **Publish:** *Write Caption and Results with AI*. **Analyse → Methods:** *Rewrite with AI*.
- Every task is also in the command palette (`Ctrl+K`, type "AI").

**Map history is saved.** The maps in the history switcher at the top are now stored in the project file, including the selected one, and come back when you reopen the project.

## What's new in 1.4: VOSviewer look by default, less clumped layouts

**The VOSviewer look is now the default.** It follows the rendering rules of VOSviewer (taken from the VOSviewer Online source):

| | VOSviewer look (default) | Studio 1.3 (previous) |
|---|---|---|
| Circle size | constant on screen: radius = max(16·w^0.5, 5)/2 px, w = weight / mean weight | map units, grows with zoom |
| Zooming | spreads items apart and reveals more labels | enlarges nodes and labels |
| Nodes | flat, 75 % opacity, no outline | shaded spheres with white outline |
| Labels | centred on the circle, 9 + 4·w^0.5 px, regular weight, no halo; a label that would collide is hidden until you zoom in | beside the circle with a halo |
| Links | 1.5·s^0.5 px (s = strength / mean strength), 40 % opacity, gradient between the two endpoint colours lightened in Lab | source colour, width grows with zoom |
| Density | whole canvas coloured with VOSviewer's viridis; kernel = 0.125 × average item distance (van Eck & Waltman 2010), so any map size looks the same | fades into the background, fixed kernel |

- **Switching back**: *Look → Studio 1.3* restores the previous appearance in one click. Each part can also be mixed individually: *Nodes → Sizing* (VOSviewer / Studio), *Size variation*, *Labels → Placement* (Beside / Centre), halo, *Links → Width variation*, curvature, *Density → Fill* (Whole canvas / Around items), *Kernel* (Auto / Fixed) and colour map (`vos`, viridis, bgy, …).
- The Clean publication and Midnight looks also use VOSviewer sizing but keep their own colours.
- Figures and single-view exports use the same rules. Screen pixels map to points so that the smallest label equals the figure label size. Density labels switch between black and white depending on the colour underneath.
- Projects saved with 1.3 on the old default look open with the new look and keep their palette. Projects that used other looks keep their colours.

**Less clumped layouts.** We benchmarked the layout against VOSviewer's own coordinates for three reference maps from VOSviewer Online:
- With VOSviewer's default parameters, the optimiser reproduces VOSviewer's Economics map exactly (distance correlation 1.000).
- VOSviewer's larger reference maps were made with lower exponents: attraction 1 / repulsion 0 (Scientometrics terms, correlation 0.98) and 1 / −1 (Leiden Ranking).
- With the classic 2 / 1 exponents, a sparse fringe of weakly linked items stretches the map, so the cores are squeezed into tight clumps. That is the density seen in 1.3.

The changes:
- **New default: attraction 1, repulsion 0 ("Balanced").** Clusters separate clearly and the canvas fills evenly.
- *Build → Layout spread*: Balanced / Classic VOS (2, 1; best for small maps) / Even (1, −1) / Custom. The attraction and repulsion sliders stay available, and attraction is kept above repulsion.
- The optimiser now follows the CWTS gradient-descent schedule: sequential per-node moves in random order, and the step shrinks ×0.75 on failure and grows after 5 improvements. On the Scientometrics benchmark it gets closer to VOSviewer (correlation 0.94–0.95 vs 0.91). The result is centred, PCA-rotated and reflected like VOSviewer's.
- Overlap removal now uses the on-screen circle size at the fitted zoom, so dense cores are opened up as they appear by default.
- Projects saved with 1.3 keep their stored parameters, so their layouts stay reproducible.

**Fixes**
- Dropping a VOSviewer map together with its network file now loads the links. Network files, which contain only `id id strength` lines, used to be ignored.

## What's new in 1.3: every view can be exported

Until 1.2, the publication figure could only hold three panels (Network, Overlay, Density), and the per-view export was hidden in a download button on the canvas. That is why Geo, Timeline and 3D seemed impossible to export. In 1.3 **every view is exportable in two ways**:

1. **One print-ready figure that can combine any views.** Publish → *Views in this figure* now has 7 toggle chips: **Network, Overlay, Density, Timeline, Geo, 3D, Matrix**. Each selected view becomes a lettered panel (a, b, c…), sized in mm for the journal, as SVG, PDF or PNG.
   - **Geo** draws the world map with the Geo view's current mode: documents per country (with a log colour key), or countries as nodes placed on the map with collaboration arcs. It is disabled when the data has no countries.
   - **3D** renders the 3D layout from the 3D view's current camera angle, with perspective, depth fading and sphere shading.
   - **Timeline** places items by average publication year, with a year axis.
   - **Matrix** shows link strength between the top 40 items, ordered by cluster.
2. **Any single view exactly as shown on screen.** This now works for all 7 views, Matrix included. It is available from 4 places:
   - Publish → *Export one view, as shown*: a strip of the 7 view icons to switch views, then **‹View› SVG / PDF / PNG**;
   - the **download button** on the canvas, whose popup now names the view and links to the publication figure;
   - the **file menu**: *Export the ‹View› view…* and *Publication figure…*;
   - **Ctrl+Shift+E**, and the command palette (*Export current view…*, *Publication figure with all views*).

   File names include the view, e.g. `cooc-keywords-map-geo.svg`.

The chosen panels are saved in the project.

Screenshots: `docs/screenshots/v13-publish-all-views.png`, `v13-figure-all-views.png`, `v13-export-geo-view.png`, `v13-export-matrix-view.png`, `v13-file-menu-export.png`, `v13-canvas-export-popup.png`.

## What's new in 1.2

- **A working top-left toolbar.** The logo and name are gone because the window title already shows them (`project.vosproj • — VOSStudio`, where `•` means unsaved changes). That space now holds:
  - a **file menu**: open data, open project, new, save as, sample, and the 6 most recent projects;
  - **Save**, with a dot when there are unsaved changes;
  - **Undo / Redo**;
  - a **map switcher** showing the current map (e.g. *Co-occurrence · Author keywords · 48*).
- **Map history.** Every map you build or open in a session is kept, up to 8, with its layout, pins and cluster names. Open the map switcher and click one to go back to it instantly, with no rebuild. A badge shows how many maps you can switch between. Rebuilding the same map replaces its entry instead of adding a duplicate.
- **Inspector for Geo.** In the corpus-overview Geo map, clicking a country opens its profile:
  - documents, share of the corpus, citations, citations per document, h-index and international co-authorship share;
  - a documents-per-year chart;
  - collaborating countries (click one to jump to it);
  - top keywords (click one to find it on the map), organisations, authors and sources;
  - its documents, most cited first.
  
  On a map of countries, the selected country node gets a compact version of the same profile.
- **Document preview instead of launching the browser.** Clicking a paper anywhere (inspector, country profile, most-cited chart, historiograph) opens a preview in the inspector with:
  - title, authors, year, source, volume and pages;
  - global citations, local citations (cited by N documents in this data set) and document type;
  - the abstract (long ones collapse, with *Show the full abstract*);
  - author keywords and index terms as chips that find the term on the map;
  - affiliations and countries;
  - the map items that come from this paper;
  - the papers in your data set that it cites or that cite it, which open in turn (← goes back);
  - its reference list.
  
  Buttons: **Open in browser** (doi.org, or a Google Scholar title search when there's no DOI), **Copy citation** (APA), **Copy DOI**, **Copy title**. `Esc` steps back through the preview history.
- **New analyses.** Each one answers a question the existing charts couldn't:
  - **Trends › Topics:** trend topics as in Bibliometrix. Each frequent term shows its median year and its Q1–Q3 span, so emerging and fading topics are visible at a glance.
  - **Trends › RPYS:** reference publication year spectroscopy. Cited references are counted by publication year and compared with the 5-year median. Peak years are listed with their most cited work: the historical roots of the field.
  - **Actors › Documents:**
    - a citation overview: h, g, mean, median, uncited share and local links;
    - most-cited documents ranked by global citations, citations per year, or local citations;
    - the citation-class distribution;
    - a **historiograph** (HistCite style): direct citations among the most locally cited papers, placed by year.
  - **Actors › production over time:** for the top authors, sources, countries or organisations, circle size shows documents per year and colour shows citations per year. **Countries** also gets **SCP vs MCP**: single- vs multiple-country publications.
  - **Analyse › Clusters over time:** documents per year for each cluster, stacked, with a **Share** mode showing growing and shrinking themes.
- **Smooth, cheap animation.**
  - The left panel and the inspector slide open and closed over about 200 ms. Their content is laid out at full width and clipped, so nothing reflows and no chart is recomputed while they move. The map keeps its framing as the canvas resizes.
  - The full-screen figure grows out of its preview while the backdrop fades in.
  - Frames are drawn only while something moves; the app returns to 0 % idle straight afterwards.
  - Animations switch off automatically when Windows' *Show animations* setting is off.
- **Palette:** new entries *Trend topics*, *RPYS*, *Most cited documents & historiograph*, *Clusters over time*, *Switch map…* and *Preview the most cited document*.

Screenshots: `docs/screenshots/v12-*.png`.

## What's new in 1.1

- **Geo works from country names.** The app matches country names (and ISO codes, and common aliases such as "USA", "UK", "Korea, South" or "Türkiye") against a built-in Natural Earth world map. No latitude or longitude is needed, and nothing is downloaded. The Geo view has two modes:
  - **Map of countries:** when the map's items are countries (for example co-authorship of countries), the nodes sit on their countries. Each country is tinted by cluster or by weight over a world basemap.
  - **Corpus overview:** for any other map, the view shows a world choropleth of documents per country, with bubbles and the top 150 collaboration arcs. Hover or click a country for its counts. The **Map co-authorship of countries** button builds the country map in one click.
- **Exports look like the screen.** Figures and view exports keep the 3D sphere shading as real vector radial gradients: `<radialGradient>` in SVG and smooth shadings in PDF. They are not flat circles or embedded bitmaps. **Publish → Current view** (and the canvas download button) exports exactly what is on screen: the camera, basemap, links, labels and legend, as SVG, PDF or PNG. **Node shading** (Match view / Flat / Sphere) controls the journal figure.
- **"?" help instead of instruction text.** Every option has a small "?" that shows the explanation on hover. The long paragraphs are gone.
- **Hover values on every chart:** bar, line, Sankey, Bradford, Lotka, three-field and the threshold-sensitivity chart. The hovered item is outlined and its exact value shown. The pointer snaps to the nearest bar or point, so the gaps between bars work too.
- **Full-screen figure preview:** click the figure in Publish to zoom it to the whole window. Wheel-zoom at the cursor, drag to pan, `0` fits, `+`/`−` zoom, `Esc` closes. The header also has the SVG, PDF and PNG exports.
- **Hideable cluster legend:** Look → *Cluster legend*, the legend button on the canvas toolbar, or `G`. Hidden legends are also left out of view exports.
- **OpenAlex with several search terms.** Separate terms with `;` (or put one per line). Quote a term to search it as an exact phrase, for example `bibliometrics; science mapping; "citation analysis"`. Each term becomes a removable chip showing its hit count. **Match: Any term** runs one search per term and merges the results with duplicates removed. **All terms** asks for works that match every term (AND).
- **Record export:** the last OpenAlex fetch (or the whole corpus, from *Data & project*) can be saved as Web of Science plain text, RIS or Scopus-style CSV, which VOSviewer, Bibliometrix and CiteSpace all read. The raw OpenAlex JSON can be saved as well.
- **Build button first:** the Build page keeps **Build / Rebuild map** pinned at the top, above the analysis choices, so it never needs scrolling.
- **Low idle cost.** When nothing changes, the app sleeps in the message queue and draws nothing, using 0 % GPU and near-0 % CPU. The v1.0 app redrew twice a second. A focused text field wakes only for the caret blink, twice a second. While a job runs, progress updates at about 10 fps, which leaves the CPU to the job. While the window is minimised or fully covered, the app does not render at all. On minimising, it also trims transient GPU memory (`IDXGIDevice3::Trim`) and releases its working set.

Screenshots of these features are in `docs/screenshots/` (`geo-overview`, `geo-countries`, `chart-hover`, `figure-zoom`, `openalex-terms`, `build-pinned`). `docs/samples/` holds untouched current-view exports (SVG and PDF) for comparison with the screen.

## What runs where

| Part | Runs on |
|---|---|
| Nodes (shaded spheres or flat discs, rings, selection and search halos), links (straight, curved, bundled, gradient), density kernel and colormap, 3D view | **GPU**: Direct3D 11 instanced draws + HLSL, compiled at startup with D3DCompile |
| UI, labels, legends, charts, minimap, overlays | **GPU**: Direct2D device context on the same swap chain, text by DirectWrite |
| PNG export and clipboard copy | Direct2D offscreen bitmap → WIC |
| SVG / PDF export | Vector writers in `src/core/figure.cpp` (resolution-independent, 1 unit = 1 pt) |
| Parsing, VOS layout, Leiden clustering, FDEB bundling, statistics | **CPU** worker threads (the UI never blocks; long jobs show progress and can be cancelled) |

The algorithms are graph and optimisation work that doesn't map well onto shaders, so they run on CPU threads. Everything you *see* is drawn by the GPU.

## Feature parity with v4

**Import and cleaning**
- Formats: WoS plain text and tab-delimited, Scopus CSV, RIS, BibTeX, OpenAlex JSON, VOSviewer map/network files, and `.vosproj` projects.
- You can merge several files; duplicates are removed by DOI/title.
- OpenAlex fetch by one or more search terms (`;`-separated, Any/All match) or a DOI list, with year range, max count and an append option. The fetched records can be exported as WoS, RIS, CSV or raw JSON.
- Field-coverage report, variant/duplicate-term finder, and a thesaurus editor with import/export in VOSviewer format.

**Analysis builder** (left panel, VOSviewer workflow)
- Co-occurrence: all / author / index keywords, and title & abstract terms.
- Co-authorship: authors, organisations, countries.
- Citation, bibliographic coupling and co-citation: documents, sources, authors, organisations, countries, cited references / sources / authors.
- Full or fractional counting, and thresholds with a live "N of M meet the threshold" count.
- Advanced settings: normalisation, VOS layout attraction/repulsion, Leiden resolution, and an item exclusion list.
- A threshold-sensitivity chart shows how items and clusters change with the threshold.

**Views**
- Network, Overlay (colour by score: average publication year, citations and so on), Density (item or cluster density), Timeline, Matrix, Geo (world map from country names: country nodes on a basemap, or a corpus choropleth with collaboration arcs) and 3D (orbit).

**Look**
- Four one-click looks: Studio, VOSviewer classic, Clean publication and Midnight, plus a light/dark UI theme.
- Palettes, including CVD-safe options: Okabe–Ito, Tol and VOSStudio.
- Colormaps and a colour-vision-deficiency preview.
- Size controls: size by, size, range, opacity, border, and flat or shaded nodes.
- Labels: size, weight, placement, halo, colour by cluster, max labels.
- Links: width, opacity, curvature, colour mode.
- Cluster hulls, cluster names and edge bundling.

**Analyse**
- Clusters: strategic diagram (Callon), auto-naming, re-cluster at a chosen resolution, and rename/recolour.
- Items: a sortable table with degree, strength, betweenness, participation and role, plus CSV export.
- Network summary and a cluster-stability check over repeated runs.

**Trends**
- Growth, Kleinberg bursts, thematic evolution (Sankey), period comparison, and three-field plots.

**Actors**
- Authors, sources, countries and organisations, plus Bradford's and Lotka's laws.

**Publish**
- Journal size presets, multi-panel figures, and label/line minimum checks with warnings.
- SVG, PDF, PNG at any dpi with sphere or flat nodes, a full-screen zoomable preview, clipboard copy, WYSIWYG current-view SVG/PDF/PNG, and the methods text.
- VOSviewer map/network export, and self-contained `.vosproj` bundles holding records, map, look and figure settings.

Every chart card has **Expand** and **SVG / PDF / PNG** export buttons.

## UX improvements over v4

- **Command palette** (`Ctrl+K`): fuzzy search over about 30 commands, every look and every view.
- **Undo/redo** (`Ctrl+Z` / `Ctrl+Y`) for style, layout, exclusions and pins.
- **Non-blocking jobs:** builds, bundling, OpenAlex and stability run in the background with a progress bar and a Cancel button. The previous map stays interactive until the new one is ready (two-phase build).
- **Hover card:** links, strength, occurrences, citations and average year without clicking.
- **Inspector:** focus, pin, copy and exclude-and-rebuild, strongest links, and centrality metrics.
- **Minimap, animated camera, box select** (`Shift`-drag on the canvas), node dragging with pinning, and search that cycles through hits with `Tab`.
- **Stable framing:** opening or closing panels or resizing the window keeps the same view of the map.
- **Per-monitor DPI v2**, crisp at 100–300 %. Drag & drop, a recent-projects list, and settings remembered in `%APPDATA%\VOSStudio`.
- The top bar switches to icon-only view tabs when the window is narrow.

## Keyboard

| Key | Action |
|---|---|
| `Ctrl+K` / `F1` | Command palette |
| `Ctrl+O`, `Ctrl+Shift+O` | Open data / open project |
| `Ctrl+S`, `Ctrl+Shift+S`, `Ctrl+N` | Save / save as / new project |
| `Ctrl+B`, `Ctrl+L` | Build map / re-run layout |
| `Ctrl+Z`, `Ctrl+Y` | Undo / redo |
| `Ctrl+J` | AI assistant |
| `Ctrl+,` | Settings |
| `Ctrl+Shift+C` | Copy figure to clipboard (PNG) |
| `1`–`7` | Network · Overlay · Density · Timeline · Matrix · Geo · 3D |
| `F`, `+`, `-` | Fit / zoom in / zoom out |
| `L`, `H`, `N`, `G` | Labels / cluster hulls / cluster names / cluster legend |
| `M`, `I` | Minimap / inspector |
| `/`, `Tab` | Focus search / next search hit |
| `0`, `1`, `+`, `-` (figure preview) | Fit / actual size / zoom in / zoom out |
| `Esc` | Close figure preview, palette or chart → clear Geo selection → clear selection → clear search |

Mouse:
- Wheel zooms at the cursor, and dragging empty space pans. `Shift`-drag box-selects.
- Dragging a node moves and pins it, and double-click focuses it.
- In 3D, dragging orbits.

## Building

**MSYS2 (recommended):** install [MSYS2](https://www.msys2.org), open the **UCRT64** shell, then:
```
pacman -S --needed mingw-w64-ucrt-x86_64-gcc make
make -j
```
or run `build.bat` from a shell where `g++` is on `PATH`.

**CMake** (MinGW; MSVC 2019+ should work but has not been tested):
```
cmake -B build -G "MinGW Makefiles"   # or: -G "Visual Studio 17 2022"
cmake --build build --config Release
```

**Cross-compile from Linux:** `make -j CROSS=x86_64-w64-mingw32-`

**Core tests** (portable, any C++17 compiler): `make test`

A full build uses about 1 GB RAM per compiler job. On small machines, use `make -j2`.

## Source layout

```
src/core/   portable C++17, no Windows headers: fully testable on any OS
  records   WoS / Scopus / RIS / BibTeX / OpenAlex parsers, dedupe, VOSviewer I/O, samples
  analysis  the 5 analysis types × 12 units, counting, thresholds, thesaurus
  algo      association strength, VOS layout, Leiden, FDEB bundling, centralities
  stats     growth, Bradford, Lotka, bursts, evolution, compare, three-field, stability
  style     looks, palettes, colormaps, CVD simulation, visual encoder
  figure    publication scene → SVG / PDF / PNG
  charts    every panel chart as a vector scene (same SVG/PDF/PNG path)
  insights  trend topics, RPYS, production over time, SCP/MCP, local citations, historiograph data, APA
  ai        AI providers (OpenAI, Gemini, Anthropic, compatible): requests, streaming parser, context builder, tasks
  semantic  TF-IDF re-ranking for semantic OpenAlex search
  project   pipeline, undo snapshots, .vosproj bundles (incl. figure settings), methods text
  world     Natural Earth 110m countries (embedded), name/ISO/alias matching, Equal Earth projection
  records   (also) WoS / RIS / CSV record writers
src/win/    Windows layer
  gfx       D3D11 device + flip-model swap chain, D2D/DirectWrite interop, WIC readback
  netview   GPU renderer for the map (HLSL instancing, density, 3D) + label placement
  ui        immediate-mode Direct2D widget toolkit (DPI-aware, themed, icons as paths)
  app       window, shell, canvas interaction, commands, jobs, undo
  pages     the seven left-panel pages, chart hover, full-screen figure preview, help texts
  insight   top-left toolbar, map history, window title, panel animation, document preview, country profile
  geo       Geo view: basemap, layers (network, overlay, density), bubbles, arcs, hover/selection, vector export
  assistant AI assistant page, streaming requests, Settings window
tests/      core, project, figure, charts, geo, insights, export, semantic, compat and ai tests
res/        icon, manifest (PerMonitorV2, UTF-8 code page), version info
```

## Automation

`VOSStudio.exe --script tour.txt` runs a scripted session, which is useful for regression screenshots. Each line is one command:
- setup and build: `sample`, `open <file>`, `openproj <file>`, `type cooc|coauth|citation|coupling|cocit`, `unit <id>`, `min <n>`, `build`, `bundle`;
- views and pages: `view <name>`, `page <name>`, `tab <n>`;
- looks and display: `look <id>`, `theme light|dark`, `hulls on`, `names on`;
- search and selection: `search <text>`, `select <text>`;
- output: `shot <png>`, `svg|pdf|png <file>`, `viewsvg|viewpdf|viewpng <file>` (current view), `records wos|ris|csv <file>`, `save <file>`;
- 1.1 additions: `legend on|off`, `geofill 0|1|2`, `geosel <country>|none`, `oaquery <terms>`, `figzoom on|off`, `hover <x> <y>`;
- 1.2 additions: `preview <record>|top|-1`, `maps <k>` (switch to history entry k), `popup filemenu|maphist`, `docrank 0|1|2`, `cymode 0|1`, `scroll <y>` / `scroll insp <y>`;
- 1.5 additions: `figopt <key> <value>`, `chartspdf <file>`, `oakind 0|1`, `oasem 0|1`, `oasemq <q1|q2>`, `geolayer network|overlay|density`, `settings` / `settings 0`, `settab 0|1|2`, `aiconfig <provider 0-3> <model> <base url> [key]`, `ai <task> [text]`, `aiinput <text>`, `aiwait`;
- 1.6 additions: `cleanunit <n>`, `cleanscan`, `cleanai`, `cleanpick <i> [0|1]`, `cleanlabel <i> <label>`, `cleanmerge`, `cleanundo`, `cleanlog`, `expandflow`, `flowsvg <file>`, `crashtest`;
- 1.7 additions: `agent <goal>`, `agentwait` (until the agent finishes or asks for approval), `agentapprove [all|no]`, `agentundo`, `agentlog`;
- 1.8 additions: `diffmap [a0 a1 b0 b1]`, `diffclear`, `mainpathroutes <n>`, `sweep` (run resolution sweep), `useres <r>`, `oafrom <year>`, `oamax <n>`, `oafetch`, `livingcheck`, `livingadd rebuild`, `livingauto 0|1`, `livingsince <yyyy-mm-dd>`;
- control: `wait <frames>`, `log <text>`, `quit`.

Any other arguments on the command line are opened as files.

## Notes and known differences

- **Testing:** the build was tested under Wine 10 (D3D11 → wined3d, Direct2D/DirectWrite → Wine) with a software GPU. Every page, view and export was exercised, but it has **not yet been run on real Windows hardware**. Please report anything that looks off. Real Direct2D and DirectWrite should render text slightly crisper than the screenshots.
- **PDF fonts:** PDF export uses the base-14 Helvetica font with WinAnsi encoding, so the file needs no embedded fonts. Characters outside the WinAnsi (Latin-1) set are replaced. For non-Latin labels, use SVG, which keeps full Unicode.
- **Sphere shading in exports** uses vector gradients that every modern SVG and PDF viewer, Illustrator and Inkscape render. Some journals ask for flat artwork; choose *Node shading → Flat* in Publish for those.
- **Geo coverage:** the embedded map covers 216 countries and territories, with aliases. 176 are Natural Earth 1:110m outlines; the other 40 are small states such as Singapore, Malta or Bahrain. Those are matched and placed at a point, but they have no outline to fill at that scale.
- **Chart hover:** the Matrix view has no hover values yet; every other chart does.
- **Differences from the web version:**
  - The native sample generator produces slightly different counts: 48 items / 478 links / Q 0.526 here versus 408 links / Q 0.586 in the web v4.
  - The native burst detector defaults to γ = 0.5, which finds more bursts in small corpora; set it to 1.0 to match the web version.
