# Bibliography upgrade: actionable implementation plan

**Status:** Phases 0–2 and the chosen import/export round-trip work in Phase 4 are implemented in the current working tree: stable record IDs, cautious legacy-key migration, metadata editing, duplicate review/keep-both/field-choice merging, and ID-aware RIS/BibTeX/CSV round trips. Core tests pass. The Windows UI has not been compiled or manually exercised in this environment; unified-reader workflows, performance, and manual acceptance remain open.

**Product boundary:** Bibliography is a reference-management and writing workspace. It has no Network destination. Network visualization and analysis remain in Visualization. “Zotero-like” describes the day-to-day organization and reading workflow only; it does not promise Zotero database, API, or interface compatibility.

This companion plan is focused on Bibliography. Product-wide requirements—visual-first Writer, Tectonic-only preview compilation and equation editing, explicit hybrid SVG/PDF export, target-aware pointer redraws, and performance across 10k–20k+ items and plot types—remain tracked in [`UPGRADE-IMPLEMENTATION-PLAN.md`](UPGRADE-IMPLEMENTATION-PLAN.md); this document does not replace or relax them.

## 1. Current-state audit

The application already keeps the corpus, attached PDFs, PDF reading state, annotations, and Writer document inside the shared project. `Project` persists the reading library through its optional `library` JSON field. Papers is a searchable/sortable table of `Corpus::recs`; Review is primarily a queue over `PdfLibrary::items`. Therefore records without PDFs and attached PDFs without a linked record are not the same row type today.

Before this slice:

- The corpus table could search and export records, route a row to a PDF, and initiate PDF acquisition/attachment.
- The Review queue could filter attached PDFs by status and edit PDF-item notes and tags.
- `PdfItem.tags` belonged to an attached PDF. There was no persisted collection or saved-view model, and a record without a PDF had no collection/tag organization path.
- Reader open/close routing did not preserve whether a paper originated in Papers or Review as a distinct route.
- Existing library parsing ignored unrecognized/missing organization fields, making the `library` object a backward-compatible place to add optional metadata.

**Identity upgrade:** corpus rows now have a durable local `Record.id`, independent of metadata edits; IDs are serialized in projects and preserved by RIS, BibTeX, and CSV exports/imports. Document references persist `RefEntry.recordId`; existing documents are migrated by exact ID or an unambiguous DOI/title/year match. PDF links, collections, tags, and fetch outcomes migrate from legacy reference keys only when a key identifies one record. Ambiguous legacy keys are retained unchanged, not guessed. Duplicate detection uses DOI or normalized title plus year—not IDs—and never merges during import.

## 2. Target information architecture

Keep the current two-workspace shell and one shared project:

- **Visualization:** map building, network exploration, analysis, trends, and figure work.
- **Bibliography:** Papers, Review, and Write. Do not add Network/Map as a Bibliography destination.
- **Persistent library context:** a Library sidebar remains beside the Papers table and the open Reader. It is the home for library-wide views, collections, tags, and saved views. The main area remains context-sensitive: table, Reader, Writer, or Bibliography home.
- **Record and asset distinction:** corpus records are references; PDFs are attached assets. A record can exist without a PDF, and a PDF can remain unlinked until matched. Phase 1 filters records by linked-PDF state without deleting or hiding the underlying record from the project.

### Core browsing concepts

1. **All papers** — all corpus records, subject to the ordinary text query.
2. **Needs PDF / Has PDF** — based on whether at least one attached PDF is linked to the record. Missing files remain attached records and are not silently treated as deleted.
3. **Reading status** — To read, Reading, Read, or Excluded. If multiple PDFs link to one record, a status view currently includes the record when any linked PDF has that status. Making conflicting per-file statuses explicit and settling the ownership rule remains part of the unified-library acceptance work.
4. **Collections** — user-created groups of stable `Record.id` values. Adding/removing a record changes membership only; it does not move, copy, detach, or delete a PDF.
5. **Tags** — record-level tags are usable with or without an attached PDF. The Library sidebar can also filter annotation codes, labeled separately as “Tags and codes”; the Papers bulk tag editor changes organization tags only. Annotation codes remain annotation metadata and must not be silently erased or conflated.
6. **Saved views** — a named snapshot of text query, status, PDF-presence filter, collection, and tag. They are project data, not app-global settings.

## 3. Data, persistence, and migration

