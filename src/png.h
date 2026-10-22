#pragma once

// png: decodes a PNG file into a bitmap.
// every colour type and bit depth is read, with palettes, tRNS transparency and Adam7
// interlacing; an animated PNG gives its default image.
// the checksums are not verified, and ancillary chunks other than tRNS are ignored.
#include "bitmap.h"
#include "strbuf.h"
#include "types.h"

// true when data starts with the PNG signature.
bool
PngIs(const u8 *data, u64 size);

// decodes the size bytes of a PNG file at data into bitmap, and appends what its pixels
// are ("8-bit RGBA, interlaced") to details.
// returns false, with bitmap left empty and the reason appended to message, when the file
// is damaged or larger than maxImagePixels.
bool
PngDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message);
