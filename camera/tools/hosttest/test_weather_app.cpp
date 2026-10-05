// Host tests for the Weather app core: JSON reader, forecast, city search, cache, settings, units, stale logic.
#include <stdio.h>
#include <string>
#include "../../../universal/include/common/nmweather.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace nmweather;

static const char *FORECAST =
	"{\"latitude\":31.5,\"longitude\":74.375,\"generationtime_ms\":0.1,\"utc_offset_seconds\":18000,\"timezone\":\"Asia/Karachi\",\"timezone_abbreviation\":\"GMT+5\",\"elevation\":212.0,"
	"\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\",\"temperature_2m\":\"\\u00b0C\",\"weather_code\":\"wmo code\",\"is_day\":\"\"},"
	"\"current\":{\"time\":\"2025-10-09T08:15\",\"interval\":900,\"temperature_2m\":31.4,\"weather_code\":1,\"is_day\":1},"
	"\"daily_units\":{\"time\":\"iso8601\"},"
	"\"daily\":{\"time\":[\"2025-10-09\",\"2025-10-10\",\"2025-10-11\",\"2025-10-12\",\"2025-10-13\",\"2025-10-14\",\"2025-10-15\"],"
	"\"weather_code\":[1,2,3,61,80,95,0],\"temperature_2m_max\":[34.1,33.0,32.2,29.9,28.0,30.5,35.0],\"temperature_2m_min\":[22.0,21.5,21.0,20.4,19.9,20.2,22.8],"
	"\"precipitation_probability_max\":[0,5,10,60,70,80,null]}}";

static const char *GEO =
	"{\"results\":[{\"id\":1172451,\"name\":\"Lahore\",\"latitude\":31.558,\"longitude\":74.35071,\"elevation\":217.0,\"feature_code\":\"PPLA\",\"country_code\":\"PK\",\"admin1_id\":1168700,"
	"\"timezone\":\"Asia/Karachi\",\"population\":10355000,\"country_id\":1168579,\"country\":\"Pakistan\",\"admin1\":\"Punjab\"},"
	"{\"id\":4707,\"name\":\"Lahore\",\"latitude\":31.7,\"longitude\":73.9,\"country\":\"Pakistan\",\"admin1\":\"Punjab\",\"timezone\":\"Asia/Karachi\"},"
	"{\"id\":2,\"name\":\"Z\\u00fcrich\",\"latitude\":47.36667,\"longitude\":8.55,\"timezone\":\"Europe/Zurich\",\"country\":\"Switzerland\",\"admin1\":\"Z\\u00fcrich\"}],\"generationtime_ms\":0.9}";

