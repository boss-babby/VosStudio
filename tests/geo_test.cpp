#include <cstdio>
#include "../src/core/world.h"
#include "../src/core/records.h"
using namespace vs;
int main() {
  const char* cases[][2] = {{"USA", "United States"}, {"CA 94305 USA", "United States"}, {"Peoples R China", "China"}, {"England", "United Kingdom"},
    {"BD", "Bangladesh"}, {"FR", "France"}, {"NO", "Norway"}, {"Dhaka 1000, Bangladesh", "Bangladesh"}, {"Turkiye", "Turkey"}, {"Viet Nam", "Vietnam"},
    {"Korea", "South Korea"}, {"Russian Federation", "Russia"}, {"U Arab Emirates", "United Arab Emirates"}, {"Brasil", "Brazil"}, {"Côte d'Ivoire", "Ivory Coast"},
    {"Singapore", "Singapore"}, {"HK", "Hong Kong"}, {"Czechia", "Czech Republic"}, {"New Delhi 110016, India", "India"}, {"DEU", "Germany"},
    {"Taiwan", "Taiwan"}, {"Trinid & Tobago", "Trinidad and Tobago"}, {"Dem Rep Congo", "DR Congo"}, {"Iran, Islamic Republic of", "Iran"},
    {"Stanford University, Stanford, United States", "United States"}, {"Mexico", "Mexico"}, {"Georgia", "Georgia"}, {"Unknownland", ""}};
  int fails = 0;
  for (auto& c : cases) {
    string got = canonicalCountry(c[0]);
    if (got != c[1]) { printf("FAIL %s -> '%s' (want '%s')\n", c[0], got.c_str(), c[1]); fails++; }
  }
  double lat, lon;
  if (!countryLatLon("Bangladesh", lat, lon) || std::fabs(lat - 24.2) > 1) { printf("FAIL latlon\n"); fails++; }
  double x, y; equalEarth(180, 0, x, y); printf("EE(180,0)=%.3f,%.3f  countries=%d\n", x, y, worldCountryCount());
  printf("fails=%d\n", fails);
  return fails;
}
