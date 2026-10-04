#include "playStats.h"

#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "common/inifile.h"
#include "common/nmformat.h"
#include "common/systemdetails.h"

namespace playstats {

namespace {

bool initDone = false;
bool seededNow = false;
Global totals;

struct Entry {
	uint32_t launches = 0, seconds = 0;
	int64_t last = 0;
};
std::map<std::string, Entry> games; // key = cwd + '\n' + name

std::string key(const std::string &cwd, const std::string &name) { return cwd + "\n" + name; }

std::string nerdPath() { return std::string(sys().isRunFromSD() ? "sd" : "fat") + ":/_nds/nerdMod"; }
std::string statsPath() { return nerdPath() + "/playstats.ini"; }
std::string timesPath() { return std::string(sys().isRunFromSD() ? "sd" : "fat") + ":/_nds/TWiLightMenu/extras/timesplayed.ini"; }

void ensureDir() {
	const std::string root = sys().isRunFromSD() ? "sd:" : "fat:";
	mkdir((root + "/_nds").c_str(), 0777);
	mkdir(nerdPath().c_str(), 0777);
}

} // namespace

void init() {
	if (initDone)
		return;
	initDone = true;
	if (!sys().fatInitOk())
		return;

	CIniFile ini(statsPath());
	totals.totalSeconds = (uint32_t)ini.GetInt("TOTAL", "SECONDS", 0);
	bool dirty = false;
	if (ini.GetInt("TOTAL", "SEEDED", 0) == 0) {
		// first run with nerdMod stats: take the history that timesplayed.ini already holds (read as plain text)
		uint32_t launches = 0, played = 0;
		if (FILE *f = fopen(timesPath().c_str(), "rb")) {
			char line[512];
			while (fgets(line, sizeof(line), f)) {
				if (line[0] == '[' || line[0] == ';')
					continue;
				const char *eq = strrchr(line, '=');
				if (!eq)
					continue;
				const long v = atol(eq + 1);
				if (v > 0) {
					launches += (uint32_t)v;
					played++;
				}
			}
			fclose(f);
		}
		ini.SetInt("TOTAL", "LAUNCHES", (int)launches);
		ini.SetInt("TOTAL", "GAMES", (int)played);
		ini.SetInt("TOTAL", "SEEDED", 1);
		seededNow = true;
		dirty = true;
	}
	totals.launches = (uint32_t)ini.GetInt("TOTAL", "LAUNCHES", 0);
	totals.gamesPlayed = (uint32_t)ini.GetInt("TOTAL", "GAMES", 0);

	// settle a pending launch
	const std::string pcwd = ini.GetString("PENDING", "PATH", "");
	const std::string pname = ini.GetString("PENDING", "NAME", "");
	const int64_t start = (int64_t)(unsigned)ini.GetInt("PENDING", "START", 0);
	if (!pname.empty()) {
		const int64_t now = (int64_t)time(NULL);
		const uint32_t credit = nmformat::sessionSeconds(start, now);
		uint32_t s = 0;
		int64_t last = 0;
		nmformat::parsePair(ini.GetString(pcwd, pname, "").c_str(), s, last);
		s += credit;
		if (start >= nmformat::MIN_VALID_UNIX)
			last = start;
		char buf[48];
		snprintf(buf, sizeof(buf), "%lu,%lu", (unsigned long)s, (unsigned long)last);
		ini.SetString(pcwd, pname, buf);
		totals.totalSeconds += credit;
		ini.SetInt("TOTAL", "SECONDS", (int)totals.totalSeconds);
		ini.SetString("PENDING", "PATH", "");
		ini.SetString("PENDING", "NAME", "");
		ini.SetInt("PENDING", "START", 0);
		dirty = true;
	}
	if (dirty) {
		ensureDir();
		ini.SaveIniFile(statsPath());
	}
	totals.valid = true;
}

void recordLaunch(const std::string &cwd, const std::string &name) {
	if (!sys().fatInitOk())
		return;
	const bool wasInit = initDone;
	init();
	const int64_t now = (int64_t)time(NULL);
	ensureDir();
	CIniFile ini(statsPath());
	// timesplayed.ini was already incremented by the caller: a count of 1 means this game is new
	CIniFile times(timesPath());
	const int count = times.GetInt(cwd, name);
	if (!(seededNow && !wasInit)) { // a history seeded just now already includes this launch
		totals.launches++;
		ini.SetInt("TOTAL", "LAUNCHES", (int)totals.launches);
		if (count <= 1) {
			totals.gamesPlayed++;
			ini.SetInt("TOTAL", "GAMES", (int)totals.gamesPlayed);
		}
	}
	games.clear();
	ini.SetString("PENDING", "PATH", cwd);
	ini.SetString("PENDING", "NAME", name);
	ini.SetInt("PENDING", "START", now >= nmformat::MIN_VALID_UNIX ? (int)now : 0);
	ini.SaveIniFile(statsPath());
}

const Global &global() { return totals; }

Game game(const std::string &cwd, const std::string &name) {
	Game g;
	if (!initDone || !sys().fatInitOk())
		return g;
	const std::string k = key(cwd, name);
	auto it = games.find(k);
	if (it == games.end()) {
		Entry e;
		CIniFile times(timesPath());
		e.launches = (uint32_t)times.GetInt(cwd, name);
		CIniFile ini(statsPath());
		nmformat::parsePair(ini.GetString(cwd, name, "").c_str(), e.seconds, e.last);
		if (games.size() > 64)
			games.clear();
		it = games.emplace(k, e).first;
	}
	g.launches = it->second.launches;
	g.seconds = it->second.seconds;
	g.lastPlayed = it->second.last;
	g.valid = true;
	return g;
}

} // namespace playstats
