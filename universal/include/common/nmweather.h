/*
	nerdMod weather: data model, cache file, provider response parsing. Pure code (no libnds, no network) so it is host-tested
	against stored responses. Nothing here invents data: a value that cannot be parsed or is implausible is rejected.

	Providers
	  * "relay"      a plain-HTTP JSON endpoint the owner runs/chooses:  {"v":1,"temp_c":21.4,"code":3,"time":1700000000}
	  * "open-meteo" api.open-meteo.com "current" block (HTTPS only - needs a TLS-capable transport, see docs/WEATHER.md)
	Both carry the temperature in Celsius and a WMO weather code.
*/
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmjson.h"

namespace nmweather {

enum Condition { COND_UNKNOWN = 0, COND_CLEAR, COND_PARTLY, COND_CLOUDY, COND_FOG, COND_RAIN, COND_STORM, COND_SNOW };

inline const char *conditionName(Condition c) {
	switch (c) {
		case COND_CLEAR: return "clear";
		case COND_PARTLY: return "partly";
		case COND_CLOUDY: return "cloudy";
		case COND_FOG: return "fog";
		case COND_RAIN: return "rain";
		case COND_STORM: return "storm";
		case COND_SNOW: return "snow";
		default: return "unknown";
	}
}

inline Condition conditionFromName(const char *s) {
	for (int c = COND_CLEAR; c <= COND_SNOW; c++)
		if (strcmp(s, conditionName((Condition)c)) == 0)
			return (Condition)c;
	return COND_UNKNOWN;
}

// WMO weather interpretation codes (as used by Open-Meteo).
inline Condition fromWmo(int code) {
	if (code == 0) return COND_CLEAR;
	if (code == 1 || code == 2) return COND_PARTLY;
	if (code == 3) return COND_CLOUDY;
	if (code == 45 || code == 48) return COND_FOG;
	if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return COND_RAIN;
	if ((code >= 71 && code <= 77) || code == 85 || code == 86) return COND_SNOW;
	if (code >= 95 && code <= 99) return COND_STORM;
	return COND_UNKNOWN;
}

struct Data {
	bool valid = false;
	int tempC10 = 0;			// tenths of a degree Celsius
	Condition cond = COND_UNKNOWN;
	int64_t time = 0;			// unix time of the observation (or of the fetch)
	char location[40] = "";
	char provider[24] = "";
};

constexpr int MIN_TEMP_C10 = -900, MAX_TEMP_C10 = 600; // -90.0 .. 60.0 C

// ---- tiny JSON number finder ----------------------------------------------------------------------------
// Finds "key": <number> anywhere in the text. Not a JSON parser: it only has to read flat provider answers, and it
// refuses anything that does not look like a number right after the colon.
inline bool findNumber(const char *json, const char *key, double &out) {
	if (!json || !key)
		return false;
	char pat[48];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = json;
	while ((p = strstr(p, pat)) != nullptr) {
		p += strlen(pat);
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p != ':')
			continue;
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (!(*p == '-' || *p == '+' || (*p >= '0' && *p <= '9')))
			continue;
		char *end = nullptr;
		const double v = strtod(p, &end);
		if (end == p)
			continue;
		out = v;
		return true;
	}
	return false;
}

inline bool tempFromDouble(double c, int &tenths) {
	if (!(c > -99.0 && c < 99.0)) // also rejects NaN
		return false;
	const int t = (int)(c * 10.0 + (c >= 0 ? 0.5 : -0.5));
	if (t < MIN_TEMP_C10 || t > MAX_TEMP_C10)
		return false;
	tenths = t;
	return true;
}

