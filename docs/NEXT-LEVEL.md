# VOSStudio: critical review and plan for the next level

*Written for version 1.5.2 (September 2026).*

---

## 1. Where the application stands

### What is genuinely strong
- **Rendering and figures.** GPU canvas, seven views, WYSIWYG SVG/PDF/PNG, journal presets, multi-panel figures and legends that match the screen. Few bibliometric tools come close on figure quality.
- **One tool for the whole workflow.** Import (WoS, Scopus, OpenAlex, VOSviewer files), analysis types that match VOSviewer, CWTS layout and Leiden clustering, analyses of trends, documents and actors, AI, and publishing.
- **Scale and speed.** 50k records build in 11 to 25 s on a 2-core machine, and 10k-paper maps stay interactive on a real GPU.
- **Methods transparency.** Seeds, a methods paragraph, cluster stability and map history.

### Where it is weak (the honest list)

| Weakness | Why it matters |
|---|---|
| **Windows only.** | A large share of academics use macOS. This caps adoption more than any missing feature. |
| **No external validation.** | Reviewers ask "does it give the same result as VOSviewer or bibliometrix?". We compare against our own references (`pt_old`), not a published benchmark. Credibility in peer review depends on this. **Addressed in 1.6.0 for VOSviewer:** see `docs/VALIDATION.md`. |
| **Data cleaning is thin.** | Merging synonyms ("AI" / "artificial intelligence"), singular/plural forms and author name variants is the most time-consuming real task in bibliometrics. The tool that makes it painless wins users. |
| **AI is a sidebar, not a co-worker.** | The assistant writes good text from a summary of the map, but it cannot *do* anything: search, filter, merge, rebuild, export. Its claims are not linked to the records they come from. |
| **Only tested under emulation.** | Most visual verification ran under Wine with a software renderer. Real GPU drivers, high-DPI multi-monitor setups and IME input need testing on real machines. |
| **Nothing to share.** | Maps can't be shared as an interactive link (VOSviewer Online can). There is no collaboration and no comment layer. |
| **Custom UI toolkit.** | Fast and good-looking, but no screen-reader support, limited keyboard navigation and no high-contrast mode. It is also a maintenance burden that grows with every widget. |
| **The competition is moving.** | VOSviewer 1.6.21 (June 2026) added OpenAlex API keys, full-text search and keyword maps from OpenAlex. That closes part of our data-source lead. AI literature tools (Elicit, Undermind, Litmaps/ResearchRabbit) own "find and read papers". Our space is **analyse, explain and publish**, and we should own it. |

---

## 2. Product and UX improvements (high value, moderate effort)

1. **Guided project flow.** A slim stepper: *Question → Data → Clean → Map → Interpret → Publish*. Each step shows what is done and what is recommended. This makes the app usable by first-time bibliometricians without adding text to the UI.
2. **Compare mode.**
   - Two periods, two corpora, or two thresholds side by side, or as a difference map (appearing, growing and fading items).
   - It answers the most common reviewer question ("how did the field change?") in one screen.
3. **Linked selection everywhere.** Selecting a cluster filters the charts, the document list, the Geo view and the timeline at once, and vice versa. Most of the data already exists; this is wiring.
4. **Interactive sharing.**
   - Export a single self-contained HTML file with the map, search, hover and cluster list.
   - It opens in any browser and can go into supplementary material.
5. **Undo for everything and named snapshots.** Map history exists; extend it to data filters and cleaning steps, so a whole analysis can be replayed.
6. **Accessibility and internationalisation.**
   - UI Automation support.
   - Full keyboard navigation and a high-contrast theme.
   - Check that CJK and Cyrillic names render in the SVG and PDF exports (font embedding).
7. **Real-hardware quality loop.**
   - Crash minidumps.
   - Opt-in anonymous diagnostics (GPU model, frame time).
   - A signed installer with auto-update. An unsigned exe triggers SmartScreen warnings, which scare users away.

---

## 3. Analytical features worth adding

Chosen because researchers cite them in papers, not because they look impressive.

> **Correction (1.6.0).** The first version of this table listed several features as new that VOSStudio already had. The *Status* column now says what exists. Rule for future proposals: check the code first.

