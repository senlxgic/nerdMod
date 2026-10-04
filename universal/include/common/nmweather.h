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

} // namespace nmweather
