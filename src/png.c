#include "png.h"

#include <string.h>

#include "inflate.h"
#include "settings.h"

static constexpr u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

enum PngColor {
  PngColorGrey = 0,
  PngColorRgb = 2,
  PngColorPalette = 3,
  PngColorGreyAlpha = 4,
  PngColorRgba = 6
};

// where each pass of an image's pixels starts, and how far apart its pixels are: the seven
// passes of Adam7, or one pass over every pixel.
typedef struct Pass {
  u32 x;
  u32 y;
  u32 stepX;
  u32 stepY;
} Pass;

static const Pass adam7[7] = {
  {0, 0, 8, 8},
  {4, 0, 8, 8},
  {0, 4, 4, 8},
  {2, 0, 4, 4},
  {0, 2, 2, 4},
  {1, 0, 2, 2},
  {0, 1, 1, 2}
};

static const Pass everyPixel = {0, 0, 1, 1};

// what the chunks say about the image, and its compressed pixels.
typedef struct Png {
  u32 width;
  u32 height;
  u8 depth;
  enum PngColor color;
  bool interlaced;

  // the samples of each pixel.
  u32 channels;

  // the palette, each entry with the alpha tRNS gives it (opaque without one).
  u8 palette[256][4];
  u32 paletteCount;

  // the sample values tRNS makes transparent in a grey or rgb image.
  bool keyed;
  u16 key[3];

  // every IDAT chunk's data, joined.
  StrBuf data;
} Png;

bool
PngIs(const u8 *data, u64 size)
{
  return size >= sizeof(signature) && memcmp(data, signature, sizeof(signature)) == 0;
}

static u32
ReadU32(const u8 *data)
{
  return (u32)data[0] << 24 | (u32)data[1] << 16 | (u32)data[2] << 8 | data[3];
}

static u16
ReadU16(const u8 *data)
{
  return (u16)(data[0] << 8 | data[1]);
}

static bool
ReadHeader(Png *png, const u8 *body, u64 length)
{
  if (length != 13) return false;

  png->width = ReadU32(body);
  png->height = ReadU32(body + 4);
  png->depth = body[8];
  png->color = body[9];
  png->interlaced = body[12] == 1;

  // compression, filtering and interlacing each have only the methods given here.
  if (!png->width || !png->height || body[10] || body[11] || body[12] > 1) return false;

  u8 depth = png->depth;
  bool power = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
  if (!power) return false;

  switch (png->color) {
    case PngColorGrey:
      png->channels = 1;
      return true;
    case PngColorPalette:
      png->channels = 1;
      return depth <= 8;
    case PngColorGreyAlpha:
      png->channels = 2;
      return depth >= 8;
    case PngColorRgb:
      png->channels = 3;
      return depth >= 8;
    case PngColorRgba:
      png->channels = 4;
      return depth >= 8;
    default:
      return false;
  }
}

static void
ReadPalette(Png *png, const u8 *body, u64 length)
{
  png->paletteCount = (u32)(length / 3 < 256 ? length / 3 : 256);

  for (u32 index = 0; index < png->paletteCount; index++) {
    memcpy(png->palette[index], body + index * 3, 3);
    png->palette[index][3] = 255;
  }
}

static void
ReadTransparency(Png *png, const u8 *body, u64 length)
{
  if (png->color == PngColorPalette) {
    for (u64 index = 0; index < length && index < 256; index++) {
      png->palette[index][3] = body[index];
    }
    return;
  }

  u32 count = png->color == PngColorGrey ? 1 : png->color == PngColorRgb ? 3 : 0;
  if (!count || length < count * 2) return;

  png->keyed = true;
  for (u32 index = 0; index < count; index++) png->key[index] = ReadU16(body + index * 2);
}

