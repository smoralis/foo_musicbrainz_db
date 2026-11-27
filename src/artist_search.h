#pragma once
// Search over the artist list: text folding (case and Latin diacritics), a country name table,
// and a small query language. Header-only; used by the database (to build the stored search
// text) and by the panel (to filter the table).
//
// Query syntax (terms are separated by spaces and must ALL match):
//   words           name, sort name, aliases, disambiguation, area or MBID contain the text
//                   (case and accents are ignored: "shiro" finds "Shirō")
//   "two words"     the same, for a phrase
//   ES              exactly two UPPERCASE letters: the artist's country code (nothing else)
//   spain  españa   a full country name (English or native): artists from that country
//   name:text       name, sort name or aliases only
//   country:text    country code or part of a country name (country:es, country:spa)
//   type:text       artist type (person, group, ...)
//   area:text       area names (area, begin area, end area)
//   genre:text      MusicBrainz genres
//   tag:text        MusicBrainz tags
//   mbid:text       MusicBrainz ID
// Words that contain spaces need quotes: genre:"hip hop".
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>
#include "../include/musicbrainz_db_types.h"

namespace mbdb_search {

inline const char* const* latin1_fold() {
    static const char* const t[64] = {
        "a",
        "a",
        "a",
        "a",
        "a",
        "a",
        "ae",
        "c",
        "e",
        "e",
        "e",
        "e",
        "i",
        "i",
        "i",
        "i",
        "d",
        "n",
        "o",
        "o",
        "o",
        "o",
        "o",
        " ",
        "o",
        "u",
        "u",
        "u",
        "u",
        "y",
        "th",
        "ss",
        "a",
        "a",
        "a",
        "a",
        "a",
        "a",
        "ae",
        "c",
        "e",
        "e",
        "e",
        "e",
        "i",
        "i",
        "i",
        "i",
        "d",
        "n",
        "o",
        "o",
        "o",
        "o",
        "o",
        " ",
        "o",
        "u",
        "u",
        "u",
        "u",
        "y",
        "th",
        "y"
    };
    return t;
}

inline const char* const* latin_ext_a_fold() {
    static const char* const t[128] = {
        "a",
        "a",
        "a",
        "a",
        "a",
        "a",
        "c",
        "c",
        "c",
        "c",
        "c",
        "c",
        "c",
        "c",
        "d",
        "d",
        "d",
        "d",
        "e",
        "e",
        "e",
        "e",
        "e",
        "e",
        "e",
        "e",
        "e",
        "e",
        "g",
        "g",
        "g",
        "g",
        "g",
        "g",
        "g",
        "g",
        "h",
        "h",
        "h",
        "h",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
        "i",
        "ij",
        "ij",
        "j",
        "j",
        "k",
        "k",
        "k",
        "l",
        "l",
        "l",
        "l",
        "l",
        "l",
        "l",
        "l",
        "l",
        "l",
        "n",
        "n",
        "n",
        "n",
        "n",
        "n",
        "n",
        "n",
        "n",
        "o",
        "o",
        "o",
        "o",
        "o",
        "o",
        "oe",
        "oe",
        "r",
        "r",
        "r",
        "r",
        "r",
        "r",
        "s",
        "s",
        "s",
        "s",
        "s",
        "s",
        "s",
        "s",
        "t",
        "t",
        "t",
        "t",
        "t",
        "t",
        "u",
        "u",
        "u",
        "u",
        "u",
        "u",
        "u",
        "u",
        "u",
        "u",
        "u",
        "u",
        "w",
        "w",
        "y",
        "y",
        "y",
        "z",
        "z",
        "z",
        "z",
        "z",
        "z",
        "s"
    };
    return t;
}

// Lower-cases ASCII and Latin letters and removes their accents, so "Shirō" and "shiro" match,
// as do "España" and "espana". Other scripts (Japanese, Cyrillic, ...) pass through unchanged.
inline std::string fold(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(std::tolower(c)));
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) { // two-byte sequence
            const uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(in[i + 1]) & 0x3Fu);
            if (cp >= 0xC0 && cp <= 0xFF) { out += latin1_fold()[cp - 0xC0]; i += 2; continue; }
            if (cp >= 0x100 && cp <= 0x17F) { out += latin_ext_a_fold()[cp - 0x100]; i += 2; continue; }
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

