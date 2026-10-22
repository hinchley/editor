#pragma once

// bitmap: the pixels of an image in memory, four bytes each (red, green, blue and alpha,
// not premultiplied), row after row from the top, and shrinking and turning them.
#include "types.h"

typedef struct Bitmap {
  u8 *pixels;
  u32 width;
  u32 height;

  // some pixel is not fully opaque.
  bool alpha;
} Bitmap;

// allocates the pixels of a width by height bitmap, all transparent black.
void
BitmapInit(Bitmap *bitmap, u32 width, u32 height);

void
BitmapFree(Bitmap *bitmap);

// makes scaled source shrunk to width by height, neither larger than source's, each pixel
// the average of the source pixels it covers, weighted by their alpha.
void
BitmapShrink(const Bitmap *source, u32 width, u32 height, Bitmap *scaled);

// turns the bitmap the way an EXIF orientation (1 to 8) says it was taken, so it shows the
// right way up: mirrored, turned a half or quarter turn, or both.
// 1, and any value outside 1 to 8, leaves it as it is.
void
BitmapOrient(Bitmap *bitmap, u32 orientation);
