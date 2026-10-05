// SD card access of the Weather app: settings and cache files under _nds/nerdMod/.
#pragma once

#include <string>

namespace store {

// Mounts the card (libnds fat) and picks "sd:" or "fat:". False when there is no usable card.
bool init();
const std::string &root();

std::string settingsPath();	// <root>/_nds/nerdMod/weather.ini
std::string cachePath();	// <root>/_nds/nerdMod/cache/weather/weather.ini

bool readFile(const std::string &path, std::string &out, size_t maxBytes);
// Creates the folders, writes <path>.tmp and renames it over <path> (a failed write never destroys the old file).
bool writeFile(const std::string &path, const char *data, size_t len);

} // namespace store