struct country_entry {
    const char* code;
    const char* names; // '|' separated: the English name first, then other names
};

inline const std::vector<country_entry>& country_table() {
    static const std::vector<country_entry> table = {
        { "AD", "Andorra" },
        { "AE", "United Arab Emirates|UAE|Emirates|الإمارات" },
        { "AF", "Afghanistan" },
        { "AG", "Antigua and Barbuda" },
        { "AI", "Anguilla" },
        { "AL", "Albania|Shqipëria" },
        { "AM", "Armenia|Հայաստան" },
        { "AO", "Angola" },
        { "AQ", "Antarctica" },
        { "AR", "Argentina" },
        { "AS", "American Samoa" },
        { "AT", "Austria|Österreich" },
        { "AU", "Australia" },
        { "AW", "Aruba" },
        { "AX", "Åland Islands|Åland" },
        { "AZ", "Azerbaijan|Azərbaycan" },
        { "BA", "Bosnia and Herzegovina|Bosnia|Bosna i Hercegovina" },
        { "BB", "Barbados" },
        { "BD", "Bangladesh|বাংলাদেশ" },
        { "BE", "Belgium|België|Belgique|Belgien" },
        { "BF", "Burkina Faso" },
        { "BG", "Bulgaria|България" },
        { "BH", "Bahrain" },
        { "BI", "Burundi" },
        { "BJ", "Benin|Bénin" },
        { "BL", "Saint Barthélemy" },
        { "BM", "Bermuda" },
        { "BN", "Brunei|Brunei Darussalam" },
        { "BO", "Bolivia" },
        { "BQ", "Bonaire, Sint Eustatius and Saba|Caribbean Netherlands" },
        { "BR", "Brazil|Brasil" },
        { "BS", "Bahamas|The Bahamas" },
        { "BT", "Bhutan" },
        { "BV", "Bouvet Island" },
        { "BW", "Botswana" },
        { "BY", "Belarus|Беларусь" },
        { "BZ", "Belize" },
        { "CA", "Canada" },
        { "CC", "Cocos (Keeling) Islands" },
        { "CD", "Democratic Republic of the Congo|DR Congo|DRC|Congo-Kinshasa" },
        { "CF", "Central African Republic" },
        { "CG", "Congo|Republic of the Congo|Congo-Brazzaville" },
        { "CH", "Switzerland|Schweiz|Suisse|Svizzera|Svizra|Helvetia" },
        { "CI", "Côte d'Ivoire|Ivory Coast" },
        { "CK", "Cook Islands" },
        { "CL", "Chile" },
        { "CM", "Cameroon|Cameroun" },
        { "CN", "China|中国|中國|Zhongguo|PRC|People's Republic of China" },
        { "CO", "Colombia" },
        { "CR", "Costa Rica" },
        { "CU", "Cuba" },
        { "CV", "Cape Verde|Cabo Verde" },
        { "CW", "Curaçao" },
        { "CX", "Christmas Island" },
        { "CY", "Cyprus|Κύπρος|Kıbrıs" },
        { "CZ", "Czechia|Czech Republic|Česko|Česká republika" },
        { "DE", "Germany|Deutschland|Allemagne|Alemania|Germania" },
        { "DJ", "Djibouti" },
        { "DK", "Denmark|Danmark" },
        { "DM", "Dominica" },
        { "DO", "Dominican Republic|República Dominicana" },
        { "DZ", "Algeria|Algérie|الجزائر" },
        { "EC", "Ecuador" },
        { "EE", "Estonia|Eesti" },
        { "EG", "Egypt|مصر|Misr" },
        { "EH", "Western Sahara" },
        { "ER", "Eritrea" },
        { "ES", "Spain|España|Espagne|Spanien|Spagna" },
        { "ET", "Ethiopia" },
        { "FI", "Finland|Suomi" },
        { "FJ", "Fiji" },
        { "FK", "Falkland Islands|Islas Malvinas|Malvinas" },
        { "FM", "Micronesia|Federated States of Micronesia" },
        { "FO", "Faroe Islands|Føroyar|Faroes" },
        { "FR", "France|Frankreich|Francia" },
        { "GA", "Gabon" },
        { "GB", "United Kingdom|UK|Great Britain|Britain|England|Scotland|Wales|Northern Ireland|Reino Unido" },
        { "GD", "Grenada" },
        { "GE", "Georgia|საქართველო|Sakartvelo" },
        { "GF", "French Guiana" },
        { "GG", "Guernsey" },
        { "GH", "Ghana" },
        { "GI", "Gibraltar" },
        { "GL", "Greenland|Kalaallit Nunaat|Grønland" },
        { "GM", "Gambia|The Gambia" },
        { "GN", "Guinea|Guinée" },
        { "GP", "Guadeloupe" },
        { "GQ", "Equatorial Guinea" },
        { "GR", "Greece|Ελλάδα|Hellas|Ellada|Grecia|Griechenland" },
        { "GS", "South Georgia and the South Sandwich Islands" },
        { "GT", "Guatemala" },
        { "GU", "Guam" },
        { "GW", "Guinea-Bissau" },
        { "GY", "Guyana" },
        { "HK", "Hong Kong|香港" },
        { "HM", "Heard Island and McDonald Islands" },
        { "HN", "Honduras" },
        { "HR", "Croatia|Hrvatska|Kroatien|Croacia" },
        { "HT", "Haiti|Haïti" },
        { "HU", "Hungary|Magyarország|Ungarn|Hungría" },
        { "ID", "Indonesia" },
        { "IE", "Ireland|Éire|Irlanda|Irland" },
        { "IL", "Israel|ישראל" },
        { "IM", "Isle of Man" },
        { "IN", "India|भारत|Bharat" },
        { "IO", "British Indian Ocean Territory" },
        { "IQ", "Iraq|العراق" },
        { "IR", "Iran|ایران|Persia" },
        { "IS", "Iceland|Ísland" },
        { "IT", "Italy|Italia|Italien|Italie" },
        { "JE", "Jersey" },
        { "JM", "Jamaica" },
        { "JO", "Jordan|الأردن" },
        { "JP", "Japan|日本|Nippon|Nihon|Japón|Japon" },
        { "KE", "Kenya" },
        { "KG", "Kyrgyzstan|Кыргызстан" },
        { "KH", "Cambodia|កម្ពុជា|Kampuchea" },
        { "KI", "Kiribati" },
        { "KM", "Comoros" },
        { "KN", "Saint Kitts and Nevis" },
        { "KP", "North Korea|Democratic People's Republic of Korea|DPRK|조선" },
        { "KR", "South Korea|Korea|Republic of Korea|한국|대한민국|Corea del Sur" },
        { "KW", "Kuwait|الكويت" },
        { "KY", "Cayman Islands" },
        { "KZ", "Kazakhstan|Қазақстан|Kazakstan" },
        { "LA", "Laos|Lao People's Democratic Republic" },
        { "LB", "Lebanon|لبنان|Liban" },
        { "LC", "Saint Lucia" },
        { "LI", "Liechtenstein" },
        { "LK", "Sri Lanka" },
        { "LR", "Liberia" },
        { "LS", "Lesotho" },
        { "LT", "Lithuania|Lietuva" },
        { "LU", "Luxembourg|Lëtzebuerg|Luxemburg" },
        { "LV", "Latvia|Latvija" },
        { "LY", "Libya|ليبيا" },
        { "MA", "Morocco|Maroc|المغرب|Marruecos" },
        { "MC", "Monaco" },
        { "MD", "Moldova|Republic of Moldova" },
        { "ME", "Montenegro|Crna Gora" },
        { "MF", "Saint Martin" },
        { "MG", "Madagascar" },
        { "MH", "Marshall Islands" },
        { "MK", "North Macedonia|Macedonia|Северна Македонија" },
        { "ML", "Mali" },
        { "MM", "Myanmar|Burma" },
        { "MN", "Mongolia|Монгол" },
        { "MO", "Macao|Macau|澳門" },
        { "MP", "Northern Mariana Islands" },
        { "MQ", "Martinique" },
        { "MR", "Mauritania" },
        { "MS", "Montserrat" },
        { "MT", "Malta" },
        { "MU", "Mauritius" },
        { "MV", "Maldives" },
        { "MW", "Malawi" },
        { "MX", "Mexico|México" },
        { "MY", "Malaysia" },
        { "MZ", "Mozambique|Moçambique" },
        { "NA", "Namibia" },
        { "NC", "New Caledonia|Nouvelle-Calédonie" },
        { "NE", "Niger" },
        { "NF", "Norfolk Island" },
        { "NG", "Nigeria" },
        { "NI", "Nicaragua" },
        { "NL", "Netherlands|Nederland|Holland|Pays-Bas|Países Bajos|Niederlande" },
        { "NO", "Norway|Norge|Noreg|Noruega|Norwegen" },
        { "NP", "Nepal|नेपाल" },
        { "NR", "Nauru" },
        { "NU", "Niue" },
        { "NZ", "New Zealand|Aotearoa" },
        { "OM", "Oman|عمان" },
        { "PA", "Panama|Panamá" },
        { "PE", "Peru|Perú" },
        { "PF", "French Polynesia|Polynésie française" },
        { "PG", "Papua New Guinea" },
        { "PH", "Philippines|Pilipinas|Filipinas" },
        { "PK", "Pakistan|پاکستان" },
        { "PL", "Poland|Polska|Polen|Polonia|Pologne" },
        { "PM", "Saint Pierre and Miquelon" },
        { "PN", "Pitcairn Islands|Pitcairn" },
        { "PR", "Puerto Rico" },
        { "PS", "Palestine|State of Palestine|فلسطين" },
        { "PT", "Portugal" },
        { "PW", "Palau" },
        { "PY", "Paraguay" },
        { "QA", "Qatar|قطر" },
        { "RE", "Réunion" },
        { "RO", "Romania|România|Rumänien" },
        { "RS", "Serbia|Srbija|Србија" },
        { "RU", "Russia|Russian Federation|Россия|Rossiya|Rusia|Russland" },
        { "RW", "Rwanda" },
        { "SA", "Saudi Arabia|السعودية|Arabia Saudita" },
        { "SB", "Solomon Islands" },
        { "SC", "Seychelles" },
        { "SD", "Sudan|السودان" },
        { "SE", "Sweden|Sverige|Suecia|Schweden|Suède" },
        { "SG", "Singapore|Singapura|新加坡" },
        { "SH", "Saint Helena" },
        { "SI", "Slovenia|Slovenija" },
        { "SJ", "Svalbard and Jan Mayen" },
        { "SK", "Slovakia|Slovensko|Slowakei" },
        { "SL", "Sierra Leone" },
        { "SM", "San Marino" },
        { "SN", "Senegal|Sénégal" },
        { "SO", "Somalia" },
        { "SR", "Suriname" },
        { "SS", "South Sudan" },
        { "ST", "Sao Tome and Principe|São Tomé and Príncipe" },
        { "SV", "El Salvador" },
        { "SX", "Sint Maarten" },
        { "SY", "Syria|سوريا|Syrian Arab Republic" },
        { "SZ", "Eswatini|Swaziland" },
        { "TC", "Turks and Caicos Islands" },
        { "TD", "Chad|Tchad" },
        { "TF", "French Southern Territories" },
        { "TG", "Togo" },
        { "TH", "Thailand|ประเทศไทย|Siam|Tailandia" },
        { "TJ", "Tajikistan|Тоҷикистон" },
        { "TK", "Tokelau" },
        { "TL", "Timor-Leste|East Timor" },
        { "TM", "Turkmenistan" },
        { "TN", "Tunisia|Tunisie|تونس" },
        { "TO", "Tonga" },
        { "TR", "Turkey|Türkiye|Turquía|Türkei" },
        { "TT", "Trinidad and Tobago" },
        { "TV", "Tuvalu" },
        { "TW", "Taiwan|台灣|臺灣|Republic of China" },
        { "TZ", "Tanzania" },
        { "UA", "Ukraine|Україна|Ukraina" },
        { "UG", "Uganda" },
        { "UM", "United States Minor Outlying Islands" },
        { "US", "United States|United States of America|USA|U.S.|America|EE. UU.|Estados Unidos|Vereinigte Staaten|États-Unis" },
        { "UY", "Uruguay" },
        { "UZ", "Uzbekistan|O'zbekiston" },
        { "VA", "Vatican City|Holy See|Vaticano" },
        { "VC", "Saint Vincent and the Grenadines" },
        { "VE", "Venezuela" },
        { "VG", "British Virgin Islands" },
        { "VI", "United States Virgin Islands|US Virgin Islands" },
        { "VN", "Vietnam|Viet Nam|Việt Nam" },
        { "VU", "Vanuatu" },
        { "WF", "Wallis and Futuna" },
        { "WS", "Samoa" },
        { "XK", "Kosovo" },
        { "YE", "Yemen|اليمن" },
        { "YT", "Mayotte" },
        { "ZA", "South Africa|Suid-Afrika|Sudáfrica" },
        { "ZM", "Zambia" },
        { "ZW", "Zimbabwe" },
        { "XW", "Worldwide|Global" },
        { "XE", "Europe" },
        { "SU", "Soviet Union|USSR" },
        { "YU", "Yugoslavia" },
        { "CS", "Serbia and Montenegro" },
        { "XC", "Czechoslovakia" },
        { "XG", "East Germany|German Democratic Republic" },
    };
    return table;
}

