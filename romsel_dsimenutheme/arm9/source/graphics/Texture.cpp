#include "Texture.h"
#include "dirIndex.h"
#include "paletteEffects.h"
#include "common/tonccpy.h"
#include "common/twlmenusettings.h"
#include "common/lodepng.h"
// #include "common/ColorLut.h"
#include <math.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern bool useTwlCfg;
extern u16* colorTable;

Texture::Texture(const std::string &filePath, const std::string &fallback1, const std::string &fallback2)
	: _paletteLength(0), _texLength(0), _texCmpLength(0), _texHeight(0), _texWidth(0), _type(TextureType::Unknown) {
	std::string pngPath;
	FILE *file = NULL;
	const std::string *paths[] = {&filePath, &fallback1, &fallback2};
	int i = 0;
	do {
		for (const char *extension : extensions) {
			const std::string candidate = *paths[i] + extension;
			if (!pathMayExist(candidate.c_str())) {
				file = NULL; // known not to exist on the SD card; same as a failed fopen()
				continue;
			}
			file = fopen(candidate.c_str(), "rb");
			if (file) {
				_type = findType(file);
				if (_type == TextureType::Unknown) {
					fclose(file);
				} else {
					pngPath = *paths[i] + extension;
					break;
				}
			}
		}
	} while (!file && ++i < 3);

	switch (_type) {
	case TextureType::PalettedGrf:
		nocashMessage("loading paletted");
		loadPaletted(file);
		break;
	case TextureType::Bmp:
	case TextureType::PalettedBmp:
		nocashMessage("loading bmp");
		loadBitmap(file);
		break;
	case TextureType::Png:
		nocashMessage("loading png");
		loadPNG(pngPath);
		break;
	case TextureType::CompressedGrf:
		nocashMessage("loading compressed");
		loadCompressed(file);
		break;
	case TextureType::Unknown:
	case TextureType::Bitmap: // ingore the bitfields
	case TextureType::Paletted:
	case TextureType::Compressed:
		break;
	}

	fclose(file);
}

TextureType Texture::findType(FILE *file) {

	fseek(file, 0, SEEK_SET);
	u32 magic[12] = {0};
	fread(&magic, sizeof(u32), 12, file);

	if (((u16)magic[0] & 0xffff) == BMP_ID('B', 'M')) {
		// Only 4 and 16 bit are supported currently
		if ((magic[7] & 0xFFFF) == 4)
			return TextureType::PalettedBmp;
		else if ((magic[7] & 0xFFFF) == 16)
			return TextureType::Bmp;
		return TextureType::Unknown;
	} else if (magic[0] == CHUNK_ID('\x89', 'P', 'N', 'G')) {
		return TextureType::Png;
	} else if (magic[0] == CHUNK_ID('R', 'I', 'F', 'F') && magic[2] == CHUNK_ID('G', 'R', 'F', ' ') &&
		magic[3] == CHUNK_ID('H', 'D', 'R', ' ') && magic[9] == CHUNK_ID('G', 'F', 'X', ' ')) {
		switch (magic[11] & 0xF0) {
		// may not be the case for general GRF, but for our case
		// We're going to assume all paletted textures are not compressed.
		case 0x00:
			return TextureType::PalettedGrf;
		// LZ77 Compressed
		case 0x10:
			return TextureType::CompressedGrf;
		default:
			return TextureType::Unknown;
		}
	}

	return TextureType::Unknown;
}