static bool
ReadChunks(Png *png, const u8 *data, u64 size)
{
  u64 at = sizeof(signature);
  bool header = false;

  // a chunk cut short by the end of the file ends the image there.
  while (at + 8 <= size) {
    u64 length = ReadU32(data + at);
    const u8 *type = data + at + 4;
    const u8 *body = data + at + 8;
    if (length > size - at - 8) break;

    // the chunk, then its checksum.
    at += 8 + length + 4;

    if (memcmp(type, "IHDR", 4) == 0) {
      if (header || !ReadHeader(png, body, length)) return false;
      header = true;
    } else if (!header) {
      return false;
    } else if (memcmp(type, "PLTE", 4) == 0) {
      ReadPalette(png, body, length);
    } else if (memcmp(type, "tRNS", 4) == 0) {
      ReadTransparency(png, body, length);
    } else if (memcmp(type, "IDAT", 4) == 0) {
      StrBufAppend(&png->data, (const char *)body, length);
    } else if (memcmp(type, "IEND", 4) == 0) {
      break;
    }
  }

  if (png->color == PngColorPalette && !png->paletteCount) return false;
  return header && png->data.length;
}

// the bytes of a row of width pixels, without the filter byte before it.
static u64
RowBytes(const Png *png, u32 width)
{
  return ((u64)width * png->channels * png->depth + 7) / 8;
}

// the pixels across and down a pass.
static void
PassSize(const Png *png, const Pass *pass, u32 *width, u32 *height)
{
  *width = png->width > pass->x ? (png->width - pass->x + pass->stepX - 1) / pass->stepX : 0;
  *height = png->height > pass->y ? (png->height - pass->y + pass->stepY - 1) / pass->stepY
                                  : 0;
}

// the bytes of a pass once inflated: each row with its filter byte, and none for an empty
// pass.
static u64
PassBytes(const Png *png, const Pass *pass)
{
  u32 width;
  u32 height;
  PassSize(png, pass, &width, &height);
  return width ? (u64)height * (1 + RowBytes(png, width)) : 0;
}

static u8
Paeth(u8 left, u8 up, u8 corner)
{
  i32 estimate = left + up - corner;
  i32 toLeft = estimate > left ? estimate - left : left - estimate;
  i32 toUp = estimate > up ? estimate - up : up - estimate;
  i32 toCorner = estimate > corner ? estimate - corner : corner - estimate;

  if (toLeft <= toUp && toLeft <= toCorner) return left;
  return toUp <= toCorner ? up : corner;
}

// undoes the filter of each of count rows, which are length bytes after their filter
// byte, with stride bytes from one pixel to the next.
// returns false on an unknown filter.
static bool
Unfilter(u8 *rows, u32 count, u64 length, u64 stride)
{
  const u8 *previous = nullptr;

  for (u32 row = 0; row < count; row++, rows += length + 1) {
    u8 filter = rows[0];
    u8 *line = rows + 1;

    for (u64 at = 0; at < length; at++) {
      u8 left = at >= stride ? line[at - stride] : 0;
      u8 up = previous ? previous[at] : 0;
      u8 corner = previous && at >= stride ? previous[at - stride] : 0;

      switch (filter) {
        case 0:
          break;
        case 1:
          line[at] = (u8)(line[at] + left);
          break;
        case 2:
          line[at] = (u8)(line[at] + up);
          break;
        case 3:
          line[at] = (u8)(line[at] + (left + up) / 2);
          break;
        case 4:
          line[at] = (u8)(line[at] + Paeth(left, up, corner));
          break;
        default:
          return false;
      }
    }

    previous = line;
  }

  return true;
}