// Folded names per country, built once.
struct country_names {
    std::string code;
    std::vector<std::string> names; // folded
};

inline const std::vector<country_names>& folded_countries() {
    static const std::vector<country_names> all = [] {
        std::vector<country_names> v;
        for (const auto& e : country_table()) {
            country_names c;
            c.code = e.code;
            std::string names = e.names;
            size_t start = 0;
            while (start <= names.size()) {
                size_t bar = names.find('|', start);
                if (bar == std::string::npos) bar = names.size();
                if (bar > start) c.names.push_back(fold(names.substr(start, bar - start)));
                start = bar + 1;
            }
            v.push_back(std::move(c));
        }
        return v;
    }();
    return all;
}

// Country codes whose name equals `folded` (exact == true) or contains it (exact == false).
inline std::vector<std::string> countries_for(const std::string& folded, bool exact) {
    std::vector<std::string> codes;
    if (folded.empty()) return codes;
    for (const auto& c : folded_countries()) {
        for (const auto& n : c.names) {
            if (exact ? n == folded : n.find(folded) != std::string::npos) { codes.push_back(c.code); break; }
        }
    }
    return codes;
}

// One artist prepared for matching: everything folded once, not per keystroke.
struct row {
    std::string name;    // display name
    std::string names;   // sort name + aliases
    std::string extra;   // disambiguation
    std::string area;    // area, begin area, end area
    std::string genres;
    std::string tags;
    std::string mbid;
    std::string country; // upper case code
    std::string type;
};

