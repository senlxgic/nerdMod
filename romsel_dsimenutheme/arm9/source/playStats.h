#pragma once
#ifndef NERDMOD_PLAY_STATS_H
#define NERDMOD_PLAY_STATS_H

// nerdMod play statistics.
//
// Launch counts are NOT stored again: they come from the existing TWiLight file extras/timesplayed.ini (the one the
// "most played" sort uses). nerdMod only adds what was missing, in sd:/_nds/nerdMod/playstats.ini:
//   [TOTAL]   SECONDS=<total play time>
//   [PENDING] PATH=<cwd> NAME=<file> START=<unix time>      a game that was launched and has not been settled yet
//   <cwd>     <file>=<seconds>,<last played unix time>        per game
//
// Play time is measured with the RTC: recordLaunch() leaves a pending record, init() settles it the next time the menu
// starts (a game cannot write anything itself). The session is only believed if it is plausible (see nmformat.h:
// no more than 12 h, clock not moved back or unset); otherwise the launch still counts but earns no time.

#include <stdint.h>
#include <string>

namespace playstats {

struct Global {
	uint32_t totalSeconds = 0;
	uint32_t launches = 0;		// sum of timesplayed.ini
	uint32_t gamesPlayed = 0;	// entries with at least one launch
	bool valid = false;
};

struct Game {
	uint32_t launches = 0;
	uint32_t seconds = 0;
	int64_t lastPlayed = 0;
	bool valid = false;
};

// Loads the totals and settles a pending launch. Cheap and safe to call more than once (works once per boot).
void init();
// Call right before a game is started (cwd + file name as used for timesplayed.ini).
void recordLaunch(const std::string &cwd, const std::string &name);

const Global &global();
// RAM lookups only; nothing here touches the card.
Game game(const std::string &cwd, const std::string &name);

} // namespace playstats

#endif