// the sample at index along a row of samples of depth bits each.
static u32
Sample(const u8 *row, u64 index, u8 depth)
{
  if (depth == 16) return ReadU16(row + index * 2);
  if (depth == 8) return row[index];

  u64 bit = index * depth;
  return (u32)(row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
}

// a sample of depth bits as 8 bits.
static u8
Scale(u32 sample, u8 depth)
{
  if (depth == 16) return (u8)((sample * 255 + 32767) / 65535);
  return (u8)(sample * 255 / ((1u << depth) - 1));
}

// writes the count pixels of a row to out, step pixels apart, noting in alpha when one is
// not opaque.
static void
ConvertRow(const Png *png, const u8 *row, u32 count, u8 *out, u32 step, bool *alpha)
{
  u8 depth = png->depth;

  for (u32 x = 0; x < count; x++, out += step * 4) {
    u32 samples[4] = {};

    for (u32 channel = 0; channel < png->channels; channel++) {
      samples[channel] = Sample(row, (u64)x * png->channels + channel, depth);
    }

    switch (png->color) {
      case PngColorGrey: {
        u8 grey = Scale(samples[0], depth);
        bool clear = png->keyed && samples[0] == png->key[0];
        out[0] = out[1] = out[2] = grey;
        out[3] = clear ? 0 : 255;
        break;
      }
      case PngColorRgb: {
        bool clear = png->keyed && samples[0] == png->key[0] && samples[1] == png->key[1] &&
          samples[2] == png->key[2];
        for (u32 channel = 0; channel < 3; channel++)
          out[channel] = Scale(samples[channel], depth);
        out[3] = clear ? 0 : 255;
        break;
      }
      case PngColorPalette: {
        // an index past the palette is drawn opaque black.
        static const u8 black[4] = {0, 0, 0, 255};
        u32 index = samples[0];
        memcpy(out, index < png->paletteCount ? png->palette[index] : black, 4);
        break;
      }
      case PngColorGreyAlpha:
        out[0] = out[1] = out[2] = Scale(samples[0], depth);
        out[3] = Scale(samples[1], depth);
        break;
      case PngColorRgba:
        for (u32 channel = 0; channel < 4; channel++)
          out[channel] = Scale(samples[channel], depth);
        break;
    }

    if (out[3] != 255) *alpha = true;
  }
}

static const char *
ColorName(enum PngColor color)
{
  switch (color) {
    case PngColorGrey:
      return "grey";
    case PngColorRgb:
      return "RGB";
    case PngColorPalette:
      return "palette";
    case PngColorGreyAlpha:
      return "grey and alpha";
    case PngColorRgba:
      return "RGBA";
  }

  return "";
}

// inflates the pixels of a png whose chunks were read and puts them in bitmap.
static bool
DecodePixels(const Png *png, Bitmap *bitmap, StrBuf *message)
{
  u32 passCount = png->interlaced ? 7 : 1;
  const Pass *passes = png->interlaced ? adam7 : &everyPixel;
  u64 expected = 0;
  for (u32 index = 0; index < passCount; index++) expected += PassBytes(png, &passes[index]);

  StrBuf raw = {};
  StrBufReserve(&raw, expected);
  const u8 *compressed = (const u8 *)png->data.data;
  bool inflated = InflateZlib(compressed, png->data.length, expected, &raw);

  if (!inflated || raw.length != expected) {
    StrBufAppendString(message, "damaged PNG");
    StrBufFree(&raw);
    return false;
  }

  BitmapInit(bitmap, png->width, png->height);
  u8 *rows = (u8 *)raw.data;
  u64 stride = png->channels * png->depth >= 8 ? png->channels * png->depth / 8 : 1;
  bool unfiltered = true;

  for (u32 index = 0; index < passCount && unfiltered; index++) {
    const Pass *pass = &passes[index];
    u32 width;
    u32 height;
    PassSize(png, pass, &width, &height);
    if (!width || !height) continue;

    u64 length = RowBytes(png, width);
    unfiltered = Unfilter(rows, height, length, stride);

    for (u32 row = 0; row < height && unfiltered; row++) {
      u64 y = pass->y + (u64)row * pass->stepY;
      u8 *out = bitmap->pixels + (y * png->width + pass->x) * 4;
      ConvertRow(png, rows + row * (length + 1) + 1, width, out, pass->stepX, &bitmap->alpha);
    }

    rows += height * (length + 1);
  }

  StrBufFree(&raw);
  if (unfiltered) return true;

  StrBufAppendString(message, "damaged PNG");
  BitmapFree(bitmap);
  return false;
}

bool
PngDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message)
{
  *bitmap = (Bitmap){};
  Png png = {};
  bool decoded = false;

  if (!PngIs(data, size) || !ReadChunks(&png, data, size)) {
    StrBufAppendString(message, "damaged PNG");
  } else if ((u64)png.width * png.height > maxImagePixels) {
    StrBufAppendFormat(message, "larger than %lu megapixels", maxImagePixels / 1'000'000);
  } else {
    decoded = DecodePixels(&png, bitmap, message);
  }

  if (decoded) {
    StrBufAppendFormat(details, "%u-bit %s", png.depth, ColorName(png.color));
    if (png.interlaced) StrBufAppendString(details, ", interlaced");
  }

  StrBufFree(&png.data);
  return decoded;
}
