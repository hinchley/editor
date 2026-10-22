#include "bitmap.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"

void
BitmapInit(Bitmap *bitmap, u32 width, u32 height)
{
  *bitmap = (Bitmap){.width = width, .height = height};
  bitmap->pixels = MemoryAllocateZeroed((u64)width * height, 4);
}

void
BitmapFree(Bitmap *bitmap)
{
  free(bitmap->pixels);
  *bitmap = (Bitmap){};
}

void
BitmapShrink(const Bitmap *source, u32 width, u32 height, Bitmap *scaled)
{
  BitmapInit(scaled, width, height);
  scaled->alpha = source->alpha;

  // the first source column of each target column, and of the one after the last.
  u32 *starts = MemoryResize(nullptr, (u64)width + 1, sizeof(u32));
  for (u32 x = 0; x <= width; x++) starts[x] = (u32)((u64)x * source->width / width);

  u8 *out = scaled->pixels;

  for (u32 y = 0; y < height; y++) {
    u32 top = (u32)((u64)y * source->height / height);
    u32 bottom = (u32)((u64)(y + 1) * source->height / height);

    for (u32 x = 0; x < width; x++, out += 4) {
      // the colours are summed premultiplied, so transparent pixels add no colour.
      u64 sums[4] = {};

      for (u32 row = top; row < bottom; row++) {
        const u8 *in = source->pixels + ((u64)row * source->width + starts[x]) * 4;

        for (u32 column = starts[x]; column < starts[x + 1]; column++, in += 4) {
          sums[0] += (u64)in[0] * in[3];
          sums[1] += (u64)in[1] * in[3];
          sums[2] += (u64)in[2] * in[3];
          sums[3] += in[3];
        }
      }

      u64 count = (u64)(bottom - top) * (starts[x + 1] - starts[x]);
      out[3] = (u8)((sums[3] + count / 2) / count);
      if (!sums[3]) continue;

      for (u32 channel = 0; channel < 3; channel++) {
        out[channel] = (u8)((sums[channel] + sums[3] / 2) / sums[3]);
      }
    }
  }

  free(starts);
}

void
BitmapCrop(const Bitmap *source, u32 x, u32 y, u32 width, u32 height, Bitmap *part)
{
  BitmapInit(part, width, height);
  part->alpha = source->alpha;

  for (u32 row = 0; row < height; row++) {
    const u8 *from = source->pixels + ((u64)(y + row) * source->width + x) * 4;
    memcpy(part->pixels + (u64)row * width * 4, from, (u64)width * 4);
  }
}

void
BitmapFlatten(Bitmap *bitmap, u32 size)
{
  // the two greys of the checkerboard.
  static const u8 greys[2] = {0x3A, 0x4E};

  u8 *pixel = bitmap->pixels;

  for (u32 y = 0; y < bitmap->height; y++) {
    for (u32 x = 0; x < bitmap->width; x++, pixel += 4) {
      u32 grey = greys[(x / size + y / size) % 2];
      u32 alpha = pixel[3];

      for (u32 channel = 0; channel < 3; channel++) {
        pixel[channel] = (u8)((pixel[channel] * alpha + grey * (255 - alpha) + 127) / 255);
      }

      pixel[3] = 255;
    }
  }

  bitmap->alpha = false;
}

void
BitmapOrient(Bitmap *bitmap, u32 orientation)
{
  if (orientation < 2 || orientation > 8) return;

  // 5 to 8 swap the sides: the source's columns become rows.
  bool turned = orientation >= 5;
  u32 width = bitmap->width;
  u32 height = bitmap->height;
  Bitmap turnedBitmap;
  BitmapInit(&turnedBitmap, turned ? height : width, turned ? width : height);
  turnedBitmap.alpha = bitmap->alpha;

  for (u32 y = 0; y < turnedBitmap.height; y++) {
    for (u32 x = 0; x < turnedBitmap.width; x++) {
      // the source pixel each target pixel comes from.
      u32 column = x;
      u32 row = y;

      switch (orientation) {
        case 2:
          column = width - 1 - x;
          break;
        case 3:
          column = width - 1 - x;
          row = height - 1 - y;
          break;
        case 4:
          row = height - 1 - y;
          break;
        case 5:
          column = y;
          row = x;
          break;
        case 6:
          column = y;
          row = height - 1 - x;
          break;
        case 7:
          column = width - 1 - y;
          row = height - 1 - x;
          break;
        case 8:
          column = width - 1 - y;
          row = x;
          break;
      }

      const u8 *from = bitmap->pixels + ((u64)row * width + column) * 4;
      memcpy(turnedBitmap.pixels + ((u64)y * turnedBitmap.width + x) * 4, from, 4);
    }
  }

  BitmapFree(bitmap);
  *bitmap = turnedBitmap;
}