void Texture::loadBitmap(FILE *file) noexcept {
	// SKIP 'B' 'M' Idenifier
	fseek(file, sizeof(u16), SEEK_SET);

	u16 offset = 0, headerSize = 0;
	u16 bitDepth = 0;
	u32 texLength = 0;

	fseek(file, 2 * sizeof(u32), SEEK_CUR);
	fread(&offset, sizeof(u32), 1, file);
	fread(&headerSize, sizeof(u32), 1, file);

	fread(&_texWidth, sizeof(u32), 1, file);
	fread(&_texHeight, sizeof(u32), 1, file);

	fseek(file, sizeof(u16), SEEK_CUR);

	fread(&bitDepth, sizeof(u16), 1, file);

	fseek(file, sizeof(u32), SEEK_CUR);

	fread(&texLength, sizeof(u32), 1, file);

	_texLength = texLength >> 1;
	_texture = std::make_unique<u16[]>(_texLength);

	// Load palette
	if (bitDepth <= 8) {
		fseek(file, sizeof(u32) * 2, SEEK_CUR);
		u32 paletteLength;
		fread(&paletteLength, sizeof(u32), 1, file);
		if (paletteLength == 0)
			_paletteLength = 1 << bitDepth;
		else
			_paletteLength = paletteLength;

		fseek(file, 0xE + headerSize, SEEK_SET);

		_palette = std::make_unique<u16[]>(_paletteLength);

		for (int i = 0; i < _paletteLength; i++) {
			u32 color;
			fread(&color, sizeof(u32), 1, file);
			u8 r = lroundf(((color >> 16) & 0xFF) * 31 / 255.0f) & 0x1F;
			u8 g = lroundf(((color >> 8) & 0xFF) * 31 / 255.0f) & 0x1F;
			u8 b = lroundf((color & 0xFF) * 31 / 255.0f) & 0x1F;

			_palette[i] = BIT(15) | b << 10 | g << 5 | r;
			if (colorTable) {
				_palette[i] = colorTable[_palette[i] % 0x8000] | BIT(15);
			}
		}
	}

	for (uint y = 0; y < _texHeight; y++) {
		fseek(file, offset + ((_texHeight - y - 1) * _texWidth) * bitDepth / 8, SEEK_SET);
		fread((u8 *)_texture.get() + (y * _texWidth) * bitDepth / 8, 1, _texWidth * bitDepth / 8, file);

		if (bitDepth == 16) { // Filter
			for (uint x = 0; x < _texWidth; x++) {
				_texture[(y * _texWidth) + x] = bmpToDS(_texture[(y * _texWidth) + x]);
			}
		} else if (bitDepth == 4) { // Swap nibbles
			for (uint x = 0; x < _texWidth; x += 2) {
				u8 *px = (u8 *)_texture.get() + ((y * _texWidth) + x) / 2;
				*px = *px << 4 | *px >> 4;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Decoded-PNG cache (nerdMod)
//
// Decoding a full-screen PNG with lodepng on the DSi's ARM9 is slow, and every
// return from a game is a cold menu start that decodes the same theme PNGs
// again. We cache the *intermediate* RGB555+opaque-flag array (before
// bmpToDS(), so the user's colour LUT is never baked in) under
//   <dev>:/_nds/TWiLightMenu/cache/themebg/<fnv1a(path)>.nbg
//
// Safety:
//  - Entry is valid only if magic/version, source path hash, source file size
//    AND source mtime, dimensions and a payload checksum all match.
//    Replacing/editing a theme PNG changes size or mtime => re-decode.
//  - ANY problem (missing, short, corrupt, stat failure, write failure) falls
//    back to the original decode path. The cache is never required.
//  - Written to a temp name and renamed, so a half-written file is never read.
//  - Bounded: PNGs larger than 512 KB decoded, or tiny (<4096 px), are not
//    cached; when the directory holds >= 32 entries it is flushed first
//    (max ~3 MB at 96 KB per full-screen entry).
// ---------------------------------------------------------------------------
namespace {

constexpr u32 NBG_MAGIC = 0x4742424E; // "NBBG"
constexpr u32 NBG_VERSION = 1;
constexpr u32 NBG_MIN_PIXELS = 4096;
constexpr u32 NBG_MAX_PIXELS = 256 * 1024;
constexpr int NBG_MAX_ENTRIES = 32;

struct NbgHeader {
	u32 magic, version, width, height, srcSize, srcMtime, pathHash, checksum;
};

u32 nbgHashPath(const char *s) {
	u32 h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (u8)*s) * 16777619u;
	return h;
}

u32 nbgChecksum(const u16 *data, u32 count) {
	u32 sum = 0x811C9DC5u;
	for (u32 i = 0; i < count; i++)
		sum = ((sum << 5) | (sum >> 27)) ^ data[i];
	return sum;
}

// Builds "<dev>:/_nds/TWiLightMenu/cache/themebg" (dir) from an sd:/ or fat:/ path.
bool nbgCacheDir(const std::string &path, std::string &dir) {
	if (path.compare(0, 4, "sd:/") == 0)
		dir = "sd:/_nds/TWiLightMenu/cache/themebg";
	else if (path.compare(0, 5, "fat:/") == 0)
		dir = "fat:/_nds/TWiLightMenu/cache/themebg";
	else
		return false;
	return true;
}

void nbgFlushIfFull(const std::string &dir) {
	DIR *d = opendir(dir.c_str());
	if (!d)
		return;
	std::vector<std::string> files;
	while (dirent *e = readdir(d)) {
		const size_t len = strlen(e->d_name);
		if (len > 4 && strcasecmp(e->d_name + len - 4, ".nbg") == 0)
			files.emplace_back(e->d_name);
		else if (len > 4 && strcasecmp(e->d_name + len - 4, ".tmp") == 0)
			files.emplace_back(e->d_name);
	}
	closedir(d);
	if ((int)files.size() >= NBG_MAX_ENTRIES) {
		for (const std::string &f : files)
			unlink((dir + "/" + f).c_str());
	}
}

} // namespace

// Fills _texture with the intermediate array from the cache. Returns false (and
// leaves no state behind) on any mismatch.
bool Texture::loadPngFromCache(const std::string &cacheFile, u32 srcSize, u32 srcMtime, u32 pathHash) {
	FILE *f = fopen(cacheFile.c_str(), "rb");
	if (!f)
		return false;

	NbgHeader h;
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == NBG_MAGIC && h.version == NBG_VERSION && h.pathHash == pathHash &&
			  h.srcSize == srcSize && h.srcMtime == srcMtime && h.width > 0 && h.height > 0 && h.width <= 1024 && h.height <= 1024 &&
			  h.width * h.height >= NBG_MIN_PIXELS && h.width * h.height <= NBG_MAX_PIXELS;
	if (ok) {
		const u32 count = h.width * h.height;
		std::unique_ptr<u16[]> tex = std::make_unique<u16[]>(count);
		ok = fread(tex.get(), sizeof(u16), count, f) == count && nbgChecksum(tex.get(), count) == h.checksum;
		if (ok) {
			// Same conversion the PNG path always did, applied now so the
			// colour LUT in effect for THIS run is used.
			for (u32 i = 0; i < count; i++)
				tex[i] = (tex[i] & 0x8000) ? bmpToDS(tex[i] & 0x7FFF) : 0;
			_texWidth = h.width;
			_texHeight = h.height;
			_texLength = count;
			_texture = std::move(tex);
		}
	}
	fclose(f);
	return ok;
}

void Texture::loadPNG(const std::string &path) {
	std::string cacheDir, cacheFile, tmpFile;
	u32 srcSize = 0, srcMtime = 0, pathHash = 0;
	bool cacheable = false;

	struct stat st;
	if (nbgCacheDir(path, cacheDir) && stat(path.c_str(), &st) == 0 && st.st_size > 0) {
		srcSize = (u32)st.st_size;
		srcMtime = (u32)st.st_mtime;
		pathHash = nbgHashPath(path.c_str());
		char name[24];
		snprintf(name, sizeof(name), "/%08lx.nbg", (unsigned long)pathHash);
		cacheFile = cacheDir + name;
		tmpFile = cacheFile.substr(0, cacheFile.size() - 4) + ".tmp";
		cacheable = true;
		if (loadPngFromCache(cacheFile, srcSize, srcMtime, pathHash))
			return;
	}

	std::vector<unsigned char> buffer;
	unsigned width, height;
	const unsigned error = lodepng::decode(buffer, width, height, path);
	_texWidth = width;
	_texHeight = height;
	_texLength = _texWidth * _texHeight;

	// Convert to DS bitmap format
	_texture = std::make_unique<u16[]>(_texWidth * _texHeight);

	const bool storeCache = cacheable && error == 0 && buffer.size() == (size_t)_texLength * 4 && _texLength >= NBG_MIN_PIXELS &&
							_texLength <= NBG_MAX_PIXELS && width <= 1024 && height <= 1024;

	if (storeCache) {
		// Intermediate form: 0x8000 | RGB555 for opaque pixels, 0 otherwise.
		for (uint i = 0; i < _texLength; i++) {
			if (buffer[(i * 4) + 3] == 0xFF) {
				_texture[i] = 0x8000 | ((buffer[i * 4] >> 3) << 10 | (buffer[(i * 4) + 1] >> 3) << 5 | buffer[(i * 4) + 2] >> 3);
			}
		}

		bool written = false;
		nbgFlushIfFull(cacheDir);
		mkdir(cacheDir.substr(0, cacheDir.rfind('/')).c_str(), 0777); // .../cache
		mkdir(cacheDir.c_str(), 0777);						// .../cache/themebg
		FILE *f = fopen(tmpFile.c_str(), "wb");
		if (f) {
			NbgHeader h = {NBG_MAGIC, NBG_VERSION, width, height, srcSize, srcMtime, pathHash, nbgChecksum(_texture.get(), _texLength)};
			written = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(_texture.get(), sizeof(u16), _texLength, f) == _texLength;
			written = (fclose(f) == 0) && written;
			if (written) {
				unlink(cacheFile.c_str());
				written = rename(tmpFile.c_str(), cacheFile.c_str()) == 0;
			}
			if (!written)
				unlink(tmpFile.c_str());
		}

		for (uint i = 0; i < _texLength; i++)
			_texture[i] = (_texture[i] & 0x8000) ? bmpToDS(_texture[i] & 0x7FFF) : 0;
		return;
	}

	for (uint i=0;i<buffer.size()/4;i++) {
		if (buffer[(i * 4) + 3] == 0xFF) { // Only keep full opacity pixels
			_texture[i] = bmpToDS((buffer[i * 4] >> 3) << 10 | (buffer[(i * 4) + 1] >> 3) << 5 | buffer[(i * 4) + 2] >> 3);
		}
	}
}

void Texture::loadPaletted(FILE *file) noexcept {

	GrfHeader header;
	fseek(file, 5 * sizeof(u32), SEEK_SET);
	fread(&header, sizeof(GrfHeader), 1, file);

	_texHeight = header.texHeight;
	_texWidth = header.texWidth;

	// Skip 'G' 'F' 'X'[datasize]
	// todo: verify

	fseek(file, 2 * sizeof(u32), SEEK_CUR);
	u32 textureLengthInBytes = 0;
	fread(&textureLengthInBytes, sizeof(u32), 1, file);

	_texLength = (textureLengthInBytes >> 9); // palette length in ints sizeof(unsigned int);

	_texture = std::make_unique<u16[]>(_texLength);
	fread(_texture.get(), sizeof(u16), _texLength, file);

	// Skip 'P' 'A' 'L'[datasize]
	// todo: verify
	fseek(file, 2 * sizeof(u32), SEEK_CUR);
	u32 paletteLength = 0;
	fread(&paletteLength, sizeof(u32), 1, file);
	_paletteLength = (paletteLength >> 9); // palette length in shorts. / sizeof(unsighed shor)

	_palette = std::make_unique<u16[]>(_paletteLength);
	fread(_palette.get(), sizeof(u16), _paletteLength, file);
	effectColorModePalette(_palette.get(), _paletteLength);
}


void Texture::loadCompressed(FILE *file) noexcept {

	GrfHeader header;
	fseek(file, 5 * sizeof(u32), SEEK_SET);
	fread(&header, sizeof(GrfHeader), 1, file);

	_texHeight = header.texHeight;
	_texWidth = header.texWidth;

	// Skip 'G' 'F' 'X'[datasize]
	// todo: verify

	fseek(file, sizeof(u32), SEEK_CUR);
	// read chunk length
	fread(&_texCmpLength, sizeof(u32), 1, file); // this includes size of the header word

	_texture = std::make_unique<u16[]>(_texCmpLength >> 1);
	fread(_texture.get(), sizeof(u8), _texCmpLength, file);
	
	_texLength = (*((u32*)_texture.get()) >> 9); // palette length in ints sizeof(unsigned int);  
}

void Texture::applyPaletteEffect(Texture::PaletteEffect effect) {
	if (_type & TextureType::Paletted) {
		effect(_palette.get(), _paletteLength);
	}
}

void Texture::applyBitmapEffect(Texture::BitmapEffect effect) {
	if (_type & TextureType::Bitmap) {
		effect(_texture.get(), _texLength);
	}
}

void Texture::applyUserPaletteFile(const std::string &filePath, Texture::PaletteEffect fallbackEffect) {
	if (_type & TextureType::Paletted) {
		FILE *file = fopen(filePath.c_str(), "rb");
		if (file) {
			u16 *pal = _palette.get();
			int offset = ((useTwlCfg ? *(u8*)0x02000444 : PersonalData->theme) * _paletteLength);
			fseek(file, sizeof(u16) * offset, SEEK_SET);
			fread(pal, sizeof(u16), _paletteLength, file);
			fclose(file);
			// swap palette bytes
			for (int i = 0; i < _paletteLength; i++) {
				pal[i] = (pal[i] << 8 & 0xFF00) | pal[i] >> 8;
			}
		}
		else {
			fallbackEffect(_palette.get(), _paletteLength);
		}
	}
}

u16 Texture::bmpToDS(u16 val) {
	// Return 0 for #ff00ff
	if ((val & 0x7FFF) == 0x7C1F)
		return 0;

	val = ((val >> 10) & 31) | (val & (31 << 5)) | ((val & 31) << 10) | BIT(15);
	if (colorTable) {
		return colorTable[val % 0x8000] | BIT(15);
	}
	return val;
}

void Texture::copy(u16 *dst, bool vram) const {
	switch(_type) {
		case TextureType::PalettedGrf:
		case TextureType::PalettedBmp:
			for (u32 i = 0; i < _texWidth * _texHeight / 2; i++) {
				u8 byte = ((u8 *)_texture.get())[i];
				*(dst++) = _palette[byte & 0xF];
				*(dst++) = _palette[byte >> 4];
			}
			break;
		case TextureType::Bmp:
		case TextureType::Png:
			tonccpy(dst, _texture.get(), _texLength * sizeof(u16));
			break;
		case TextureType::CompressedGrf:
			decompress((u8 *)_texture.get(), (u8 *)dst, vram ? LZ77Vram : LZ77);
			effectColorModeBmpPalette(dst, _texLength);
			break;
		case TextureType::Unknown:
		case TextureType::Bitmap: // ingore the bitfields
		case TextureType::Paletted:
		case TextureType::Compressed:
			break;
	}
}