// `item.search` is built by the database: five folded sections separated by 0x1F, in this
// order: names, disambiguation, areas, genres, tags (lines inside a section end with '\n').
inline row make_row(const artist_list_item& item) {
    row r;
    r.name = fold(item.name);
    std::vector<std::string> parts;
    for (size_t start = 0;;) {
        const size_t sep = item.search.find('\x1f', start);
        parts.push_back(item.search.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    auto at = [&](size_t i) { return i < parts.size() ? parts[i] : std::string(); };
    r.names = at(0);
    r.extra = at(1);
    r.area = at(2);
    r.genres = at(3);
    r.tags = at(4);
    r.mbid = fold(item.mbid);
    for (char c : item.country) r.country.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    r.type = fold(item.type);
    return r;
}

struct term {
    enum kind_t { text, code, name, country, type, area, genre, tag, mbid } kind = text;
    std::string value;              // folded
    std::vector<std::string> codes; // country codes this term stands for
};

struct query {
    std::vector<term> terms;
    bool empty() const { return terms.empty(); }
};

inline query parse_query(const std::string& raw) {
    query q;

    // A whole query that is a multi-word country name ("united kingdom", "south korea") is one
    // term; otherwise its words would be matched separately.
    {
        size_t b = 0, e = raw.size();
        while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
        const std::string whole = raw.substr(b, e - b);
        if (whole.find(' ') != std::string::npos && whole.find('"') == std::string::npos &&
            whole.find(':') == std::string::npos) {
            const std::string folded = fold(whole);
            const auto codes = countries_for(folded, true);
            if (!codes.empty()) {
                term t;
                t.kind = term::text;
                t.value = folded;
                t.codes = codes;
                q.terms.push_back(std::move(t));
                return q;
            }
        }
    }

    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= raw.size()) break;

        std::string field, token;
        bool quoted = false;
        // optional "field:" prefix
        size_t colon = i;
        while (colon < raw.size() && std::isalpha(static_cast<unsigned char>(raw[colon]))) ++colon;
        if (colon < raw.size() && raw[colon] == ':' && colon > i) {
            const std::string f = fold(raw.substr(i, colon - i));
            if (f == "name" || f == "country" || f == "type" || f == "mbid" || f == "area" || f == "genre" || f == "tag") {
                field = f;
                i = colon + 1;
            }
        }
        if (i < raw.size() && raw[i] == '"') {
            quoted = true;
            ++i;
            while (i < raw.size() && raw[i] != '"') token.push_back(raw[i++]);
            if (i < raw.size()) ++i;
        } else {
            while (i < raw.size() && !std::isspace(static_cast<unsigned char>(raw[i]))) token.push_back(raw[i++]);
        }
        if (token.empty()) continue;

        term t;
        t.value = fold(token);
        bool two_upper = token.size() == 2 && !quoted;
        for (char c : token) if (c < 'A' || c > 'Z') two_upper = false;

        if (field == "name") t.kind = term::name;
        else if (field == "type") t.kind = term::type;
        else if (field == "area") t.kind = term::area;
        else if (field == "genre") t.kind = term::genre;
        else if (field == "tag") t.kind = term::tag;
        else if (field == "mbid") t.kind = term::mbid;
        else if (field == "country") {
            t.kind = term::country;
            if (token.size() == 2) {
                std::string up;
                for (char c : token) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                t.codes.push_back(up);
            }
            if (token.size() != 2) { // two letters mean the code only, not "es" inside other names
                const auto by_name = countries_for(t.value, false);
                t.codes.insert(t.codes.end(), by_name.begin(), by_name.end());
            }
        }
        else if (two_upper) {
            t.kind = term::code;
            t.codes.push_back(token);
        }
        else {
            t.kind = term::text;
            t.codes = countries_for(t.value, true); // a full country name also matches that country
        }
        q.terms.push_back(std::move(t));
    }
    return q;
}

