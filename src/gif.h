#pragma once

// gif: decodes the first frame of a GIF file into a bitmap the size of its screen.
// global and local palettes, a transparent colour and interlacing are read; the parts of
// the screen the frame does not cover are transparent.
// a frame whose data ends early is shown as far as it goes.
#include "bitmap.h"
#include "strbuf.h"
#include "types.h"

// true when data starts with a GIF signature.
bool
GifIs(const u8 *data, u64 size);

// decodes the size bytes of a GIF file at data into bitmap, and appends what it holds
// ("256 colours, interlaced, first of 12 frames") to details.
// returns false, with bitmap left empty and the reason appended to message, when the file
// has no frame or is larger than maxImagePixels.
bool
GifDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message);