### Implemented Phase 1 schema

The existing `library` object now optionally stores:

- `collections`: stable collection IDs/names and member `Record.id` values.
- `recordTags`: shared tags keyed by stable `Record.id`.
- `savedViews`: ID/name and the query, status, PDF-presence, collection, and tag filters.

Legacy DOI/title-year reference keys migrate only when they identify one record; ambiguous membership/tag keys are left unchanged. The in-memory collection keeps a membership index for responsive lookup; JSON stores the ordered stable-ID list. Collection/view ID counters are recovered from stored IDs. Organization-only projects are recognized as saveable even when no PDF has been attached.

### Backward compatibility and tag policy

- Old projects without the optional fields load with empty collections/views/tags and retain every existing item, note, annotation, status, and fetch result.
- Existing per-PDF tags remain on their PDFs. When a linked PDF is edited as a paper, the visible tag set is the union of shared record tags and legacy tags from linked PDF items. An explicit edit promotes that set to the shared record tag set and synchronizes linked PDF copies. Linking a previously tagged PDF merges its tags into the record-level set.
- Deleting a collection removes only its membership container. It is blocked while a saved view refers to it; users can delete the dependent saved view first. Deleting a saved view never deletes papers.
- Optional fields need no destructive project migration or new storage location. Keep the current `library` JSON object and preserve the existing project-file save path.

### Phase 2 identity migration — implemented

- `Record.id` is durable, survives DOI/title/year corrections, is preserved in project JSON, and is made unique within a project without merging colliding imports. If an imported local ID is already in use, the import path assigns a fresh local ID and reports that reassignment.
- Writer references persist `RefEntry.recordId`. On document load, legacy references are linked by an existing stable ID or by a unique DOI/title/year match; a stale non-empty ID is never redirected by metadata alone.
- Existing PDF/library keys migrate to local IDs only when the legacy key maps to one record. Ambiguous mappings remain intact and unresolved rather than being guessed.
- The metadata editor covers first-class fields and arbitrary extra metadata. DOI/author/title changes retain the record ID and refresh its linked Writer reference.
- Duplicate candidates are grouped by bibliographic fields. Import keeps all records. Review offers **Keep both** or an explicit merge; merge presents conflicting scalar values for field-by-field selection, combines list metadata, keeps the chosen record ID, remaps document/PDF/library links, and records the removed ID in merged-record metadata.

## 4. Interaction workflows

### Browse, organize, and find

1. Open Papers from the Bibliography route. Keep the Library sidebar visible while the corpus table is open.
2. Use All papers, Needs PDF, Review attached PDFs, status controls, text query, tag, or collection to scope the table. Show the active scope in the sidebar and keep it after opening/closing a PDF.
3. Select one or more visible rows (or focus one row). The folder action toggles membership in a chosen collection. The tag action adds/removes a tag across the selected records; it also supports records that have no PDF.
4. Create a collection in the sidebar. The collection row filters the table and attached-PDF queue. Delete a collection only after any saved views that depend on it have been removed.
5. Save the current query/status/PDF/collection/tag combination as a named view. Reopening it restores those filters. Duplicate/empty names are rejected with a visible message.
6. Attach, fetch, or open PDFs from the existing Paper/PDF actions. Review continues to expose unlinked PDFs so a reference can be matched later.

### Read while retaining context

- Opening an attached PDF keeps Library navigation available beside the Reader.
- Collection/tag/query/status filters also scope the visible attached-PDF queue; applying a Library filter while reading does not close the Reader.
- Closing a Reader opened from Papers returns to the Papers route; one opened from Review returns to Review. Switching workspaces remembers the active Reader item and returns to it as before.
- Reader status and tag edits invalidate cached table filters and persist through the project library. Notes and annotations remain owned by their attached PDF.

### Import/export and round-trip policy

The existing file-open/import path supports Web of Science tagged text (plain and tab-delimited), Scopus CSV, RIS, BibTeX, and OpenAlex JSON. The Papers table exports RIS, BibTeX, CSV, and Web of Science tagged text; reading notes and coding data retain their Markdown/CSV exports. RIS, BibTeX, and CSV carry a VOSStudio local record ID plus source key, URL, issue, author IDs, index terms, and arbitrary extra fields; they also retain the format-specific core metadata listed below. Web of Science export retains its tagged-text format unchanged and is not used as the stable-ID round-trip format. Duplicate candidates are reported from bibliographic fields, never merged on import, and explicitly reviewed later. No Zotero database/API interoperability is claimed.

