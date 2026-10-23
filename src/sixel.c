#include "sixel.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"

// the colours of the palette, and the index that marks a transparent pixel.
static constexpr u32 paletteSize = 255;
static constexpr u8 transparent = 255;

// colours are counted in bins of 5 bits a channel.
static constexpr u32 binCount = 1u << 15;
static constexpr u16 noEntry = UINT16_MAX;

// the pixels whose colour falls in one bin: how many, and the sum of each channel.
typedef struct Bin {
  u64 count;
  u64 sums[3];
} Bin;

// a box of colour space for median cut: the occupied bins from start to end of the sorted
// list, and their pixels.
typedef struct Box {
  u32 start;
  u32 end;
  u64 count;
} Box;

// the palette of one bitmap, and the entry each bin is drawn with (noEntry until needed).
typedef struct Palette {
  u8 colors[paletteSize][3];
  u32 count;
  u16 *entries;
} Palette;

static u32
BinOf(u32 red, u32 green, u32 blue)
{
  return (red >> 3) << 10 | (green >> 3) << 5 | blue >> 3;
}

// one channel of a bin, from 0 to 31.
static u32
BinChannel(u32 bin, u32 channel)
{
  return (bin >> (10 - channel * 5)) & 31;
}

static void
CountColors(const Bitmap *bitmap, Bin *bins)
{
  const u8 *pixel = bitmap->pixels;
  u64 count = (u64)bitmap->width * bitmap->height;

  for (u64 index = 0; index < count; index++, pixel += 4) {
    if (pixel[3] < 128) continue;

    Bin *bin = &bins[BinOf(pixel[0], pixel[1], pixel[2])];
    bin->count++;
    for (u32 channel = 0; channel < 3; channel++) bin->sums[channel] += pixel[channel];
  }
}

// the channel along which the box's bins spread furthest, and how far.
static u32
WidestChannel(const Box *box, const u16 *sorted, u32 *range)
{
  u32 widest = 0;
  *range = 0;

  for (u32 channel = 0; channel < 3; channel++) {
    u32 low = 31;
    u32 high = 0;

    for (u32 index = box->start; index < box->end; index++) {
      u32 value = BinChannel(sorted[index], channel);
      if (value < low) low = value;
      if (value > high) high = value;
    }

    if (high - low >= *range && high > low) {
      *range = high - low;
      widest = channel;
    }
  }

  return widest;
}

// sorts the box's bins by one channel, a counting sort since a channel has 32 values.
static void
SortBox(const Box *box, u16 *sorted, u32 channel, u16 *scratch)
{
  u32 starts[33] = {};

  for (u32 index = box->start; index < box->end; index++) {
    starts[BinChannel(sorted[index], channel) + 1]++;
  }

  for (u32 value = 0; value < 32; value++) starts[value + 1] += starts[value];

  for (u32 index = box->start; index < box->end; index++) {
    u16 bin = sorted[index];
    scratch[starts[BinChannel(bin, channel)]++] = bin;
  }

  memcpy(sorted + box->start, scratch, (box->end - box->start) * sizeof(u16));
}

// splits the box that most needs it, its pixels times its widest spread, at the median
// pixel along that spread, and returns false when no box can be split.
static bool
SplitBox(Box *boxes, u32 *boxCount, u16 *sorted, const Bin *bins, u16 *scratch)
{
  u32 chosen = UINT32_MAX;
  u32 chosenChannel = 0;
  u64 best = 0;

  for (u32 index = 0; index < *boxCount; index++) {
    u32 range;
    u32 channel = WidestChannel(&boxes[index], sorted, &range);
    u64 score = boxes[index].count * range;
    if (score <= best) continue;

    best = score;
    chosen = index;
    chosenChannel = channel;
  }

  if (chosen == UINT32_MAX) return false;

  Box *box = &boxes[chosen];
  SortBox(box, sorted, chosenChannel, scratch);

  // the first bin past half the pixels starts the second box, which is never empty.
  u64 half = box->count / 2;
  u64 seen = 0;
  u32 split = box->start + 1;

  for (u32 index = box->start; index < box->end - 1; index++) {
    seen += bins[sorted[index]].count;
    split = index + 1;
    if (seen >= half) break;
  }

  Box second = {split, box->end, box->count - seen};
  box->end = split;
  box->count = seen;
  boxes[(*boxCount)++] = second;
  return true;
}