| Feature | What it gives | Status |
|---|---|---|
| **Thesaurus editor** | Clean, merged terms and authors; VOSviewer-compatible thesaurus files | Existed (Data → Clean terms, Thesaurus). **1.6.0** adds the AI cleaning agent, editable preferred labels, generic-term removal and undo |
| **Thematic map** (centrality × density quadrants) | The "motor, niche, basic, emerging themes" plot | Existed: Analyse → Strategic diagram |
| **Thematic evolution** (alluvial between periods) | How clusters split and merge over time | Existed: Trends → Evolution |
| **Burst detection** (Kleinberg) | CiteSpace's best-known output | Existed: Trends → Bursts |
| **Lotka's and Bradford's laws**, normalised citations | Standard descriptive bibliometrics | Existed: Actors → Laws; "Norm. citations" overlay score. Percentile indicators are still missing |
| **Cluster stability** | Agreement of clusterings across seeds (ARI, NMI) | Existed: Analyse → Stability. A sweep across resolutions is still missing |
| **PRISMA-style flow diagram** | Required by many journals for literature studies | **New in 1.6.0**: Data → Records flow |
| **Main path analysis** (SPC) on citation networks | The knowledge backbone of a field | Missing |
| **Semantic map** (embedding-based document map) | Clusters by meaning, not shared words | Missing |

---|---|---|
| **Thesaurus editor** (AI-assisted, see 4.2) | Clean, merged terms and authors; VOSviewer-compatible thesaurus files | M |
| **Thematic map** (centrality × density quadrants) | The "motor, niche, basic, emerging themes" plot from bibliometrix, widely used | S |
| **Thematic evolution** (alluvial between periods) | How clusters split and merge over time | M |
| **Burst detection** (Kleinberg) with a burst-strength table | CiteSpace's best-known output; complements trend topics | S |
| **Main path analysis** (SPC) on citation networks | The knowledge backbone of a field; pairs with the historiograph | M |
| **Semantic map** (embedding-based document map) | Clusters by meaning, not shared words; works where keywords are poor | M |
| **Field-normalised impact** (percentiles, normalised citations), Lotka's and Bradford's laws | Standard descriptive bibliometrics that reviewers expect | S |
| **Resolution sweep and robustness report** | Clustering stability across resolutions and seeds; defends the map in review | S |
| **PRISMA-style flow diagram** of the data selection | Required by many journals for literature studies | S |

---

## 4. AI: from assistant to agent

### 4.1 The architecture (the most important decision)

**Let the app compute and the model decide.** The language model should never compute a number or invent a reference. It should call **tools** that the app already implements, then explain the results.

- **Tool layer.** The command palette's `commands` list and the script commands (`build`, `type`, `unit`, `min`, `view`, `select`, `figpanels`, `svg`, and so on) are 80 % of a tool API already. Expose them to the model as typed functions (JSON schema) through provider function calling. OpenAI, Gemini and Claude all support it.
- **Plan → approve → act → verify.**
  1. The agent shows a short plan ("search OpenAlex for …, keep 2015 to 2026, merge 14 synonym pairs, build co-occurrence of author keywords with min. 5, name clusters").
  2. The user approves or edits it.
  3. Each step runs with undo.
  4. The agent checks the result (for example, modularity too low → suggests a different threshold).
- **Grounding.** Every factual sentence the AI writes carries record IDs, shown as clickable chips that open the in-app document preview. References are only ever taken from the corpus, never generated. Unsupported claims are flagged.
- **Reproducibility.** Store the model, date, prompts and outputs in the project, and add a line to the methods paragraph: *"Cluster labels were suggested by gpt-5-mini (27 Sep 2026) and reviewed by the authors."*
- **Cost control.** Cache replies by content hash in the project. Show the token or cost estimate before long runs. Use a small model for bulk work (screening, merge proposals) and a strong model for synthesis.
- **Privacy option.** Ollama and other local models already work through the OpenAI-compatible provider. Add a small local embedding model for fully offline semantic features.

### 4.2 Agent features, in order of value

1. **Data-cleaning agent** (highest value, easiest to trust).
   - Proposes merges: synonyms, acronyms, spelling variants, singular/plural, and author name variants (using OpenAlex author IDs and ORCID when available).
   - Output is a review table (merge, keep separate, rename) with a reason and the counts affected.
   - Accepted rows become a thesaurus that can be saved and reused.
2. **Question-to-map agent.**
   - From a research question it designs the search (keyword and semantic, as today), fetches and deduplicates the records, and screens them for relevance.
   - It then chooses suitable analyses (for example, co-word plus co-authorship of countries plus a trend view), builds them, names the clusters, and drafts an overview.
   - Every step stays visible and editable.
3. **Screening assistant for systematic reviews.**
   - Title/abstract screening against written inclusion and exclusion criteria, with confidence scores and reasons.
   - A disagreement queue when the AI is the second reviewer.
   - PRISMA counts are filled in automatically.