// Relay answer or Open-Meteo answer; `now` stamps the observation when the provider gives no time.
inline bool parseResponse(const char *body, int64_t now, Data &out) {
	if (!body)
		return false;
	double t = 0, code = 0, when = 0;
	bool haveT = findNumber(body, "temp_c", t) || findNumber(body, "temperature_2m", t);
	bool haveC = findNumber(body, "code", code) || findNumber(body, "weather_code", code);
	if (!haveT || !haveC)
		return false;
	int tenths;
	if (!tempFromDouble(t, tenths) || code < 0 || code > 99)
		return false;
	const Condition c = fromWmo((int)code);
	if (c == COND_UNKNOWN)
		return false;
	Data d;
	d.valid = true;
	d.tempC10 = tenths;
	d.cond = c;
	d.time = now;
	if (findNumber(body, "time", when) && when > 1577836800.0 && when <= (double)now + 86400.0)
		d.time = (int64_t)when;
	out = d;
	return true;
}

// ---- cache file -------------------------------------------------------------------------------------------
constexpr int CACHE_VERSION = 1;

inline int serialize(char *out, size_t n, const Data &d) {
	return snprintf(out, n, "[WEATHER]\nVERSION=%d\nTEMP_C10=%d\nCONDITION=%s\nTIME=%lld\nLOCATION=%s\nPROVIDER=%s\n", CACHE_VERSION, d.tempC10,
					conditionName(d.cond), (long long)d.time, d.location, d.provider);
}

inline bool lineValue(const char *text, const char *key, char *out, size_t n) {
	const size_t kl = strlen(key);
	for (const char *p = text; *p;) {
		if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
			p += kl + 1;
			size_t i = 0;
			while (*p && *p != '\n' && *p != '\r' && i + 1 < n)
				out[i++] = *p++;
			out[i] = 0;
			return true;
		}
		while (*p && *p != '\n')
			p++;
		if (*p == '\n')
			p++;
	}
	return false;
}

inline bool parseCache(const char *text, Data &out) {
	char v[64];
	if (!text || !lineValue(text, "VERSION", v, sizeof(v)) || atoi(v) != CACHE_VERSION)
		return false;
	Data d;
	if (!lineValue(text, "TEMP_C10", v, sizeof(v)))
		return false;
	d.tempC10 = atoi(v);
	if (d.tempC10 < MIN_TEMP_C10 || d.tempC10 > MAX_TEMP_C10)
		return false;
	if (!lineValue(text, "CONDITION", v, sizeof(v)))
		return false;
	d.cond = conditionFromName(v);
	if (d.cond == COND_UNKNOWN)
		return false;
	if (!lineValue(text, "TIME", v, sizeof(v)))
		return false;
	d.time = atoll(v);
	if (d.time < 1577836800)
		return false;
	lineValue(text, "LOCATION", d.location, sizeof(d.location));
	lineValue(text, "PROVIDER", d.provider, sizeof(d.provider));
	d.valid = true;
	out = d;
	return true;
}

// ---- display ----------------------------------------------------------------------------------------------
// Temperature text like "21C" / "-3C" / "70F"; rounds half away from zero.
inline void tempText(char *out, size_t n, int tempC10, bool fahrenheit) {
	int v;
	if (fahrenheit) {
		const int f10 = tempC10 * 9 / 5 + 320;
		v = (f10 >= 0 ? f10 + 5 : f10 - 5) / 10;
	} else {
		v = (tempC10 >= 0 ? tempC10 + 5 : tempC10 - 5) / 10;
	}
	snprintf(out, n, "%d%c", v, fahrenheit ? 'F' : 'C');
}

// ---- location settings (sd:/_nds/nerdMod/weather.ini) -------------------------------------------------------
struct Location {
	bool valid = false;
	double lat = 0, lon = 0;
	char name[40] = "";
	char relayUrl[160] = "";
};