// picks the palette by median cut, and points the bins it was picked from at their entry.
static void
ChoosePalette(const Bin *bins, Palette *palette)
{
  u16 *sorted = MemoryResize(nullptr, binCount, sizeof(u16));
  u16 *scratch = MemoryResize(nullptr, binCount, sizeof(u16));
  u32 occupied = 0;
  u64 pixels = 0;

  for (u32 bin = 0; bin < binCount; bin++) {
    if (!bins[bin].count) continue;
    sorted[occupied++] = (u16)bin;
    pixels += bins[bin].count;
  }

  Box boxes[paletteSize] = {{0, occupied, pixels}};
  u32 boxCount = occupied ? 1 : 0;

  while (boxCount < paletteSize && boxCount < occupied) {
    if (!SplitBox(boxes, &boxCount, sorted, bins, scratch)) break;
  }

  for (u32 index = 0; index < boxCount; index++) {
    u64 sums[3] = {};

    for (u32 at = boxes[index].start; at < boxes[index].end; at++) {
      const Bin *bin = &bins[sorted[at]];
      for (u32 channel = 0; channel < 3; channel++) sums[channel] += bin->sums[channel];
      palette->entries[sorted[at]] = (u16)index;
    }

    u64 count = boxes[index].count;
    for (u32 channel = 0; channel < 3; channel++) {
      palette->colors[index][channel] = (u8)((sums[channel] + count / 2) / count);
    }
  }

  palette->count = boxCount;
  free(sorted);
  free(scratch);
}

// the palette entry closest to a colour, found once for each bin.
static u8
EntryOf(Palette *palette, u32 red, u32 green, u32 blue)
{
  u32 bin = BinOf(red, green, blue);
  if (palette->entries[bin] != noEntry) return (u8)palette->entries[bin];

  u32 best = 0;
  i64 bestDistance = INT64_MAX;

  for (u32 index = 0; index < palette->count; index++) {
    const u8 *color = palette->colors[index];
    i64 distance = 0;

    for (u32 channel = 0; channel < 3; channel++) {
      i64 middle = (i64)(BinChannel(bin, channel) << 3 | 4);
      distance += (middle - color[channel]) * (middle - color[channel]);
    }

    if (distance >= bestDistance) continue;
    bestDistance = distance;
    best = index;
  }

  palette->entries[bin] = (u16)best;
  return (u8)best;
}

static i32
Clamp(i32 value)
{
  return value < 0 ? 0 : value > 255 ? 255 : value;
}

// gives each pixel its palette entry, or transparent, carrying each one's error onto the
// pixels not yet done.
// a bin of one exact colour has that colour as its entry, so its pixels carry no error,
// and an image of few colours stays crisp.
static void
MapPixels(const Bitmap *bitmap, Palette *palette, u8 *entries)
{
  u32 width = bitmap->width;

  // the error carried to this row and the next, in sixteenths, with a pixel spare at each
  // end.
  i32 *errors = MemoryAllocateZeroed(((u64)width + 2) * 6, sizeof(i32));
  i32 *current = errors;
  i32 *next = errors + ((u64)width + 2) * 3;

  for (u32 y = 0; y < bitmap->height; y++) {
    memset(next, 0, ((u64)width + 2) * 3 * sizeof(i32));

    for (u32 x = 0; x < width; x++) {
      u64 at = (u64)y * width + x;
      const u8 *pixel = bitmap->pixels + at * 4;

      if (pixel[3] < 128) {
        entries[at] = transparent;
        continue;
      }

      i32 *carried = current + ((u64)x + 1) * 3;
      i32 color[3];

      for (u32 channel = 0; channel < 3; channel++) {
        color[channel] = Clamp(pixel[channel] + carried[channel] / 16);
      }

      u8 entry = EntryOf(palette, (u32)color[0], (u32)color[1], (u32)color[2]);
      entries[at] = entry;

      for (u32 channel = 0; channel < 3; channel++) {
        i32 error = color[channel] - palette->colors[entry][channel];
        carried[channel + 3] += error * 7;
        next[(u64)x * 3 + channel] += error * 3;
        next[((u64)x + 1) * 3 + channel] += error * 5;
        next[((u64)x + 2) * 3 + channel] += error;
      }
    }

    i32 *done = current;
    current = next;
    next = done;
  }

  free(errors);
}