### Import/export field contract

| Format | Import mapping and unrecognized data | Export round-trip contract |
|---|---|---|
| **RIS** | Reads DOI, URL, issue, `SP`/`EP`, abstract, keywords, references, citation note, source key, local ID, and unknown tags. | Writes local ID in `ID`, source key in `AN`, issue/pages/URL in `IS`/`SP`/`UR`; VOSStudio extension tags retain arbitrary fields, author IDs, and index terms. |
| **BibTeX** | Reads common BibTeX fields, VOSStudio identity/extra-field fields, and unknown fields. | Writes local ID/source key/issue/pages/URL plus a versioned base64 JSON payload for arbitrary fields and author IDs; `keywords-plus`, `references`, publisher, language, abstract, and citations map to their corresponding fields. |
| **Scopus CSV** | Reads standard Scopus columns, optional local ID/author-ID/extra-field columns, and preserves unrecognized column names and values in `Record.extra`. | The CSV export retains the Scopus-style base columns and adds local record ID, author IDs, issue, URL, and a JSON extra-field column. The extra-field object is carried as JSON inside a correctly quoted CSV cell. |
| **Web of Science plain text** | Maps standard tags including issue, URL, source key, author identifiers, and retains other unrecognized tags. | **Legacy export is unchanged:** no VOSStudio local ID or new custom payload is added. It is not an ID/URL/arbitrary-field round-trip format; this behavior has a byte-for-byte compatibility fixture. |
| **Web of Science tab-delimited** | Maps known columns including optional VOSStudio local ID; unknown column names and values are retained in `Record.extra`. | No new tab-delimited exporter is introduced. |
| **OpenAlex JSON** | The OpenAlex work ID remains the source key; a local ID is generated. Issue/page range/URL and author IDs are mapped; unknown top-level properties are retained in `Record.extra` as JSON text. | Import-only in the current UI. |

Parser normalization (for example DOI normalization and `bibClean` whitespace/trailing-punctuation cleanup) remains intentional. Project JSON is the lossless local representation; RIS/BibTeX/CSV provide explicit VOSStudio extensions for the fields above. External applications may ignore those custom fields, so their interoperability is not claimed. Automated fixtures exercise every supported import parser, the three ID-aware export/import round trips, and the unchanged Web of Science export contract.

## 5. Phased implementation and gates

| Phase | Scope | Status | Exit gate |
|---|---|---|---|
| **0 — Audit and compatibility boundary** | Confirm shared-project ownership, separate record/PDF surfaces, current route behavior, existing exports, and reference-key limitation. | **Done** | Findings recorded here; no Network destination introduced. |
| **1 — Library organization slice** | Persist collections, shared record tags, and saved views; expose status/PDF/collection/tag/query filters in the Library sidebar; add selected-row collection/tag actions; retain the sidebar in Papers and Reader; preserve reader return route; save organization-only projects. | **Implemented; UI validation pending** | Core round trips pass. On Windows, verify create/add/remove/filter/save/reopen/delete, no-PDF tagging, Reader context, narrow/high-DPI layout, and no unintended data changes. |
| **2 — Durable record identity and metadata** | Stable IDs and conservative migration; first-class/extra metadata editing; duplicate review, keep-both, and field-choice merge workflows. | **Implemented; Windows UI validation pending** | Core migration, round-trip, and merge tests pass; ambiguous mappings are not guessed. Verify projects and UI workflows on Windows. |
| **3 — Unified library operations** | Join records, PDFs, collections, tags, statuses, reading notes, saved views, and Writer citations through stable IDs; finish asset-link repair and bulk workflows. | **Partially implemented; acceptance pending** | Ensure records without PDFs and unlinked PDFs remain manageable; resolve multi-PDF status ownership; manual tests, undo/preview, and performance checks remain. |
| **4 — Import/export execution** | Keep the existing import formats (WoS, Scopus, RIS, BibTeX, OpenAlex JSON) and export formats (RIS, BibTeX, CSV, WoS tagged text); preserve IDs in RIS/BibTeX/CSV and unknown metadata where supported. | **Implemented for existing formats; round-trip tests added** | Fixture tests cover stable IDs, issue/URL, source key, author IDs, index terms, references, citation count, and arbitrary metadata in ID-aware formats; verify real third-party consumers. WoS export remains compatible and does not promise local-ID round trips. No Zotero compatibility claim. |
| **5 — Reliability, accessibility, and performance** | Windows UI/DPI/keyboard tests, screen-reader labels where supported, large-library profiling, autosave/backup behavior, and project migration regression suite. | **Planned** | Measured p50/p95 browsing/filter/edit latency on representative libraries; no silent record/PDF/annotation loss; filters never weaken exact row selection. |

