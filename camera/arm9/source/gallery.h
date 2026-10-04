// The album: photos (NM_*.bmp) and videos (NV_*.nvid) in one list, newest last, with thumbnails, details,
// playback and delete-with-confirmation.
#pragma once

#include <string>
#include <vector>

enum class AlbumExit { Back, PowerExit };

struct MediaItem {
	std::string name;
	bool video = false;
};

// All photos and videos, oldest first, bounded to the newest `maxCount`.
void galleryList(std::vector<MediaItem> &items, size_t maxCount);

AlbumExit galleryRun();
