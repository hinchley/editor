#include "gif.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "settings.h"

// codes are at most 12 bits, so the string table holds at most this many entries.
static constexpr u32 maxCodes = 4096;

// the first frame: where it sits on the screen, its palette, its transparent colour (or
// -1) and its compressed pixels.
typedef struct Frame {
  u32 left;
  u32 top;
  u32 width;
  u32 height;
  bool interlaced;
  const u8 *palette;
  u32 paletteCount;
  i32 transparent;
  u8 minimumSize;
  StrBuf codes;
} Frame;

typedef struct Gif {
  u32 width;
  u32 height;
  u32 frameCount;
  Frame frame;
} Gif;

// the string table of the codes being decoded: each string is a shorter one (its prefix)
// and a last index, and knows its length and its first index.
static u16 prefixes[maxCodes];
static u8 lasts[maxCodes];
static u8 firsts[maxCodes];
static u16 lengths[maxCodes];

bool
GifIs(const u8 *data, u64 size)
{
  return size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0);
}

static u16
ReadU16(const u8 *data)
{
  return (u16)(data[0] | data[1] << 8);
}

// passes over a run of data sub-blocks and the empty one that ends it, appending their
// data to out unless it is nullptr; a run cut short by the end of the file ends there.
static void
ReadSubBlocks(const u8 *data, u64 size, u64 *at, StrBuf *out)
{
  while (*at < size) {
    u64 length = data[(*at)++];
    if (!length) return;

    if (length > size - *at) length = size - *at;
    if (out) StrBufAppend(out, (const char *)data + *at, length);
    *at += length;
  }
}

// the colour table of 2 to 256 entries that flags announce, which starts at at; false when
// the file ends inside it.
static bool
ReadPalette(const u8 *data, u64 size, u64 *at, u8 flags, const u8 **palette, u32 *count)
{
  if (!(flags & 0x80)) return true;

  *count = 2u << (flags & 7);
  if (*count * 3 > size - *at) return false;

  *palette = data + *at;
  *at += *count * 3;
  return true;
}

// reads the screen, the first frame (with the transparent colour of the graphic control
// extension before it) and counts the frames after it.
static bool
ReadBlocks(Gif *gif, const u8 *data, u64 size)
{
  if (size < 13) return false;

  gif->width = ReadU16(data + 6);
  gif->height = ReadU16(data + 8);

  u64 at = 13;
  const u8 *globalPalette = nullptr;
  u32 globalCount = 0;
  if (!ReadPalette(data, size, &at, data[10], &globalPalette, &globalCount)) return false;

  Frame *frame = &gif->frame;
  i32 transparent = -1;

  // anything but an extension or a frame is the trailer or damage, and ends the file.
  while (at < size) {
    u8 kind = data[at++];

    if (kind == 0x21 && at < size) {
      u8 label = data[at++];
      bool control = label == 0xF9 && at + 5 <= size && data[at] >= 4;
      if (control) transparent = data[at + 1] & 1 ? data[at + 4] : -1;
      ReadSubBlocks(data, size, &at, nullptr);
      continue;
    }

    if (kind != 0x2C || size - at < 10) break;

    const u8 *descriptor = data + at;
    u8 flags = descriptor[8];
    at += 9;

    // later frames are only counted.
    if (gif->frameCount++) {
      const u8 *palette;
      u32 count;
      if (!ReadPalette(data, size, &at, flags, &palette, &count) || at >= size) break;

      at++;
      ReadSubBlocks(data, size, &at, nullptr);
      continue;
    }

    *frame = (Frame){
      .left = ReadU16(descriptor),
      .top = ReadU16(descriptor + 2),
      .width = ReadU16(descriptor + 4),
      .height = ReadU16(descriptor + 6),
      .interlaced = flags & 0x40,
      .palette = globalPalette,
      .paletteCount = globalCount,
      .transparent = transparent
    };

    if (!ReadPalette(data, size, &at, flags, &frame->palette, &frame->paletteCount)) {
      return false;
    }

    if (at >= size) return false;
    frame->minimumSize = data[at++];
    ReadSubBlocks(data, size, &at, &frame->codes);
  }

  // a screen of no size takes the first frame's.
  if (!gif->width || !gif->height) {
    gif->width = frame->left + frame->width;
    gif->height = frame->top + frame->height;
  }

  bool sized = frame->minimumSize >= 2 && frame->minimumSize <= 8;
  return gif->frameCount && frame->palette && sized && gif->width && gif->height;
}

