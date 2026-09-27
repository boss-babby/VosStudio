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

// duplicate removal: DOI first, then normalised title + year. Returns number removed.
int deduplicate(vector<Record>& recs);

string normCountry(const string& c);
vector<std::pair<string, string>> countryCoords();  // name → "lat,lon" lookup (Geo view)
bool countryLatLon(const string& c, double& lat, double& lon);

// sample corpora (deterministic) — returned as export text, so they go through the real parsers
string sampleWoSExport();
string sampleScopusExport();

// record export (e.g. OpenAlex fetches): WoS tagged text (VOSviewer / bibliometrix / CiteSpace), RIS, Scopus-style CSV
enum class RecordExport { WoS, RIS, CSV };
string writeRecords(const vector<Record>& recs, RecordExport fmt);
string wosAuthor(const string& name);  // "Boris A. Petrov" -> "Petrov, BA"; "Petrov B" -> "Petrov, B"

vector<vector<string>> parseCsv(const string& text, char sep = ',');

}  // namespace vs
