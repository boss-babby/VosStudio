// VOSStudio Native — bibliographic import (WoS, Scopus, RIS, BibTeX, OpenAlex, VOSviewer)
#pragma once
#include "json.h"
#include "model.h"

namespace vs {

BibFormat detectFormat(const string& text, const string& fileName);
// Parses records from a file's text. Returns number of records added.
int parseRecords(const string& text, BibFormat fmt, vector<Record>& out, string* err = nullptr);
int parseOpenAlex(const Json& j, vector<Record>& out);

// VOSviewer map + network files → network (either may be empty)
bool parseVosviewer(const string& mapText, const string& netText, Network& net, string* err = nullptr);
string writeVosMap(const Network& net);
string writeVosNetwork(const Network& net);

// Local record identity is independent of DOI/title, and remains unchanged when metadata is corrected.
string stableRecordId(const Record& r, uint64_t ordinal = 0);
size_t ensureRecordIds(vector<Record>& recs, const std::unordered_set<string>& reserved = {});  // assigns missing IDs and repairs collisions without merging

struct RecordDuplicateGroup { vector<int> indices; bool hasDoiMatch = false; };
vector<RecordDuplicateGroup> recordDuplicateGroups(const vector<Record>& recs);  // DOI or normalized title + year; never merges

struct RecordFieldConflict { string field, keepValue, mergeValue; };
vector<RecordFieldConflict> recordMergeConflicts(const Record& keep, const Record& merge);
Record mergeRecords(const Record& keep, const Record& merge, const std::unordered_map<string, bool>& useMerge, vector<RecordFieldConflict>* conflicts = nullptr);  // caller must show/resolve conflicts first

string normCountry(const string& c);
vector<std::pair<string, string>> countryCoords();  // name → "lat,lon" lookup (Geo view)
bool countryLatLon(const string& c, double& lat, double& lon);

// sample corpora (deterministic) — returned as export text, so they go through the real parsers
string sampleWoSExport();
string sampleScopusExport();

// record export (e.g. OpenAlex fetches): WoS tagged text (VOSviewer / bibliometrix / CiteSpace), RIS, Scopus-style CSV
enum class RecordExport { WoS, RIS, CSV, BibTeX };
string writeRecords(const vector<Record>& recs, RecordExport fmt);
string wosAuthor(const string& name);  // "Boris A. Petrov" -> "Petrov, BA"; "Petrov B" -> "Petrov, B"

vector<vector<string>> parseCsv(const string& text, char sep = ',');

}  // namespace vs