inline bool parseLocation(const char *text, Location &out) {
	Location l;
	double lat = 0, lon = 0;
	char v[64];
	if (!text)
		return false;
	if (lineValue(text, "NAME", v, sizeof(v)))
		snprintf(l.name, sizeof(l.name), "%.39s", v);
	lineValue(text, "RELAY_URL", l.relayUrl, sizeof(l.relayUrl));
	if (lineValue(text, "LATITUDE", v, sizeof(v)))
		lat = atof(v);
	else
		return false;
	if (lineValue(text, "LONGITUDE", v, sizeof(v)))
		lon = atof(v);
	else
		return false;
	if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0 || (lat == 0.0 && lon == 0.0))
		return false; // 0,0 is "not set", never a real choice
	l.lat = lat;
	l.lon = lon;
	l.valid = true;
	out = l;
	return true;
}


// =====================================================================================================================
//  Phase 2D: forecast, city search and the full cache used by the Weather app
// =====================================================================================================================

// ---- calendar helpers (no libc time zone code: the answer carries its own UTC offset) -------------------------------
inline int64_t daysFromCivil(int y, int m, int d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const int yoe = (int)(y - era * 400);
	const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

// "2026-10-05" -> weekday 0 = Sunday .. 6 = Saturday, or -1
inline int weekdayOfDate(const char *date) {
	int y, m, d;
	if (!date || sscanf(date, "%d-%d-%d", &y, &m, &d) != 3 || y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31)
		return -1;
	const int64_t days = daysFromCivil(y, m, d); // 1970-01-01 was a Thursday (4)
	return (int)(((days % 7) + 7 + 4) % 7);
}

inline const char *weekdayName(int wd) {
	static const char *n[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
	return wd >= 0 && wd < 7 ? n[wd] : "---";
}

// "2026-10-05T12:30" (local time of the place) + utc offset seconds -> unix time; false if malformed
inline bool parseLocalTime(const char *t, int utcOffset, int64_t &unixOut) {
	int y, m, d, hh = 0, mm = 0;
	if (!t || sscanf(t, "%d-%d-%dT%d:%d", &y, &m, &d, &hh, &mm) < 3 || y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59)
		return false;
	unixOut = daysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60 - utcOffset;
	return true;
}

// ---- forecast ----------------------------------------------------------------------------------------------------
constexpr int MAX_DAYS = 7;
constexpr int NO_VALUE = -999;

struct Day {
	char date[11] = "";	// "2026-10-05"
	int hiC10 = NO_VALUE, loC10 = NO_VALUE;
	Condition cond = COND_UNKNOWN;
	int precip = -1;	// percent, -1 unknown
};

struct Forecast {
	bool valid = false;
	Data now;			// current conditions (tempC10, cond, time)
	bool isDay = true;
	int utcOffset = 0;
	char tz[40] = "";
	int dayCount = 0;
	Day days[MAX_DAYS];
};

inline bool jsonTemp(const nmjson::Doc &doc, int node, int &tenths) {
	double v;
	return doc.number(node, v) && tempFromDouble(v, tenths);
}

// Open-Meteo /v1/forecast answer (current + daily). Needs the current block; daily entries that are bad are skipped,
// never invented.
inline bool parseForecast(const char *body, size_t len, int64_t nowUnix, Forecast &out) {
	nmjson::Doc *doc = new nmjson::Doc();
	Forecast f;
	bool ok = false;
	do {
		if (!doc->parse(body, len))
			break;
		const int root = doc->root();
		const int cur = doc->get(root, "current");
		if (cur < 0)
			break;
		double t = 0, code = 0, off = 0, isDay = 1;
		int tenths;
		if (!jsonTemp(*doc, doc->get(cur, "temperature_2m"), tenths))
			break;
		if (!doc->number(doc->get(cur, "weather_code"), code) || code < 0 || code > 99)
			break;
		const Condition c = fromWmo((int)code);
		if (c == COND_UNKNOWN)
			break;
		(void)t;
		doc->number(doc->get(root, "utc_offset_seconds"), off);
		if (off < -50400 || off > 50400)
			off = 0;
		f.utcOffset = (int)off;
		doc->string(doc->get(root, "timezone"), f.tz, sizeof f.tz);
		if (doc->number(doc->get(cur, "is_day"), isDay))
			f.isDay = isDay != 0;
		f.now.valid = true;
		f.now.tempC10 = tenths;
		f.now.cond = c;
		f.now.time = nowUnix;
		char ts[24];
		int64_t when;
		if (doc->string(doc->get(cur, "time"), ts, sizeof ts) && parseLocalTime(ts, f.utcOffset, when) && when > 1577836800 && when <= nowUnix + 86400)
			f.now.time = when;
		const int daily = doc->get(root, "daily");
		const int dTime = doc->get(daily, "time"), dCode = doc->get(daily, "weather_code"), dMax = doc->get(daily, "temperature_2m_max"),
				  dMin = doc->get(daily, "temperature_2m_min"), dPop = doc->get(daily, "precipitation_probability_max");
		const int n = doc->size(dTime);
		for (int i = 0; i < n && i < MAX_DAYS && dTime >= 0; i++) {
			Day d;
			if (!doc->string(doc->at(dTime, i), d.date, sizeof d.date) || weekdayOfDate(d.date) < 0)
				continue;
			double dc;
			if (!doc->number(doc->at(dCode, i), dc) || dc < 0 || dc > 99)
				continue;
			d.cond = fromWmo((int)dc);
			if (d.cond == COND_UNKNOWN)
				continue;
			if (!jsonTemp(*doc, doc->at(dMax, i), d.hiC10) || !jsonTemp(*doc, doc->at(dMin, i), d.loC10))
				continue;
			double pp;
			if (doc->number(doc->at(dPop, i), pp) && pp >= 0 && pp <= 100)
				d.precip = (int)pp;
			f.days[f.dayCount++] = d;
		}
		f.valid = true;
		ok = true;
	} while (false);
	delete doc;
	if (ok)
		out = f;
	return ok;
}

// ---- city search (Open-Meteo geocoding) ----------------------------------------------------------------------------
constexpr int MAX_PLACES = 8;

struct Place {
	char name[32] = "";
	char region[28] = "";
	char country[28] = "";
	char timezone[40] = "";
	double lat = 0, lon = 0;
	bool valid() const { return name[0] && !(lat == 0 && lon == 0); }
};

struct PlaceList {
	int count = 0;
	Place p[MAX_PLACES];
};

// {"results":[{...}]} ; an answer without "results" is a valid empty list; malformed JSON is false.
inline bool parseGeocode(const char *body, size_t len, PlaceList &out) {
	nmjson::Doc *doc = new nmjson::Doc();
	PlaceList l;
	bool ok = false;
	if (doc->parse(body, len) && doc->type(doc->root()) == nmjson::T_OBJ) {
		ok = true;
		const int res = doc->get(doc->root(), "results");
		const int n = doc->size(res);
		for (int i = 0; i < n && l.count < MAX_PLACES; i++) {
			const int r = doc->at(res, i);
			Place p;
			if (!doc->string(doc->get(r, "name"), p.name, sizeof p.name))
				continue;
			doc->string(doc->get(r, "admin1"), p.region, sizeof p.region);
			doc->string(doc->get(r, "country"), p.country, sizeof p.country);
			doc->string(doc->get(r, "timezone"), p.timezone, sizeof p.timezone);
			if (!doc->number(doc->get(r, "latitude"), p.lat) || !doc->number(doc->get(r, "longitude"), p.lon))
				continue;
			if (p.lat < -90 || p.lat > 90 || p.lon < -180 || p.lon > 180 || !p.valid())
				continue;
			l.p[l.count++] = p;
		}
	}
	delete doc;
	if (ok)
		out = l;
	return ok;
}

// Percent-encodes a search text (UTF-8 bytes allowed). Letters, digits and -_.~ stay; everything else becomes %XX.
inline void urlEncode(const char *in, char *out, size_t cap) {
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~')
			out[o++] = (char)*p;
		else
			o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
	}
	out[o] = 0;
}

inline void geocodeUrl(char *out, size_t cap, const char *query) {
	char enc[96];
	urlEncode(query, enc, sizeof enc);
	snprintf(out, cap, "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json", enc, MAX_PLACES);
}

inline void forecastUrl(char *out, size_t cap, double lat, double lon) {
	snprintf(out, cap,
			 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,is_day"
			 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=auto&forecast_days=%d",
			 lat, lon, MAX_DAYS);
}

// ---- settings (extended weather.ini) ---------------------------------------------------------------------------------
struct Settings {
	Location place;				// NAME / LATITUDE / LONGITUDE (the first three keys older builds already read)
	char country[28] = "";
	char timezone[40] = "";
	bool fahrenheit = false;
	bool autoRefresh = true;
	int intervalMin = 60;		// 30, 60, 180, 360
};

inline bool validInterval(int m) { return m == 30 || m == 60 || m == 180 || m == 360; }

inline Settings parseSettings(const char *text) {
	Settings s;
	if (!text)
		return s;
	Location l;
	if (parseLocation(text, l))
		s.place = l;
	char v[64];
	lineValue(text, "COUNTRY", s.country, sizeof s.country);
	lineValue(text, "TIMEZONE", s.timezone, sizeof s.timezone);
	if (lineValue(text, "UNITS", v, sizeof v))
		s.fahrenheit = (v[0] == 'F' || v[0] == 'f');
	if (lineValue(text, "AUTO_REFRESH", v, sizeof v))
		s.autoRefresh = atoi(v) != 0;
	if (lineValue(text, "INTERVAL_MIN", v, sizeof v) && validInterval(atoi(v)))
		s.intervalMin = atoi(v);
	return s;
}

inline int serializeSettings(char *out, size_t n, const Settings &s) {
	return snprintf(out, n, "[WEATHER]\nNAME=%s\nLATITUDE=%.4f\nLONGITUDE=%.4f\nCOUNTRY=%s\nTIMEZONE=%s\nUNITS=%c\nAUTO_REFRESH=%d\nINTERVAL_MIN=%d\n", s.place.name, s.place.lat,
					s.place.lon, s.country, s.timezone, s.fahrenheit ? 'F' : 'C', s.autoRefresh ? 1 : 0, s.intervalMin);
}

// ---- full cache (the home card reads the first block; the Weather app reads everything) --------------------------------
// Same [WEATHER] file and VERSION=1 as before, with more keys. Older readers ignore the extra keys.
inline int serializeFull(char *out, size_t n, const Forecast &f, const char *location, const char *provider, const char *country) {
	Data d = f.now;
	snprintf(d.location, sizeof d.location, "%.39s", location);
	snprintf(d.provider, sizeof d.provider, "%.23s", provider);
	int o = serialize(out, n, d);
	if (o < 0 || (size_t)o >= n)
		return o;
	o += snprintf(out + o, n - (size_t)o, "COUNTRY=%s\nTZ=%s\nUTC_OFFSET=%d\nISDAY=%d\nDAYS=%d\n", country, f.tz, f.utcOffset, f.isDay ? 1 : 0, f.dayCount);
	for (int i = 0; i < f.dayCount && (size_t)o < n; i++)
		o += snprintf(out + o, n - (size_t)o, "D%d=%s,%d,%d,%s,%d\n", i, f.days[i].date, f.days[i].hiC10, f.days[i].loC10, conditionName(f.days[i].cond), f.days[i].precip);
	return o;
}

inline bool parseFull(const char *text, Forecast &out, char *location = nullptr, size_t locCap = 0, char *country = nullptr, size_t countryCap = 0) {
	Data d;
	if (!parseCache(text, d))
		return false;
	Forecast f;
	f.now = d;
	char v[96];
	if (lineValue(text, "TZ", f.tz, sizeof f.tz)) {}
	if (lineValue(text, "UTC_OFFSET", v, sizeof v))
		f.utcOffset = atoi(v);
	if (lineValue(text, "ISDAY", v, sizeof v))
		f.isDay = atoi(v) != 0;
	int n = 0;
	if (lineValue(text, "DAYS", v, sizeof v))
		n = atoi(v);
	if (n < 0 || n > MAX_DAYS)
		n = 0;
	for (int i = 0; i < n; i++) {
		char key[16];
		snprintf(key, sizeof key, "D%d", i);
		if (!lineValue(text, key, v, sizeof v))
			continue;
		Day day;
		char date[16], cond[16];
		int hi, lo, pp;
		if (sscanf(v, "%15[0-9-],%d,%d,%15[a-z],%d", date, &hi, &lo, cond, &pp) != 5 || strlen(date) != 10 || weekdayOfDate(date) < 0)
			continue;
		day.cond = conditionFromName(cond);
		if (day.cond == COND_UNKNOWN || hi < MIN_TEMP_C10 || hi > MAX_TEMP_C10 || lo < MIN_TEMP_C10 || lo > MAX_TEMP_C10)
			continue;
		memcpy(day.date, date, 11);
		day.hiC10 = hi;
		day.loC10 = lo;
		day.precip = (pp >= 0 && pp <= 100) ? pp : -1;
		f.days[f.dayCount++] = day;
	}
	f.valid = true;
	if (location && locCap)
		snprintf(location, locCap, "%s", d.location);
	if (country && countryCap)
		lineValue(text, "COUNTRY", country, countryCap);
	out = f;
	return true;
}

// Day number (days since 1970-01-01) of "YYYY-MM-DD", or INT64_MIN when malformed.
inline int64_t dayNumber(const char *date) {
	int y, m, d;
	if (!date || strlen(date) != 10 || sscanf(date, "%d-%d-%d", &y, &m, &d) != 3 || y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31)
		return INT64_MIN;
	return daysFromCivil(y, m, d);
}

// The local calendar day (days since 1970) of the place at unix time `t`.
inline int64_t localDay(int64_t t, int utcOffset) {
	const int64_t s = t + utcOffset;
	return s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
}

// "TODAY", "TOMORROW" or "WED 15" for a forecast row, relative to the place's current local day.
inline void dayLabel(char *out, size_t n, const char *date, int64_t today) {
	const int64_t dn = dayNumber(date);
	if (dn == INT64_MIN)
		snprintf(out, n, "---");
	else if (dn == today)
		snprintf(out, n, "TODAY");
	else if (dn == today + 1)
		snprintf(out, n, "TOMORROW");
	else
		snprintf(out, n, "%s %d", weekdayName(weekdayOfDate(date)), atoi(date + 8));
}

// ---- age text ("just now", "5 min ago", "2 h ago", "3 days ago") -------------------------------------------------------
inline void ageText(char *out, size_t n, int64_t now, int64_t then) {
	const int64_t s = now - then;
	if (then <= 0 || s < 0)
		snprintf(out, n, "time unknown");
	else if (s < 90)
		snprintf(out, n, "just now");
	else if (s < 3600)
		snprintf(out, n, "%d min ago", (int)((s + 30) / 60));
	else if (s < 86400)
		snprintf(out, n, "%d h ago", (int)(s / 3600));
	else
		snprintf(out, n, "%d days ago", (int)(s / 86400));
}

// Is the stored observation older than the auto-refresh interval (or in the future by more than a day = clock problem)?
inline bool isStale(int64_t now, int64_t then, int intervalMin) { return then <= 0 || now < then - 86400 || now - then >= (int64_t)intervalMin * 60; }

// Cheap sanity check of the DSi clock: certificates cannot be verified with a wrong date. 2026-01-01 is the floor.
inline bool clockLooksSet(int64_t now) { return now >= 1767225600; }

} // namespace nmweather
