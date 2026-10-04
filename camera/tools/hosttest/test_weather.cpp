#include <stdio.h>
#include "../../../universal/include/common/nmweather.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
using namespace nmweather;
int main() {
	const int64_t NOW = 1760000000;
	Data d;
	// stored Open-Meteo answer
	const char *om = "{\"latitude\":52.52,\"longitude\":13.419,\"generationtime_ms\":0.05,\"current_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\\u00b0C\",\"weather_code\":\"wmo code\"},"
					 "\"current\":{\"time\":\"2025-10-09T08:00\",\"interval\":900,\"temperature_2m\":14.3,\"weather_code\":61}}";
	CHECK(parseResponse(om, NOW, d) && d.valid && d.tempC10 == 143 && d.cond == COND_RAIN && d.time == NOW);
	// relay answer with a time
	CHECK(parseResponse("{\"v\":1,\"temp_c\":-3.46,\"code\":71,\"time\":1759990000}", NOW, d) && d.tempC10 == -35 && d.cond == COND_SNOW && d.time == 1759990000);
	CHECK(parseResponse("{ \"temp_c\" : 21 , \"code\" : 0 }", NOW, d) && d.tempC10 == 210 && d.cond == COND_CLEAR);
	CHECK(parseResponse("{\"temp_c\":30,\"code\":95}", NOW, d) && d.cond == COND_STORM);
	CHECK(parseResponse("{\"temp_c\":5,\"code\":45}", NOW, d) && d.cond == COND_FOG);
	CHECK(parseResponse("{\"temp_c\":5,\"code\":3}", NOW, d) && d.cond == COND_CLOUDY);
	CHECK(parseResponse("{\"temp_c\":5,\"code\":2}", NOW, d) && d.cond == COND_PARTLY);
	// a future "time" is not believed
	CHECK(parseResponse("{\"temp_c\":5,\"code\":2,\"time\":1999999999}", NOW, d) && d.time == NOW);
	// malformed / hostile answers are rejected, nothing is made up
	CHECK(!parseResponse(nullptr, NOW, d));
	CHECK(!parseResponse("", NOW, d));
	CHECK(!parseResponse("<html>502 Bad Gateway</html>", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":21}", NOW, d));
	CHECK(!parseResponse("{\"code\":3}", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":\"warm\",\"code\":3}", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":nan,\"code\":3}", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":999,\"code\":3}", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":-120,\"code\":3}", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":20,\"code\":-1}", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":20,\"code\":1000}", NOW, d));
	CHECK(!parseResponse("{\"temp_c\":20,\"code\":42}", NOW, d)); // unknown WMO code
	CHECK(!parseResponse("{\"temp_c\":", NOW, d));
	CHECK(!parseResponse("{\"temp_c\"", NOW, d));
	{ char big[4096]; memset(big, '{', sizeof big); big[sizeof big - 1] = 0; CHECK(!parseResponse(big, NOW, d)); }
	// failed parse leaves the previous data alone
	Data keep; keep.valid = true; keep.tempC10 = 77;
	CHECK(!parseResponse("garbage", NOW, keep) && keep.tempC10 == 77);
	// cache round trip + stale age
	Data c; c.valid = true; c.tempC10 = -35; c.cond = COND_SNOW; c.time = NOW - 7300;
	snprintf(c.location, sizeof c.location, "Home"); snprintf(c.provider, sizeof c.provider, "relay-v1");
	char buf[512]; serialize(buf, sizeof buf, c);
	Data r;
	CHECK(parseCache(buf, r) && r.tempC10 == -35 && r.cond == COND_SNOW && r.time == c.time && !strcmp(r.location, "Home") && !strcmp(r.provider, "relay-v1"));
	// damaged caches are ignored
	CHECK(!parseCache("", r));
	CHECK(!parseCache("[WEATHER]\nVERSION=2\nTEMP_C10=1\nCONDITION=clear\nTIME=1760000000\n", r));
	CHECK(!parseCache("[WEATHER]\nVERSION=1\nTEMP_C10=5000\nCONDITION=clear\nTIME=1760000000\n", r));
	CHECK(!parseCache("[WEATHER]\nVERSION=1\nTEMP_C10=10\nCONDITION=plasma\nTIME=1760000000\n", r));
	CHECK(!parseCache("[WEATHER]\nVERSION=1\nTEMP_C10=10\nCONDITION=clear\nTIME=5\n", r));
	CHECK(!parseCache("[WEATHER]\nVERSION=1\nTEMP_C10=10\nCONDITION=clear\n", r));
	CHECK(!parseCache(nullptr, r));
	// temperature text
	char t[16];
	tempText(t, sizeof t, 214, false); CHECK(!strcmp(t, "21C"));
	tempText(t, sizeof t, 215, false); CHECK(!strcmp(t, "22C"));
	tempText(t, sizeof t, -35, false); CHECK(!strcmp(t, "-4C"));
	tempText(t, sizeof t, 0, true); CHECK(!strcmp(t, "32F"));
	tempText(t, sizeof t, 215, true); CHECK(!strcmp(t, "71F"));
	tempText(t, sizeof t, -400, true); CHECK(!strcmp(t, "-40F"));
	// location file
	Location l;
	CHECK(parseLocation("[WEATHER]\nNAME=Lahore\nLATITUDE=31.5497\nLONGITUDE=74.3436\n", l) && l.valid && !strcmp(l.name, "Lahore") && l.lat > 31.5 && l.lon > 74.3);
	CHECK(parseLocation("LATITUDE=-33.9\nLONGITUDE=151.2\nRELAY_URL=http://example.invalid/w\n", l) && !strcmp(l.relayUrl, "http://example.invalid/w"));
	CHECK(!parseLocation("LATITUDE=0\nLONGITUDE=0\n", l));      // unset
	CHECK(!parseLocation("LATITUDE=91\nLONGITUDE=10\n", l));
	CHECK(!parseLocation("LATITUDE=10\nLONGITUDE=181\n", l));
	CHECK(!parseLocation("LATITUDE=10\n", l));
	CHECK(!parseLocation("", l));
	printf(failures ? "%d FAILURES\n" : "weather tests OK\n", failures);
	return failures;
}