4. **Grounded interpretation.**
   - For each cluster the app selects representative documents (central, highly cited, recent). The model reads their abstracts and writes a label and summary with citations.
   - "Why are these two clusters linked?" is answered from the bridging documents (high betweenness), not from general knowledge.
5. **Ask the map.** Questions like *"Which countries collaborate most on this topic since 2020?"* are answered by tool calls and returned as a chart plus numbers from the app, with one explanatory paragraph.
6. **Natural-language commands.** *"Show only the ethics cluster after 2019 and export it single-column for Nature"* becomes a sequence of existing commands, previewed before it runs.
7. **Writing studio.**
   - Results and methods sections tied to the actual figures, with in-text citations from the corpus.
   - Exported to DOCX, BibTeX or RIS.
   - Each paragraph links back to the evidence that supports it.
8. **Living maps (monitoring).**
   - Saved searches re-run on a schedule.
   - New papers are placed on the existing map ("7 new papers landed in cluster 3, 2 form a new topic").
   - Emerging-topic alerts.
9. **Figure critic.** Checks label overlap, smallest font size in points, colour-vision safety and journal size limits, then suggests fixes. It can extend the warnings the Publish panel already shows.
10. **MCP server.** Exposing VOSStudio's tools over the Model Context Protocol lets external agents (Claude Desktop, ChatGPT, IDE agents) drive the app. This is cheap once the tool layer exists, and is a differentiator: most literature tools have no MCP server.

### 4.3 Risks to design against
- **Hallucinated or misattributed findings.** Mitigated by grounding: record-ID chips and references from the corpus only.
- **Silent changes to the analysis.** Mitigated by plan approval, undo, and the AI steps being written into the map history.
- **Over-trust in AI screening.** Always keep a human decision and show agreement statistics.
- **Cost surprises.** Mitigated by estimates, budgets and caching.

---

## 5. Engineering foundations

1. **Cross-platform strategy.** The core (`src/core`) is already portable C++ and passes its tests on Linux. The Windows layer is the Direct2D renderer, DirectWrite text and the Win32 shell. Options:
   - **(a)** Put the UI renderer behind an interface and add a Metal or Skia backend for macOS. This keeps the look; effort is L.
   - **(b)** Ship a browser build of the viewer and publishing parts only (it doubles as the shareable HTML map); effort is M.

   Recommendation: (b) first for reach, then (a).
2. **Command-line and batch mode.** `vosstudio --recipe analysis.json` for reproducible pipelines and CI. It reuses the script engine.
3. **External validation suite.**
   - Build the same public datasets in VOSviewer and bibliometrix and compare items, links, clusters (adjusted Rand index) and layout (Procrustes).
   - Publish the numbers in the docs, and consider a short software paper (for example, *SoftwareX* or *Scientometrics*).
4. **Tests on real Windows machines.** CI on Windows runners with a GPU, and a small set of reference screenshots compared per build.

---

## 6. Suggested roadmap

| Release | Focus | Contents |
|---|---|---|
| **1.6: Trust and cleaning** | Credibility | Done in 1.6.0: AI cleaning agent, label editing and undo in the cleaning studio, PRISMA records flow, crash reports with recovery, validation against VOSviewer 1.6.21 (`docs/VALIDATION.md`), which led to switching clustering to the VOS quality function and fixing a byte-order-mark import bug. Still open: comparison of descriptive tables with Bibliometrix; signed installer (needs a code-signing certificate). Thematic map and bursts already existed |
| **1.7: The agent** | AI as a co-worker | **Done in 1.7.0:** agent with 19 application tools (read, view, change), approval for every change, one-step undo of a run, live step log, stop and step limit; cluster stability fix and clustering method choice. **1.7.1:** autonomous literature reports (OpenAlex search, paper reading, charts, map figure, cited sections, one PDF with numbered references), Ask First / Automatic. Still open: ask-the-map; natural-language commands; cost display and caching; MCP server |
| **1.8: Time and comparison** | Science of science | **Done in 1.8.0:** difference map (period-by-period change overlay), main path analysis (SPC, global path + key routes), resolution-sweep report with CSV export, Geo density styles and weight choice, living-map checks with new-paper placement onto clusters and per-project auto alerts, smooth label halos in every export. Still open: difference-map mini-map on the canvas, email/desktop alerts, and a living-map HTML share. |
| **2.0: Reach** | Adoption | Shareable HTML maps and a browser viewer; macOS renderer; writing studio with DOCX export; accessibility pass |

**What not to do:** more views or visual effects. Seven views is enough. The next gains come from **trust** (validation, grounding), **less manual work** (cleaning, agent) and **reach** (sharing, macOS).