// decodes the frame's codes into up to count palette indexes, and returns how many there
// were; damaged data ends the frame where it starts.
static u64
DecodeCodes(const Frame *frame, u8 *indexes, u64 count)
{
  u32 clear = 1u << frame->minimumSize;
  u32 end = clear + 1;

  for (u32 code = 0; code < clear; code++) {
    lasts[code] = firsts[code] = (u8)code;
    lengths[code] = 1;
  }

  const u8 *codes = (const u8 *)frame->codes.data;
  u64 codesLength = frame->codes.length;
  u64 at = 0;
  u32 bits = 0;
  u32 bitCount = 0;

  u32 codeSize = frame->minimumSize + 1u;
  u32 next = end + 1;
  u32 previous = UINT32_MAX;
  u64 produced = 0;

  while (produced < count) {
    while (bitCount < codeSize && at < codesLength) {
      bits |= (u32)codes[at++] << bitCount;
      bitCount += 8;
    }

    if (bitCount < codeSize) break;

    u32 code = bits & ((1u << codeSize) - 1);
    bits >>= codeSize;
    bitCount -= codeSize;

    if (code == clear) {
      codeSize = frame->minimumSize + 1u;
      next = end + 1;
      previous = UINT32_MAX;
      continue;
    }

    if (code == end) break;

    if (previous == UINT32_MAX) {
      if (code > clear) break;
    } else {
      // a code may only name the string about to be added, which is the previous one
      // and its own first index.
      if (code > next || (code == next && next == maxCodes)) break;

      if (next < maxCodes) {
        prefixes[next] = (u16)previous;
        lasts[next] = code < next ? firsts[code] : firsts[previous];
        firsts[next] = firsts[previous];
        lengths[next] = (u16)(lengths[previous] + 1);
        next++;
        if (next == 1u << codeSize && codeSize < 12) codeSize++;
      }
    }

    // the string is written from its end, along its prefixes, dropping what is past count.
    u64 length = lengths[code];
    u32 link = code;

    for (u64 index = length; index > 0; index--) {
      if (produced + index - 1 < count) indexes[produced + index - 1] = lasts[link];
      link = prefixes[link];
    }

    produced = produced + length < count ? produced + length : count;
    previous = code;
  }

  return produced;
}

// the row of the frame that the row-th row sent is, when the rows come in the four passes
// of interlacing.
static u32
InterlacedRow(u32 row, u32 height)
{
  static const u32 starts[4] = {0, 4, 2, 1};
  static const u32 steps[4] = {8, 8, 4, 2};

  for (u32 pass = 0; pass < 4; pass++) {
    u32 rows = height > starts[pass] ? (height - starts[pass] + steps[pass] - 1) / steps[pass]
                                     : 0;
    if (row < rows) return starts[pass] + row * steps[pass];
    row -= rows;
  }

  return row;
}

// draws the decoded indexes of the frame onto the screen-sized bitmap.
static void
DrawFrame(const Gif *gif, const u8 *indexes, u64 count, Bitmap *bitmap)
{
  const Frame *frame = &gif->frame;

  for (u64 index = 0; index < count; index++) {
    u32 row = (u32)(index / frame->width);
    if (frame->interlaced) row = InterlacedRow(row, frame->height);

    u64 x = frame->left + index % frame->width;
    u64 y = (u64)frame->top + row;
    u8 color = indexes[index];
    bool clear = color == frame->transparent || color >= frame->paletteCount;
    if (x >= gif->width || y >= gif->height || clear) continue;

    u8 *pixel = bitmap->pixels + (y * gif->width + x) * 4;
    memcpy(pixel, frame->palette + color * 3, 3);
    pixel[3] = 255;
  }

  const u8 *pixel = bitmap->pixels;
  u64 pixels = (u64)gif->width * gif->height;

  for (u64 index = 0; index < pixels && !bitmap->alpha; index++, pixel += 4) {
    bitmap->alpha = pixel[3] != 255;
  }
}

// true when the screen, or the frame, which need not fit on it, has too many pixels.
static bool
TooLarge(const Gif *gif)
{
  u64 screen = (u64)gif->width * gif->height;
  u64 frame = (u64)gif->frame.width * gif->frame.height;
  return screen > maxImagePixels || frame > maxImagePixels;
}

bool
GifDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message)
{
  *bitmap = (Bitmap){};
  Gif gif = {};
  bool decoded = false;

  if (!GifIs(data, size) || !ReadBlocks(&gif, data, size)) {
    StrBufAppendString(message, "damaged GIF");
  } else if (TooLarge(&gif)) {
    StrBufAppendFormat(message, "larger than %lu megapixels", maxImagePixels / 1'000'000);
  } else {
    const Frame *frame = &gif.frame;
    u64 count = (u64)frame->width * frame->height;
    u8 *indexes = MemoryAllocate(count);
    u64 produced = DecodeCodes(frame, indexes, count);

    BitmapInit(bitmap, gif.width, gif.height);
    DrawFrame(&gif, indexes, produced, bitmap);
    free(indexes);
    decoded = true;

    StrBufAppendFormat(details, "%u colours", frame->paletteCount);
    if (frame->interlaced) StrBufAppendString(details, ", interlaced");
    if (gif.frameCount > 1) {
      StrBufAppendFormat(details, ", first of %u frames", gif.frameCount);
    }
  }

  StrBufFree(&gif.frame.codes);
  return decoded;
}
