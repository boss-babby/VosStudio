// VOSStudio Native — world basemap, country matching and map projection
#include "world.h"

#include <mutex>
#include <unordered_map>

#include "worlddata.inc"

namespace vs {
namespace {

// Small states that are not in the 1:110m polygons: point locations only
const WorldCountry EXTRA[] = {
    {"Singapore", "SG", "SGP", 103.82f, 1.35f, 0, 0, "Asia", "Republic of Singapore"},
    {"Hong Kong", "HK", "HKG", 114.17f, 22.30f, 0, 0, "Asia", "Hong Kong SAR|Hong Kong SAR China|Hong Kong, China"},
    {"Macao", "MO", "MAC", 113.55f, 22.19f, 0, 0, "Asia", "Macau|Macao SAR"},
    {"Malta", "MT", "MLT", 14.40f, 35.90f, 0, 0, "Europe", "Republic of Malta"},
    {"Bahrain", "BH", "BHR", 50.55f, 26.07f, 0, 0, "Asia", "Kingdom of Bahrain"},
    {"Mauritius", "MU", "MUS", 57.55f, -20.30f, 0, 0, "Africa", ""},
    {"Maldives", "MV", "MDV", 73.22f, 3.20f, 0, 0, "Asia", ""},
    {"Andorra", "AD", "AND", 1.52f, 42.51f, 0, 0, "Europe", ""},
    {"Monaco", "MC", "MCO", 7.42f, 43.74f, 0, 0, "Europe", ""},
    {"Liechtenstein", "LI", "LIE", 9.55f, 47.16f, 0, 0, "Europe", ""},
    {"San Marino", "SM", "SMR", 12.46f, 43.94f, 0, 0, "Europe", ""},
    {"Barbados", "BB", "BRB", -59.54f, 13.19f, 0, 0, "North America", ""},
    {"Seychelles", "SC", "SYC", 55.49f, -4.68f, 0, 0, "Africa", ""},
    {"Cape Verde", "CV", "CPV", -23.60f, 15.10f, 0, 0, "Africa", "Cabo Verde"},
    {"Comoros", "KM", "COM", 43.87f, -11.88f, 0, 0, "Africa", ""},
    {"Samoa", "WS", "WSM", -172.10f, -13.76f, 0, 0, "Oceania", ""},
    {"Tonga", "TO", "TON", -175.20f, -21.18f, 0, 0, "Oceania", ""},
    {"Grenada", "GD", "GRD", -61.68f, 12.12f, 0, 0, "North America", ""},
    {"Saint Lucia", "LC", "LCA", -60.98f, 13.91f, 0, 0, "North America", "St Lucia"},
    {"Antigua and Barbuda", "AG", "ATG", -61.80f, 17.06f, 0, 0, "North America", "Antigua & Barbu"},
    {"Guadeloupe", "GP", "GLP", -61.55f, 16.25f, 0, 0, "North America", ""},
    {"Martinique", "MQ", "MTQ", -61.02f, 14.64f, 0, 0, "North America", ""},
    {"Reunion", "RE", "REU", 55.53f, -21.12f, 0, 0, "Africa", "Réunion"},
    {"Mayotte", "YT", "MYT", 45.17f, -12.83f, 0, 0, "Africa", ""},
    {"French Guiana", "GF", "GUF", -53.13f, 3.93f, 0, 0, "South America", ""},
    {"Guam", "GU", "GUM", 144.79f, 13.44f, 0, 0, "Oceania", ""},
    {"Bermuda", "BM", "BMU", -64.75f, 32.31f, 0, 0, "North America", ""},
    {"Faroe Islands", "FO", "FRO", -6.91f, 61.89f, 0, 0, "Europe", "Faroe Isl"},
    {"Monserrat", "MS", "MSR", -62.19f, 16.74f, 0, 0, "North America", "Montserrat"},
    {"Palau", "PW", "PLW", 134.58f, 7.51f, 0, 0, "Oceania", ""},
    {"Kiribati", "KI", "KIR", 173.0f, 1.87f, 0, 0, "Oceania", ""},
    {"Micronesia", "FM", "FSM", 158.21f, 6.92f, 0, 0, "Oceania", "Federated States of Micronesia|Micronesia, Federated States of"},
    {"Marshall Islands", "MH", "MHL", 171.18f, 7.13f, 0, 0, "Oceania", "Marshall Island"},
    {"Tuvalu", "TV", "TUV", 179.2f, -8.52f, 0, 0, "Oceania", ""},
    {"Nauru", "NR", "NRU", 166.93f, -0.52f, 0, 0, "Oceania", ""},
    {"Sao Tome and Principe", "ST", "STP", 6.61f, 0.19f, 0, 0, "Africa", "Sao Tome & Prin"},
    {"Saint Kitts and Nevis", "KN", "KNA", -62.78f, 17.36f, 0, 0, "North America", "St Kitts & Nevi"},
    {"Saint Vincent and the Grenadines", "VC", "VCT", -61.29f, 12.98f, 0, 0, "North America", "St Vincent"},
    {"Dominica", "DM", "DMA", -61.37f, 15.41f, 0, 0, "North America", ""},
    {"Vatican City", "VA", "VAT", 12.45f, 41.90f, 0, 0, "Europe", "Holy See|Vatican"},
};
const int N_EXTRA = int(sizeof(EXTRA) / sizeof(EXTRA[0]));

// Bibliographic-database spellings (lower-case, folded) -> canonical name
const char* const ALIASES[][2] = {
    {"usa", "United States"}, {"u.s.a", "United States"}, {"u.s.a.", "United States"}, {"u.s.", "United States"}, {"united states of america", "United States"},
    {"america", "United States"}, {"peoples r china", "China"}, {"people's republic of china", "China"}, {"peoples republic of china", "China"}, {"pr china", "China"},
    {"p.r. china", "China"}, {"p. r. china", "China"}, {"mainland china", "China"}, {"england", "United Kingdom"}, {"scotland", "United Kingdom"}, {"wales", "United Kingdom"},
    {"north ireland", "United Kingdom"}, {"northern ireland", "United Kingdom"}, {"uk", "United Kingdom"}, {"u.k.", "United Kingdom"}, {"u.k", "United Kingdom"},
    {"great britain", "United Kingdom"}, {"britain", "United Kingdom"}, {"korea", "South Korea"}, {"republic of korea", "South Korea"}, {"korea, republic of", "South Korea"},
    {"korea (south)", "South Korea"}, {"s korea", "South Korea"}, {"south korea", "South Korea"}, {"korea, south", "South Korea"}, {"north korea", "North Korea"},
    {"korea, democratic people's republic of", "North Korea"}, {"dem people's rep korea", "North Korea"}, {"russian federation", "Russia"}, {"ussr", "Russia"},
    {"viet nam", "Vietnam"}, {"turkiye", "Turkey"}, {"czechia", "Czech Republic"}, {"iran, islamic republic of", "Iran"}, {"islamic republic of iran", "Iran"},
    {"u arab emirates", "United Arab Emirates"}, {"uae", "United Arab Emirates"}, {"the netherlands", "Netherlands"}, {"holland", "Netherlands"},
    {"taiwan, province of china", "Taiwan"}, {"republic of china", "Taiwan"}, {"taiwan roc", "Taiwan"}, {"cote d'ivoire", "Ivory Coast"}, {"cote divoire", "Ivory Coast"},
    {"bosnia & herceg", "Bosnia and Herzegovina"}, {"bosnia and herzegovina", "Bosnia and Herzegovina"}, {"bosnia", "Bosnia and Herzegovina"},
    {"trinid & tobago", "Trinidad and Tobago"}, {"trinidad tobago", "Trinidad and Tobago"}, {"papua n guinea", "Papua New Guinea"},
    {"dem rep congo", "DR Congo"}, {"democratic republic of congo", "DR Congo"}, {"congo, the democratic republic of the", "DR Congo"}, {"zaire", "DR Congo"},
    {"rep congo", "Congo"}, {"republic of congo", "Congo"}, {"congo republic", "Congo"}, {"cent afr republ", "Central African Republic"},
    {"byelarus", "Belarus"}, {"bělarus", "Belarus"}, {"kyrgyz republic", "Kyrgyzstan"}, {"brunei darussalam", "Brunei"}, {"fed rep ger", "Germany"},
    {"west germany", "Germany"}, {"deutschland", "Germany"}, {"espana", "Spain"}, {"italia", "Italy"}, {"brasil", "Brazil"}, {"mexique", "Mexico"},
    {"swaziland", "Eswatini"}, {"burma", "Myanmar"}, {"macedonia", "North Macedonia"}, {"republic of north macedonia", "North Macedonia"},
    {"lao people's democratic republic", "Laos"}, {"laos", "Laos"}, {"syrian arab republic", "Syria"}, {"libyan arab jamahiriya", "Libya"},
    {"tanzania, united republic of", "Tanzania"}, {"united republic of tanzania", "Tanzania"}, {"moldova, republic of", "Moldova"}, {"republic of moldova", "Moldova"},
    {"venezuela, bolivarian republic of", "Venezuela"}, {"bolivia, plurinational state of", "Bolivia"}, {"palestine, state of", "Palestine"},
    {"state of palestine", "Palestine"}, {"west bank", "Palestine"}, {"gaza", "Palestine"}, {"serbia monteneg", "Serbia"}, {"serbia and montenegro", "Serbia"},
    {"yugoslavia", "Serbia"}, {"timor leste", "Timor-Leste"}, {"east timor", "Timor-Leste"}, {"the gambia", "Gambia"}, {"gambia", "Gambia"},
    {"guinea bissau", "Guinea-Bissau"}, {"sultanate of oman", "Oman"}, {"kingdom of saudi arabia", "Saudi Arabia"}, {"ksa", "Saudi Arabia"},
    {"new caledonia", "New Caledonia"}, {"fr polynesia", "French Polynesia"}, {"falkland islands", "Falkland Islands"}, {"greenland", "Greenland"},
    {"puerto rico", "Puerto Rico"}, {"cabo verde", "Cape Verde"}, {"sao tome", "Sao Tome and Principe"}, {"solomon islands", "Solomon Islands"},
};

string fold(const string& s) {
  string f = lower(asciiFold(trim(s)));
  // collapse punctuation used inconsistently by databases
  string o;
  for (char c : f) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    if (c == ' ' && !o.empty() && o.back() == ' ') continue;
    o += c;
  }
  while (!o.empty() && (o.back() == '.' || o.back() == ' ' || o.back() == ';')) o.pop_back();
  return trim(o);
}

struct Index {
  std::unordered_map<string, int> byName;  // folded name -> index
  std::unordered_map<string, int> byCode;  // ISO2 / ISO3 upper -> index
};

const Index& index() {
  static Index ix;
  static std::once_flag once;
  std::call_once(once, [] {
    int n = worldCountryCount();
    for (int i = 0; i < n; i++) ix.byName.emplace(fold(worldCountry(i).name), i);  // canonical names win
    for (int i = 0; i < n; i++) {
      const WorldCountry& c = worldCountry(i);
      for (auto& a : split(c.aliases, '|')) if (!trim(a).empty()) ix.byName.emplace(fold(a), i);
      if (c.iso2[0]) ix.byCode.emplace(c.iso2, i);
      if (c.iso3[0]) ix.byCode.emplace(c.iso3, i);
    }
    for (auto& a : ALIASES) {
      string target = fold(a[1]);
      auto it = ix.byName.find(target);
      if (it != ix.byName.end()) ix.byName[fold(a[0])] = it->second;
    }
  });
  return ix;
}

int lookupName(const string& folded) {
  const Index& ix = index();
  auto it = ix.byName.find(folded);
  return it == ix.byName.end() ? -1 : it->second;
}

}  // namespace

