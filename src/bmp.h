#pragma once

// bmp: decodes a Windows BMP file into a bitmap.
// the core, info and V2 to V5 headers are read, at 1, 4, 8, 16, 24 and 32 bits, with bit
// field masks (an alpha mask among them), and 4 and 8-bit run-length compression.
#include "bitmap.h"
#include "strbuf.h"
#include "types.h"

// true when data starts with the BMP signature.
bool
BmpIs(const u8 *data, u64 size);

// decodes the size bytes of a BMP file at data into bitmap, and appends what its pixels
// are ("8-bit palette") to details.
// returns false, with bitmap left empty and the reason appended to message, when the file
// is damaged or larger than maxImagePixels.
bool
BmpDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message);