int main() {
	// ---- JSON reader
	{
		nmjson::Doc *d = new nmjson::Doc();
		const char *j = "{\"a\":[1,2.5,-3e2,true,null,\"x\\ny\"],\"b\":{\"c\":\"\\u00e9t\\u00e9 \\\"q\\\"\"},\"d\":\"\xC3\xBC\"}";
		CHECK(d->parse(j, strlen(j)));
		double v; char s[32]; bool b;
		CHECK(d->number(d->at(d->get(d->root(), "a"), 1), v) && v == 2.5);
		CHECK(d->number(d->at(d->get(d->root(), "a"), 2), v) && v == -300.0);
		CHECK(d->boolean(d->at(d->get(d->root(), "a"), 3), b) && b);
		CHECK(d->type(d->at(d->get(d->root(), "a"), 4)) == nmjson::T_NULL);
		CHECK(d->string(d->at(d->get(d->root(), "a"), 5), s, sizeof s) && !strcmp(s, "x y"));
		CHECK(d->string(d->get(d->get(d->root(), "b"), "c"), s, sizeof s) && !strcmp(s, "ete \"q\""));
		CHECK(d->string(d->get(d->root(), "d"), s, sizeof s) && !strcmp(s, "u")); // folded
		CHECK(d->get(d->root(), "zz") < 0 && d->at(d->get(d->root(), "a"), 99) < 0);
		// truncation never overflows
		char tiny[4]; CHECK(d->string(d->get(d->get(d->root(), "b"), "c"), tiny, sizeof tiny) && strlen(tiny) == 3);
		// malformed documents
		const char *bad[] = {"", "{", "{\"a\":}", "{\"a\":1,}", "[1 2]", "{\"a\" 1}", "nul", "{\"a\":\"unterminated}", "{\"a\":01x}", "[1,2", "{} extra", "{\"a\":1e999}",
							 "{\"a\":\"\\u12g4\"}", "{\"a\":--1}", "\"x"};
		for (const char *t : bad) {
			nmjson::Doc *e = new nmjson::Doc();
			CHECK(!e->parse(t, strlen(t)));
			delete e;
		}
		// depth / size limits
		std::string deep(200, '[');
		deep += std::string(200, ']');
		CHECK(!d->parse(deep.data(), deep.size()));
		std::string many = "[";
		for (int i = 0; i < 2000; i++) many += i ? ",1" : "1";
		many += "]";
		CHECK(!d->parse(many.data(), many.size()));
		// not NUL terminated: parse must honour the length
		const char buf[] = {'[', '1', ',', '2', ']', 'X', 'Y'};
		CHECK(d->parse(buf, 5) && d->size(d->root()) == 2);
		delete d;
	}

	// ---- forecast
	{
		const int64_t NOW = 1760000000; // 2025-10-09 08:53 UTC
		Forecast f;
		CHECK(parseForecast(FORECAST, strlen(FORECAST), NOW, f) && f.valid);
		CHECK(f.now.tempC10 == 314 && f.now.cond == COND_PARTLY && f.isDay && f.utcOffset == 18000 && !strcmp(f.tz, "Asia/Karachi"));
		CHECK(f.now.time == 1760000100 - 52 * 0 - 0 || f.now.time > 1759900000); // local 08:15 +05:00 = 03:15 UTC
		CHECK(f.now.time == daysFromCivil(2025, 10, 9) * 86400 + 3 * 3600 + 15 * 60);
		CHECK(f.dayCount == 7);
		CHECK(!strcmp(f.days[0].date, "2025-10-09") && f.days[0].hiC10 == 341 && f.days[0].loC10 == 220 && f.days[0].cond == COND_PARTLY && f.days[0].precip == 0);
		CHECK(f.days[3].cond == COND_RAIN && f.days[3].precip == 60 && f.days[5].cond == COND_STORM);
		CHECK(f.days[6].precip == -1 && f.days[6].cond == COND_CLEAR); // null probability -> unknown, day kept
		CHECK(weekdayOfDate("2025-10-09") == 4 && !strcmp(weekdayName(4), "THU")); // 9 Oct 2025 was a Thursday
		CHECK(weekdayOfDate("1970-01-01") == -1 && weekdayOfDate("2024-02-29") == 4 && weekdayOfDate("2000-01-01") == 6);
		// without daily: still valid (current only)
		const char *cur = "{\"utc_offset_seconds\":0,\"current\":{\"time\":\"2025-10-09T08:15\",\"temperature_2m\":-4.26,\"weather_code\":71,\"is_day\":0}}";
		CHECK(parseForecast(cur, strlen(cur), NOW, f) && f.dayCount == 0 && f.now.tempC10 == -43 && f.now.cond == COND_SNOW && !f.isDay);
		// rejects
		Forecast keep; keep.valid = true; keep.dayCount = 3;
		const char *bad[] = {"", "{}", "{\"current\":{}}", "{\"current\":{\"temperature_2m\":20}}", "{\"current\":{\"temperature_2m\":\"hot\",\"weather_code\":1}}",
							 "{\"current\":{\"temperature_2m\":200,\"weather_code\":1}}", "{\"current\":{\"temperature_2m\":20,\"weather_code\":42}}", "<html>", "{\"error\":true,\"reason\":\"x\"}",
							 "{\"current\":{\"temperature_2m\":20,\"weather_code\":1}"};
		for (const char *t : bad) CHECK(!parseForecast(t, strlen(t), NOW, keep) && keep.dayCount == 3);
		// bad day rows are skipped, good ones kept
		const char *mix = "{\"current\":{\"temperature_2m\":20,\"weather_code\":1},\"daily\":{\"time\":[\"2025-10-09\",\"garbage\",\"2025-10-11\"],\"weather_code\":[1,1,99],"
						  "\"temperature_2m_max\":[25,26,27],\"temperature_2m_min\":[10,11,12]}}";
		CHECK(parseForecast(mix, strlen(mix), NOW, f) && f.dayCount == 2 && !strcmp(f.days[1].date, "2025-10-11") && f.days[1].cond == COND_STORM);
		// UNKNOWN code in a day drops the day
		const char *unk = "{\"current\":{\"temperature_2m\":20,\"weather_code\":1},\"daily\":{\"time\":[\"2025-10-09\"],\"weather_code\":[42],\"temperature_2m_max\":[25],\"temperature_2m_min\":[10]}}";
		CHECK(parseForecast(unk, strlen(unk), NOW, f) && f.dayCount == 0);
	}

	// ---- city search
	{
		PlaceList l;
		CHECK(parseGeocode(GEO, strlen(GEO), l) && l.count == 3);
		CHECK(!strcmp(l.p[0].name, "Lahore") && !strcmp(l.p[0].region, "Punjab") && !strcmp(l.p[0].country, "Pakistan") && !strcmp(l.p[0].timezone, "Asia/Karachi"));
		CHECK(l.p[0].lat > 31.55 && l.p[0].lat < 31.56 && l.p[0].lon > 74.35 && l.p[0].lon < 74.36);
		CHECK(!strcmp(l.p[2].name, "Zurich") && !strcmp(l.p[2].region, "Zurich")); // folded to ASCII
		CHECK(parseGeocode("{\"generationtime_ms\":0.6}", 25, l) && l.count == 0); // "no results" is an answer, not an error
		PlaceList keep; keep.count = 5;
		CHECK(!parseGeocode("{\"results\":[", 12, keep) && keep.count == 5);
		CHECK(!parseGeocode("[]", 2, keep));
		// results without coordinates / with 0,0 / out of range are dropped
		const char *odd = "{\"results\":[{\"name\":\"A\"},{\"name\":\"B\",\"latitude\":0,\"longitude\":0},{\"name\":\"C\",\"latitude\":91,\"longitude\":0},{\"name\":\"D\",\"latitude\":10,\"longitude\":20}]}";
		CHECK(parseGeocode(odd, strlen(odd), l) && l.count == 1 && !strcmp(l.p[0].name, "D"));
		// never more than MAX_PLACES
		std::string many = "{\"results\":[";
		for (int i = 0; i < 20; i++) many += std::string(i ? "," : "") + "{\"name\":\"N" + std::to_string(i) + "\",\"latitude\":10.5,\"longitude\":20.5}";
		many += "]}";
		CHECK(parseGeocode(many.data(), many.size(), l) && l.count == MAX_PLACES);
		char enc[96];
		urlEncode("New York", enc, sizeof enc); CHECK(!strcmp(enc, "New%20York"));
		urlEncode("S\xC3\xA3o Paulo&x=1", enc, sizeof enc); CHECK(!strcmp(enc, "S%C3%A3o%20Paulo%26x%3D1"));
		urlEncode("", enc, sizeof enc); CHECK(!strcmp(enc, ""));
		char tinyEnc[8]; urlEncode("abcdefghijkl", tinyEnc, sizeof tinyEnc); CHECK(strlen(tinyEnc) < 8);
		char url[300]; geocodeUrl(url, sizeof url, "Lahore"); CHECK(strstr(url, "name=Lahore&count=8") && !strncmp(url, "https://geocoding-api.open-meteo.com/", 37));
		forecastUrl(url, sizeof url, 31.558, 74.3507); CHECK(strstr(url, "latitude=31.5580&longitude=74.3507") && strstr(url, "timezone=auto") && strstr(url, "forecast_days=7"));
	}

	// ---- cache round trip (and compatibility with the home card's reader)
	{
		const int64_t NOW = 1760000000;
		Forecast f;
		CHECK(parseForecast(FORECAST, strlen(FORECAST), NOW, f));
		char text[1024];
		const int n = serializeFull(text, sizeof text, f, "Lahore", "open-meteo", "Pakistan");
		CHECK(n > 0 && n < (int)sizeof text);
		Forecast g; char loc[40], cc[28];
		CHECK(parseFull(text, g, loc, sizeof loc, cc, sizeof cc));
		CHECK(!strcmp(loc, "Lahore") && !strcmp(cc, "Pakistan") && g.dayCount == 7 && g.now.tempC10 == 314 && g.now.cond == COND_PARTLY && g.now.time == f.now.time);
		CHECK(!strcmp(g.days[3].date, "2025-10-12") && g.days[3].hiC10 == 299 && g.days[3].precip == 60 && g.days[6].precip == -1 && g.utcOffset == 18000 && g.isDay);
		Data old; CHECK(parseCache(text, old) && old.tempC10 == 314 && !strcmp(old.location, "Lahore")); // the home card still reads it
		// corrupt day lines are skipped, a corrupt header is rejected
		std::string t = text;
		const size_t p = t.find("D2=");
		t.replace(p, 20, "D2=garbage,,,,,,,,,,");
		CHECK(parseFull(t.c_str(), g) && g.dayCount == 6);
		CHECK(!parseFull("[WEATHER]\nVERSION=9\n", g) && !parseFull("", g) && !parseFull(nullptr, g));
		std::string t2 = text; t2.replace(t2.find("DAYS=7"), 6, "DAYS=99");
		CHECK(parseFull(t2.c_str(), g) && g.dayCount == 0);
	}

	// ---- settings
	{
		Settings s;
		s.place.valid = true; s.place.lat = 31.558; s.place.lon = 74.3507; snprintf(s.place.name, sizeof s.place.name, "Lahore");
		snprintf(s.country, sizeof s.country, "Pakistan"); snprintf(s.timezone, sizeof s.timezone, "Asia/Karachi"); s.fahrenheit = true; s.autoRefresh = false; s.intervalMin = 180;
		char text[400]; serializeSettings(text, sizeof text, s);
		Settings r = parseSettings(text);
		CHECK(r.place.valid && !strcmp(r.place.name, "Lahore") && r.fahrenheit && !r.autoRefresh && r.intervalMin == 180 && !strcmp(r.country, "Pakistan") && !strcmp(r.timezone, "Asia/Karachi"));
		CHECK(r.place.lat > 31.557 && r.place.lat < 31.559);
		// old file (Phase 2C) without the new keys keeps working with defaults
		r = parseSettings("[WEATHER]\nNAME=Lahore\nLATITUDE=31.5497\nLONGITUDE=74.3436\n");
		CHECK(r.place.valid && !r.fahrenheit && r.autoRefresh && r.intervalMin == 60);
		r = parseSettings("[WEATHER]\nNAME=X\nLATITUDE=1\nLONGITUDE=2\nINTERVAL_MIN=7\nUNITS=C\nAUTO_REFRESH=1\n");
		CHECK(r.intervalMin == 60);
		r = parseSettings(""); CHECK(!r.place.valid);
		r = parseSettings(nullptr); CHECK(!r.place.valid);
	}

	// ---- units, age, staleness, clock
	{
		char t[16];
		tempText(t, sizeof t, 314, false); CHECK(!strcmp(t, "31C"));
		tempText(t, sizeof t, 314, true); CHECK(!strcmp(t, "89F"));
		tempText(t, sizeof t, -35, false); CHECK(!strcmp(t, "-4C"));
		tempText(t, sizeof t, -35, true); CHECK(!strcmp(t, "26F"));
		char a[24];
		ageText(a, sizeof a, 1000, 960); CHECK(!strcmp(a, "just now"));
		ageText(a, sizeof a, 1000 + 300, 1000); CHECK(!strcmp(a, "5 min ago"));
		ageText(a, sizeof a, 1000 + 2 * 3600 + 10, 1000); CHECK(!strcmp(a, "2 h ago"));
		ageText(a, sizeof a, 1000 + 3 * 86400, 1000); CHECK(!strcmp(a, "3 days ago"));
		ageText(a, sizeof a, 1000, 0); CHECK(!strcmp(a, "time unknown"));
		ageText(a, sizeof a, 1000, 5000); CHECK(!strcmp(a, "time unknown"));
		CHECK(!isStale(10000, 10000 - 30 * 60 + 5, 60) && isStale(10000, 10000 - 3600, 60) && isStale(10000, 0, 60) && isStale(1000000, 1000000 + 200000, 60));
		CHECK(!clockLooksSet(946684800) && clockLooksSet(1780000000));
		CHECK(daysFromCivil(1970, 1, 1) == 0 && daysFromCivil(2000, 3, 1) == 11017);
	}

	// ---- day labels
	{
		char l[16];
		const int64_t today = dayNumber("2025-10-09");
		dayLabel(l, sizeof l, "2025-10-09", today); CHECK(!strcmp(l, "TODAY"));
		dayLabel(l, sizeof l, "2025-10-10", today); CHECK(!strcmp(l, "TOMORROW"));
		dayLabel(l, sizeof l, "2025-10-12", today); CHECK(!strcmp(l, "SUN 12"));
		dayLabel(l, sizeof l, "bogus", today); CHECK(!strcmp(l, "---"));
		// 2025-10-09 22:30 UTC is already 10 Oct in Karachi (+5h) and still 9 Oct in London
		const int64_t t = daysFromCivil(2025, 10, 9) * 86400 + 22 * 3600 + 1800;
		CHECK(localDay(t, 18000) == dayNumber("2025-10-10") && localDay(t, 0) == dayNumber("2025-10-09") && localDay(t, -3600 * 9) == dayNumber("2025-10-09"));
		CHECK(localDay(-1, 0) == -1 && localDay(0, 0) == 0);
	}

	if (failures) { printf("%d failure(s)\n", failures); return 1; }
	printf("weather app tests OK\n");
	return 0;
}