inline bool has_code(const std::vector<std::string>& codes, const std::string& country) {
    for (const auto& c : codes) if (c == country) return true;
    return false;
}

inline bool matches(const query& q, const row& r) {
    for (const auto& t : q.terms) {
        bool ok = false;
        switch (t.kind) {
        case term::code:
            ok = has_code(t.codes, r.country);
            break;
        case term::name:
            ok = r.name.find(t.value) != std::string::npos || r.names.find(t.value) != std::string::npos;
            break;
        case term::country:
            ok = has_code(t.codes, r.country);
            break;
        case term::type:
            ok = r.type.find(t.value) != std::string::npos;
            break;
        case term::area:
            ok = r.area.find(t.value) != std::string::npos;
            break;
        case term::genre:
            ok = r.genres.find(t.value) != std::string::npos;
            break;
        case term::tag:
            ok = r.tags.find(t.value) != std::string::npos;
            break;
        case term::mbid:
            ok = r.mbid.find(t.value) != std::string::npos;
            break;
        case term::text:
            ok = r.name.find(t.value) != std::string::npos || r.names.find(t.value) != std::string::npos ||
                 r.extra.find(t.value) != std::string::npos || r.area.find(t.value) != std::string::npos ||
                 r.mbid.find(t.value) != std::string::npos ||
                 has_code(t.codes, r.country);
            break;
        }
        if (!ok) return false;
    }
    return true;
}

} // namespace mbdb_search
