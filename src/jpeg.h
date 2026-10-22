#pragma once

// jpeg: decodes a JPEG file into a bitmap, turned the way its EXIF orientation says.
// baseline and progressive Huffman coded images of 8 bits are read, grey or in colour
// (YCbCr, or RGB as Adobe marks it), with any sampling factors and restart intervals.
// arithmetic coded, lossless, hierarchical, 12-bit and CMYK images are not supported, and
// a file that ends early shows what was decoded.
#include "bitmap.h"
#include "strbuf.h"
#include "types.h"

// true when data starts with a JPEG start of image marker.
bool
JpegIs(const u8 *data, u64 size);

// decodes the size bytes of a JPEG file at data into bitmap, and appends what it is
// ("progressive, 4:2:0") to details.
// returns false, with bitmap left empty and the reason appended to message, when the file
// is damaged, of a kind that is not supported, or larger than maxImagePixels.
bool
JpegDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message);