int worldCountryCount() { return worlddata::N_COUNTRIES + N_EXTRA; }
const WorldCountry& worldCountry(int i) { return i < worlddata::N_COUNTRIES ? worlddata::COUNTRIES[i] : EXTRA[i - worlddata::N_COUNTRIES]; }
const WorldRing& worldRing(int i) { return worlddata::RINGS[i]; }
const int16_t* worldPts() { return worlddata::PTS; }

int findCountry(const string& text) {
  string t = trim(text);
  if (t.empty()) return -1;
  // exact ISO codes (as written by OpenAlex, or "USA")
  if (t.size() <= 3) {
    string up;
    bool alpha = true;
    for (char c : t) { if (!std::isalpha(static_cast<unsigned char>(c))) alpha = false; up += char(std::toupper(static_cast<unsigned char>(c))); }
    if (alpha) {
      auto it = index().byCode.find(up);
      if (it != index().byCode.end() && (t.size() == 3 || std::isupper(static_cast<unsigned char>(t[0])))) return it->second;
    }
  }
  string f = fold(t);
  int k = lookupName(f);
  if (k >= 0) return k;
  // last comma/semicolon separated part
  auto parts = splitAny(f, ",;");
  if (parts.size() > 1) {
    string last = trim(parts.back());
    k = lookupName(last);
    if (k >= 0) return k;
    f = last;
  }
  // trailing words: "stanford ca 94305 usa", "dhaka 1000 bangladesh", "new delhi 110016 india"
  auto words = splitAny(f, " ");
  for (int n = std::min<int>(5, int(words.size())); n >= 1; n--) {
    string tail;
    for (size_t w = words.size() - size_t(n); w < words.size(); w++) tail += (tail.empty() ? "" : " ") + words[w];
    k = lookupName(tail);
    if (k >= 0) return k;
  }
  // trailing upper-case ISO-3 code in the original ("... 94305 USA")
  auto ow = splitAny(t, " ,;");
  if (!ow.empty() && ow.back().size() == 3) {
    auto it = index().byCode.find(ow.back());
    if (it != index().byCode.end()) return it->second;
  }
  return -1;
}

string canonicalCountry(const string& text) {
  int k = findCountry(text);
  return k < 0 ? string() : string(worldCountry(k).name);
}

bool countryLonLat(const string& text, double& lon, double& lat) {
  int k = findCountry(text);
  if (k < 0) return false;
  lon = worldCountry(k).labelLon;
  lat = worldCountry(k).labelLat;
  return true;
}

void equalEarth(double lon, double lat, double& x, double& y) {
  const double A1 = 1.340264, A2 = -0.081106, A3 = 0.000893, A4 = 0.003796;
  const double M = std::sqrt(3.0) / 2.0;
  double lam = lon * M_PI / 180.0, phi = clampv(lat, -89.9, 89.9) * M_PI / 180.0;
  double th = std::asin(M * std::sin(phi));
  double t2 = th * th, t6 = t2 * t2 * t2;
  x = lam * std::cos(th) / (M * (A1 + 3 * A2 * t2 + t6 * (7 * A3 + 9 * A4 * t2)));
  y = th * (A1 + A2 * t2 + t6 * (A3 + A4 * t2));
}

}  // namespace vs
