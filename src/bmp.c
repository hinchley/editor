#include "bmp.h"

#include <stdbit.h>
#include <string.h>

#include "settings.h"

// where the info header starts, after the file header.
static constexpr u64 infoStart = 14;

// the masks of 16 and 32-bit pixels without bit fields: 5 bits each of red, green and
// blue, or 8 each with the top 8 unused.
static const u32 masks16[4] = {0x7C00, 0x3E0, 0x1F, 0};
static const u32 masks32[4] = {0xFF'0000, 0xFF00, 0xFF, 0};

enum BmpCompression {
  BmpCompressionNone = 0,
  BmpCompressionRle8 = 1,
  BmpCompressionRle4 = 2,
  BmpCompressionBitFields = 3,
  BmpCompressionAlphaBitFields = 6
};

typedef struct Bmp {
  u32 width;
  u32 height;

  // the rows are stored from the top, rather than from the bottom.
  bool topDown;
  u16 depth;
  u32 compression;

  // the bits of a 16 or 32-bit pixel that hold red, green, blue and alpha (0 for none).
  u32 masks[4];

  // the palette of a 1, 4 or 8-bit image, in blue, green, red order, each entry stride
  // bytes long.
  const u8 *palette;
  u32 paletteCount;
  u32 stride;

  // where the pixels start.
  u64 offset;
} Bmp;

bool
BmpIs(const u8 *data, u64 size)
{
  return size >= 2 && data[0] == 'B' && data[1] == 'M';
}

static u16
ReadU16(const u8 *data)
{
  return (u16)(data[0] | data[1] << 8);
}

static u32
ReadU32(const u8 *data)
{
  return (u32)data[0] | (u32)data[1] << 8 | (u32)data[2] << 16 | (u32)data[3] << 24;
}

// reads the masks of a bit field image, which follow an info header and are part of the
// later ones.
static void
ReadMasks(Bmp *bmp, const u8 *data, u64 size, u32 headerSize)
{
  u32 count = bmp->compression == BmpCompressionAlphaBitFields || headerSize >= 56 ? 4 : 3;
  if (infoStart + 40 + count * 4 > size) return;

  for (u32 index = 0; index < count; index++) {
    bmp->masks[index] = ReadU32(data + infoStart + 40 + index * 4);
  }
}

static bool
ReadHeader(Bmp *bmp, const u8 *data, u64 size)
{
  if (size < infoStart + 12) return false;

  bmp->offset = ReadU32(data + 10);
  u32 headerSize = ReadU32(data + infoStart);
  bmp->stride = 4;
  u32 colorsUsed = 0;

  if (headerSize == 12) {
    bmp->width = ReadU16(data + 18);
    bmp->height = ReadU16(data + 20);
    bmp->depth = ReadU16(data + 24);
    bmp->stride = 3;
  } else {
    bool known = headerSize == 40 || headerSize == 52 || headerSize == 56 ||
      headerSize == 108 || headerSize == 124;
    if (!known || size < infoStart + 40) return false;

    i32 width = (i32)ReadU32(data + 18);
    i32 height = (i32)ReadU32(data + 22);
    if (width <= 0 || height == 0 || height == INT32_MIN) return false;

    bmp->width = (u32)width;
    bmp->topDown = height < 0;
    bmp->height = (u32)(height < 0 ? -height : height);
    bmp->depth = ReadU16(data + 28);
    bmp->compression = ReadU32(data + 30);
    colorsUsed = ReadU32(data + 46);
  }

  bool bitFields = bmp->compression == BmpCompressionBitFields ||
    bmp->compression == BmpCompressionAlphaBitFields;
  if (bitFields) ReadMasks(bmp, data, size, headerSize);

  // the masks of an info header follow it, before the palette.
  u64 palette = infoStart + headerSize;
  if (bitFields && headerSize == 40)
    palette += bmp->compression == BmpCompressionAlphaBitFields ? 16 : 12;

  if (bmp->depth <= 8 && palette < size) {
    u32 count = colorsUsed && colorsUsed < 256 ? colorsUsed : 1u << bmp->depth;
    u64 room = (size - palette) / bmp->stride;
    bmp->palette = data + palette;
    bmp->paletteCount = count < room ? count : (u32)room;
  }

  u16 depth = bmp->depth;
  return bmp->width && bmp->height &&
    (depth == 1 || depth == 4 || depth == 8 || depth == 16 || depth == 24 || depth == 32);
}

// one channel of a 16 or 32-bit pixel, from the bits its mask picks, as 8 bits.
static u8
Channel(u32 pixel, u32 mask)
{
  if (!mask) return 0;

  u32 shift = stdc_trailing_zeros(mask);
  u32 bits = stdc_count_ones(mask);
  u32 value = (pixel & mask) >> shift;
  if (bits >= 8) return (u8)(value >> (bits - 8));

  u32 largest = (1u << bits) - 1;
  return (u8)((value * 255 + largest / 2) / largest);
}

// writes a palette entry to out as an opaque pixel, black for an index past the palette.
static void
PutEntry(const Bmp *bmp, u32 index, u8 *out)
{
  out[3] = 255;
  if (index >= bmp->paletteCount) return;

  const u8 *entry = bmp->palette + index * bmp->stride;
  out[0] = entry[2];
  out[1] = entry[1];
  out[2] = entry[0];
}

// writes the pixels of a row, stored at in, to out.
static void
ConvertRow(const Bmp *bmp, const u8 *in, u8 *out)
{
  for (u32 x = 0; x < bmp->width; x++, out += 4) {
    if (bmp->depth <= 8) {
      u64 bit = (u64)x * bmp->depth;
      u32 index = (u32)(in[bit / 8] >> (8 - bmp->depth - bit % 8)) & ((1u << bmp->depth) - 1);
      PutEntry(bmp, index, out);
    } else if (bmp->depth == 24) {
      const u8 *pixel = in + (u64)x * 3;
      out[0] = pixel[2];
      out[1] = pixel[1];
      out[2] = pixel[0];
      out[3] = 255;
    } else {
      u32 pixel = bmp->depth == 16 ? ReadU16(in + (u64)x * 2) : ReadU32(in + (u64)x * 4);
      for (u32 channel = 0; channel < 3; channel++) {
        out[channel] = Channel(pixel, bmp->masks[channel]);
      }
      out[3] = bmp->masks[3] ? Channel(pixel, bmp->masks[3]) : 255;
    }
  }
}

// writes the palette entry of the pixel at x along the row-th row from the bottom, when it
// is inside the bitmap.
static void
PutRunPixel(const Bmp *bmp, u64 x, u64 row, u32 index, Bitmap *bitmap)
{
  if (x >= bmp->width || row >= bmp->height) return;

  u64 y = bmp->height - 1 - row;
  PutEntry(bmp, index, bitmap->pixels + (y * bmp->width + x) * 4);
}

// the index of the at-th pixel packed in bytes, two to a byte when four is set.
static u32
PackedIndex(const u8 *bytes, u32 at, bool four)
{
  if (!four) return bytes[at];
  return at % 2 ? bytes[at / 2] & 15u : (u32)bytes[at / 2] >> 4;
}

// decodes run-length compressed pixels of 4 or 8 bits, from the bottom row up.
// a run is a count and the index (or two indexes, alternating) to repeat; a count of 0
// starts an escape: the end of a row or of the image, a jump ahead, or a count of indexes
// given one by one, padded to an even number of bytes.
// pixels the runs pass over stay transparent, and damage ends the image where it starts.
static void
DecodeRuns(const Bmp *bmp, const u8 *data, u64 size, Bitmap *bitmap)
{
  bool four = bmp->compression == BmpCompressionRle4;
  u64 at = bmp->offset;
  u64 x = 0;
  u64 row = 0;

  while (at + 2 <= size && row < bmp->height) {
    u8 count = data[at];
    u8 value = data[at + 1];
    at += 2;

    if (count) {
      // a 4-bit run alternates the two indexes of its byte.
      for (u32 index = 0; index < count; index++) {
        u32 entry = four ? PackedIndex(&value, index % 2, true) : value;
        PutRunPixel(bmp, x++, row, entry, bitmap);
      }
    } else if (value == 0) {
      x = 0;
      row++;
    } else if (value == 1) {
      break;
    } else if (value == 2) {
      if (size - at < 2) break;
      x += data[at];
      row += data[at + 1];
      at += 2;
    } else {
      u64 bytes = four ? (value + 1u) / 2 : value;
      if (bytes > size - at) break;

      for (u32 index = 0; index < value; index++) {
        PutRunPixel(bmp, x++, row, PackedIndex(data + at, index, four), bitmap);
      }

      at += bytes + bytes % 2;
    }
  }
}

// sets the bitmap's alpha flag, or when every pixel is transparent, as when programs
// write no alpha into the alpha bits, makes them all opaque.
static void
SettleAlpha(Bitmap *bitmap)
{
  u64 count = (u64)bitmap->width * bitmap->height;
  bool visible = false;

  for (u64 index = 0; index < count; index++) {
    u8 alpha = bitmap->pixels[index * 4 + 3];
    if (alpha) visible = true;
    if (alpha != 255) bitmap->alpha = true;
  }

  if (visible) return;

  for (u64 index = 0; index < count; index++) bitmap->pixels[index * 4 + 3] = 255;
  bitmap->alpha = false;
}

static bool
DecodePixels(Bmp *bmp, const u8 *data, u64 size, Bitmap *bitmap, StrBuf *message)
{
  u32 compression = bmp->compression;
  u16 depth = bmp->depth;
  bool bitFields = compression == BmpCompressionBitFields ||
    compression == BmpCompressionAlphaBitFields;
  bool runs = (compression == BmpCompressionRle8 && depth == 8) ||
    (compression == BmpCompressionRle4 && depth == 4);
  bool direct = compression == BmpCompressionNone ||
    (bitFields && (depth == 16 || depth == 32));

  if (!(direct || runs) || (depth <= 8 && !bmp->paletteCount) || bmp->offset > size) {
    StrBufAppendString(message, "damaged BMP");
    return false;
  }

  if (runs) {
    BitmapInit(bitmap, bmp->width, bmp->height);
    DecodeRuns(bmp, data, size, bitmap);
    SettleAlpha(bitmap);
    return true;
  }

  if (!bitFields && depth == 16) memcpy(bmp->masks, masks16, sizeof(masks16));
  if (!bitFields && depth == 32) memcpy(bmp->masks, masks32, sizeof(masks32));

  // each row is padded to a whole number of 4-byte words, except perhaps the last.
  u64 rowBytes = ((u64)bmp->width * depth + 31) / 32 * 4;
  u64 lastBytes = ((u64)bmp->width * depth + 7) / 8;

  if (rowBytes * (bmp->height - 1) + lastBytes > size - bmp->offset) {
    StrBufAppendString(message, "damaged BMP");
    return false;
  }

  BitmapInit(bitmap, bmp->width, bmp->height);

  for (u32 row = 0; row < bmp->height; row++) {
    u32 y = bmp->topDown ? row : bmp->height - 1 - row;
    const u8 *in = data + bmp->offset + row * rowBytes;
    ConvertRow(bmp, in, bitmap->pixels + (u64)y * bmp->width * 4);
  }

  if (bmp->masks[3]) SettleAlpha(bitmap);
  return true;
}

bool
BmpDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message)
{
  *bitmap = (Bitmap){};
  Bmp bmp = {};

  if (!BmpIs(data, size) || !ReadHeader(&bmp, data, size)) {
    StrBufAppendString(message, "damaged BMP");
    return false;
  }

  if ((u64)bmp.width * bmp.height > maxImagePixels) {
    StrBufAppendFormat(message, "larger than %lu megapixels", maxImagePixels / 1'000'000);
    return false;
  }

  if (!DecodePixels(&bmp, data, size, bitmap, message)) return false;

  StrBufAppendFormat(details, "%u-bit", bmp.depth);
  if (bmp.depth <= 8) StrBufAppendString(details, " palette");
  if (bitmap->alpha) StrBufAppendString(details, " with alpha");
  return true;
}
