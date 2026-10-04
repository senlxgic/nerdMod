#ifndef NM_PHOTOS_H
#define NM_PHOTOS_H

#include <string>
#include <vector>

#include <nds/ndstypes.h>

// Photo storage for nerdMod Camera.
//
// Folder: <sd>:/_nds/nerdMod/photos/
// Names:  NM_YYYYMMDD_HHMMSS.bmp (a _N suffix is added if that name exists, so
//         an existing photo is never overwritten).
// Format: uncompressed 24-bit BMP, 640x480. Chosen over PNG because it needs no
//         encoder or large RGB buffer (rows are converted and written one at a
//         time, so a capture only needs the 600 KB YUV buffer), saving is
//         bounded by SD speed instead of CPU, and the gallery can read single
//         rows back without decoding a whole image.

enum PhotoResult {
	PHOTO_OK = 0,
	PHOTO_NO_STORAGE,  // SD card / folder not available
	PHOTO_NO_SPACE,    // the card ran out of space while writing (partial file removed)
	PHOTO_WRITE_FAILED // out of space or write error (partial file is removed)
};

// Creates the photo folder if needed. Returns false if it cannot be used.
bool photosEnsureFolder(void);

// Converts a 640x480 YUV422 capture (as delivered by the camera interface) to a
// BMP and stores it. On success outName receives the new file name.
PhotoResult photosSaveYuv(const u16 *yuv422, std::string &outName);

// Lists photo file names (NM_*.bmp), oldest first. Bounded to maxCount newest entries.
void photosList(std::vector<std::string> &names, size_t maxCount);

// Draws a photo scaled to 256x192 RGB555 (with bit 15 set) into dst.
// Returns false if the file is missing, corrupt or not one of our BMPs.
bool photosDrawScaled(const std::string &name, u16 *dst);

// Draws a raw capture (YUV422 640x480) scaled to 256x192 RGB555 into dst.
void photosDrawYuvScaled(const u16 *yuv422, u16 *dst);

bool photosDelete(const std::string &name);

// Full path of the photo folder (the folder may not exist yet).
std::string photosDirectory(void);

#endif