## 6. Tests and acceptance criteria

### Automated checks already added

- `corpus_test`: distinct IDs for duplicate records; bibliographic duplicate groups remain present after import; per-file provenance; project round trip of URL, issue, arbitrary metadata, and review state; explicit field-choice merge behavior.
- `export_test`: full field round trips for stable/source IDs, title/authors/author IDs, DOI/URL/issue/pages, publisher/language/abstract, keywords/index terms/references/citation count, and extra fields through RIS, BibTeX, and CSV; import fixtures for WoS plain/tab, Scopus, and OpenAlex; byte-compatible WoS export fixture.
- `doc_test`: legacy document-reference migration by record metadata, persistent `recordId`, and citation-link remapping after an explicit merge.
- `library_test`: stable record-key use, unambiguous legacy-key migration for PDFs/collections/tags/fetch statuses, and refusal to choose a duplicate DOI without title disambiguation.
- `project_test`: organization-only project persistence; migration of a pre-ID project with an item, collection, and record tags; conservative preservation of a legacy key that maps to duplicate DOI records; map/project open-save coverage.

### Remaining automated additions

- Whole-project legacy fixtures for malformed/duplicate collection IDs, stale keys, and saved views referencing deleted collections; the pre-ID item/collection/tag migration and ambiguous-key cases are now covered.
- Filter semantics for multiple linked PDFs, missing files, unlinked PDFs, tag/collection combinations, and scoped selection/export.
- Reader/table route-transition tests and project open/save/open regression tests.
- UI-level tests for metadata editing, review navigation, keep-both, and merge choices once Windows UI test infrastructure is available.

### Manual Windows acceptance for the implemented UI

1. Open an existing project with corpus records and no collections; confirm it loads unchanged and can be saved.
2. Create a collection; select/focus multiple records; add them; apply the collection; remove them; confirm the records and PDFs are not deleted. Import or inspect same-DOI/title-year records; confirm they keep separate IDs and remain independently organizable. For legacy library keys that match multiple records, confirm the load path does not guess or reassign the ambiguous key.
3. Add/remove tags on a record with no PDF and one with a linked PDF; confirm the tag filters both the table and queue and the PDF/annotation notes remain intact.
4. Save a view combining query, status, PDF presence, collection, and tag; close/reopen the project; confirm the view restores.
5. Attempt to delete a collection used by a saved view; confirm it is blocked without altering either object. Delete the view, then delete the collection; confirm papers remain.
6. Confirm Review still lists attached PDFs with no corpus link; open a linked PDF from Papers and PDFs from Review; use the sidebar while reading; close/switch workspace; confirm the intended route, item, and scroll/filter context return.
7. Exercise 100k-row corpus virtualization and 10k attached-item stress data; record browsing/filter latency rather than claiming performance without measurement.
8. Check minimum supported window width, 100/150/200% DPI, keyboard navigation, focus, tooltips, and popup clipping.

## 7. Deferred scope and guardrails

- Record-ID migration, first-class/extra metadata editing, and duplicate review/merge are implemented. Collection hierarchy/rename, advanced boolean/field-query syntax, and saved sort/column configuration remain deferred.
- New external formats beyond the current import/export set and Zotero interoperability remain out of scope until explicitly planned and tested. Do not copy Zotero’s interface/theme or claim its database/API compatibility.
- Cross-device/cloud sync, shared/group libraries, browser connector, citation-style catalog expansion, and network visualization inside Bibliography are not part of this slice.
- Do not delete records, PDFs, annotations, notes, or unsupported metadata as a side effect of collection/tag actions or migration. Preserve exact selection semantics and make conflicting linked-PDF statuses visible in the later unified-library phase.
- Windows app validation, GPU measurements, and the broader 10k–20k+ plot-performance acceptance are still outstanding; this Bibliography work does not report them as measured.