static void
AppendNumber(StrBuf *out, u64 number)
{
  char digits[20];
  u32 count = 0;

  do {
    digits[count++] = (char)('0' + number % 10);
    number /= 10;
  } while (number);

  while (count) StrBufAppendByte(out, digits[--count]);
}

// appends count copies of one sixel, as a repeat when that is shorter.
static void
AppendRun(StrBuf *out, u8 sixel, u32 count)
{
  char character = (char)('?' + sixel);

  if (count > 3) {
    StrBufAppendByte(out, '!');
    AppendNumber(out, count);
    StrBufAppendByte(out, character);
    return;
  }

  while (count--) StrBufAppendByte(out, character);
}

// appends the bands of six rows: for each colour in a band, the columns from its first
// pixel there to its last, as sixels of the rows it is drawn in.
static void
AppendBands(const Bitmap *bitmap, const u8 *entries, StrBuf *out)
{
  u32 width = bitmap->width;
  u8 *sixels = MemoryAllocateZeroed(paletteSize, width);
  u32 first[paletteSize];
  u32 last[paletteSize];

  // the colours drawn in the band, in the order they were met, and which those are.
  u8 used[paletteSize];
  bool present[paletteSize] = {};

  for (u32 top = 0; top < bitmap->height; top += 6) {
    u32 usedCount = 0;
    u32 rows = bitmap->height - top < 6 ? bitmap->height - top : 6;

    for (u32 row = 0; row < rows; row++) {
      const u8 *line = entries + (u64)(top + row) * width;

      for (u32 x = 0; x < width; x++) {
        u8 entry = line[x];
        if (entry == transparent) continue;

        if (!present[entry]) {
          present[entry] = true;
          used[usedCount++] = entry;
          first[entry] = last[entry] = x;
        }

        if (x < first[entry]) first[entry] = x;
        if (x > last[entry]) last[entry] = x;
        sixels[(u64)entry * width + x] |= (u8)(1 << row);
      }
    }

    if (top) StrBufAppendByte(out, '-');

    for (u32 index = 0; index < usedCount; index++) {
      u8 entry = used[index];
      u8 *sixel = &sixels[(u64)entry * width];

      // each colour goes back to the start of the band, except the first.
      if (index) StrBufAppendByte(out, '$');
      StrBufAppendByte(out, '#');
      AppendNumber(out, entry);

      // the columns before its first pixel are passed over with blank sixels.
      if (first[entry]) AppendRun(out, 0, first[entry]);

      u32 x = first[entry];

      while (x <= last[entry]) {
        u32 run = x + 1;
        while (run <= last[entry] && sixel[run] == sixel[x]) run++;
        AppendRun(out, sixel[x], run - x);
        x = run;
      }

      memset(sixel + first[entry], 0, last[entry] - first[entry] + 1);
      present[entry] = false;
    }
  }

  free(sixels);
}

void
SixelEncode(const Bitmap *bitmap, StrBuf *out)
{
  StrBufClear(out);

  Bin *bins = MemoryAllocateZeroed(binCount, sizeof(Bin));
  CountColors(bitmap, bins);

  Palette palette = {};
  palette.entries = MemoryResize(nullptr, binCount, sizeof(u16));
  memset(palette.entries, 0xFF, binCount * sizeof(u16));
  ChoosePalette(bins, &palette);
  free(bins);

  u8 *entries = MemoryResize(nullptr, (u64)bitmap->width * bitmap->height, 1);
  MapPixels(bitmap, &palette, entries);
  free(palette.entries);

  // a device control string: pixels left undrawn stay transparent (the 1), and the raster
  // attributes give square pixels and the size.
  StrBufAppendFormat(out, "\x1bP0;1;0q\"1;1;%u;%u", bitmap->width, bitmap->height);

  // the palette, in percentages.
  for (u32 index = 0; index < palette.count; index++) {
    const u8 *color = palette.colors[index];
    StrBufAppendFormat(
      out,
      "#%u;2;%u;%u;%u",
      index,
      (color[0] * 100 + 127) / 255,
      (color[1] * 100 + 127) / 255,
      (color[2] * 100 + 127) / 255
    );
  }

  AppendBands(bitmap, entries, out);
  StrBufAppendString(out, "\x1b\\");
  free(entries);
}
