#pragma once

// bitmap: the pixels of an image in memory, four bytes each (red, green, blue and alpha,
// not premultiplied), row after row from the top, and shrinking them.
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
