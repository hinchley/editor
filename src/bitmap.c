#include "bitmap.h"

#include <stdlib.h>

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
