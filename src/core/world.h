// VOSStudio Native — world basemap, country matching and map projection
#pragma once
#include <cstdint>
#include "common.h"

namespace vs {

struct WorldCountry {
  const char* name;       // canonical English short name (what the app shows and stores)
  const char* iso2;
  const char* iso3;
  float labelLon, labelLat;
  int ring0, rings;       // polygon rings in worldRing(); rings == 0 for point-only countries
  const char* continent;
  const char* aliases;    // '|'-separated alternative names
};
struct WorldRing { int start, count, hole; };

int worldCountryCount();
const WorldCountry& worldCountry(int i);
const WorldRing& worldRing(int i);
const int16_t* worldPts();   // lon*100, lat*100 pairs

// Resolve any country spelling to a country index (-1 if unknown). Accepts ISO-2/ISO-3 codes,
// Natural Earth names, WoS/Scopus/OpenAlex spellings ("Peoples R China", "USA", "England", "Turkiye"...)
// and free text ending in a country ("Stanford, CA 94305 USA", "Dhaka 1000, Bangladesh").
int findCountry(const string& text);
// Canonical country name for any spelling; empty when not recognised.
string canonicalCountry(const string& text);
bool countryLonLat(const string& text, double& lon, double& lat);

// Equal Earth projection (Šavrič, Patterson & Jenny 2018). lon/lat in degrees; x in about [-2.71, 2.71], y in [-1.32, 1.32], y up.
void equalEarth(double lon, double lat, double& x, double& y);

}  // namespace vs
