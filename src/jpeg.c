#include "jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "settings.h"

// codes up to this many bits long are found with one table lookup.
static constexpr u32 fastBits = 9;

// the most components an image may have (grey, or three of colour), and tables of each kind.
static constexpr u32 maxComponents = 3;
static constexpr u32 maxTables = 4;

// the natural (row by row) position of each coefficient in zigzag order.
// clang-format off
static const u8 zigzag[64] = {
  0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

// the inverse DCT's basis: cosines[x][u] is cos((2x + 1)uπ / 16) times C(u) / 2, where
// C(0) is 1/√2 and C(u) is 1 otherwise, scaled by 4096.
static const i32 cosines[8][8] = {
  {1448,  2009,  1892,  1703,  1448,  1138,   784,   400},
  {1448,  1703,   784,  -400, -1448, -2009, -1892, -1138},
  {1448,  1138,  -784, -2009, -1448,   400,  1892,  1703},
  {1448,   400, -1892, -1138,  1448,  1703,  -784, -2009},
  {1448,  -400, -1892,  1138,  1448, -1703,  -784,  2009},
  {1448, -1138,  -784,  2009, -1448,  -400,  1892, -1703},
  {1448, -1703,   784,   400, -1448,  2009, -1892,  1138},
  {1448, -2009,  1892, -1703,  1448, -1138,   784,  -400}
};
// clang-format on

// a Huffman table: its symbols in code order, the symbol and length of each code of up to
// fastBits bits by the bits that start with it (length 0 for a longer code), and for each
// length the largest code and what turns a code into its symbol's index.
typedef struct Huffman {
  bool defined;
  u8 symbols[256];
  u8 fastSymbols[1 << fastBits];
  u8 fastLengths[1 << fastBits];
  i32 largest[17];
  i32 offsets[17];
} Huffman;

typedef struct Component {
  u8 id;
  u32 horizontal;
  u32 vertical;
  u8 quantTable;

  // the tables the scan being decoded gives it, and its last DC value there.
  u8 dcTable;
  u8 acTable;
  i32 prediction;

  // the blocks stored, a whole number of MCUs across and down, and those the image covers.
  u32 blocksWide;
  u32 blocksHigh;
  u32 usedWide;
  u32 usedHigh;

  // the samples across and down that the image covers.
  u32 sampleWidth;
  u32 sampleHeight;

  // 64 coefficients a block, in natural order, and once transformed the samples, eight
  // rows of eight a block, blocksWide * 8 to a row.
  i16 *coefficients;
  u8 *samples;
} Component;

// the bits of a scan's entropy coded data, read from the most significant end, with the 0
// stuffed after each 0xFF taken out; at a marker it gives zeros.
typedef struct Bits {
  const u8 *data;
  u64 size;
  u64 at;
  u64 buffer;
  u32 count;
  bool marker;
} Bits;

typedef struct Jpeg {
  u32 width;
  u32 height;
  bool framed;
  bool progressive;
  Component components[maxComponents];
  u32 componentCount;
  u32 maxHorizontal;
  u32 maxVertical;
  u32 mcusWide;
  u32 mcusHigh;

  u16 quant[maxTables][64];
  bool quantDefined[maxTables];
  Huffman dc[maxTables];
  Huffman ac[maxTables];
  u32 restartInterval;

  // an Adobe segment's colour transform (0 for RGB), when there was one.
  bool adobe;
  u8 transform;

  // the EXIF orientation, 1 to 8.
  u32 orientation;
  bool scanned;

  Bits bits;
  u32 endOfBands;
} Jpeg;

// the components of a scan, and the spectral band and successive approximation bits it
// codes.
typedef struct Scan {
  Component *components[maxComponents];
  u32 count;
  u32 start;
  u32 end;
  u32 high;
  u32 low;
} Scan;

bool
JpegIs(const u8 *data, u64 size)
{
  return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

static u16
ReadU16(const u8 *data)
{
  return (u16)(data[0] << 8 | data[1]);
}

static i16
Clamp16(i32 value)
{
  return (i16)(value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : value);
}

static u8
Clamp8(i64 value)
{
  return (u8)(value < 0 ? 0 : value > 255 ? 255 : value);
}

// builds a table from the count of codes of each length and the symbols in code order;
// false when the counts do not fit in their lengths.
static bool
BuildHuffman(Huffman *huffman, const u8 *counts, const u8 *symbols, u32 total)
{
  memcpy(huffman->symbols, symbols, total);
  memset(huffman->fastLengths, 0, sizeof(huffman->fastLengths));

  u32 code = 0;
  u32 index = 0;

  for (u32 length = 1; length <= 16; length++) {
    huffman->offsets[length] = (i32)index - (i32)code;

    for (u32 count = 0; count < counts[length - 1]; count++, code++, index++) {
      // more codes than the length has room for.
      if (code >= 1u << length) return false;
      if (length > fastBits) continue;

      u32 first = code << (fastBits - length);
      for (u32 fill = 0; fill < 1u << (fastBits - length); fill++) {
        huffman->fastSymbols[first + fill] = symbols[index];
        huffman->fastLengths[first + fill] = (u8)length;
      }
    }

    huffman->largest[length] = counts[length - 1] ? (i32)code - 1 : -1;
    code <<= 1;
  }

  huffman->defined = true;
  return true;
}

static bool
ReadHuffman(Jpeg *jpeg, const u8 *segment, u64 length)
{
  while (length >= 17) {
    u32 kind = segment[0] >> 4;
    u32 id = segment[0] & 15;
    const u8 *counts = segment + 1;
    u32 total = 0;
    for (u32 index = 0; index < 16; index++) total += counts[index];

    if (kind > 1 || id >= maxTables || total > 256 || 17 + total > length) return false;

    Huffman *huffman = kind ? &jpeg->ac[id] : &jpeg->dc[id];
    if (!BuildHuffman(huffman, counts, segment + 17, total)) return false;

    segment += 17 + total;
    length -= 17 + total;
  }

  return true;
}

static bool
ReadQuant(Jpeg *jpeg, const u8 *segment, u64 length)
{
  while (length >= 65) {
    bool wide = segment[0] >> 4;
    u32 id = segment[0] & 15;
    u64 size = wide ? 129 : 65;
    if (id >= maxTables || size > length) return false;

    for (u32 index = 0; index < 64; index++) {
      u16 value = wide ? ReadU16(segment + 1 + index * 2) : segment[1 + index];
      jpeg->quant[id][zigzag[index]] = value;
    }

    jpeg->quantDefined[id] = true;
    segment += size;
    length -= size;
  }

  return true;
}

static bool
ReadFrame(Jpeg *jpeg, const u8 *segment, u64 length, StrBuf *message)
{
  if (jpeg->framed || length < 6) return false;

  u32 count = segment[5];

  if (segment[0] != 8) {
    StrBufAppendFormat(message, "%u-bit JPEG is not supported", segment[0]);
    return false;
  }

  if (count == 4) {
    StrBufAppendString(message, "CMYK JPEG is not supported");
    return false;
  }

  jpeg->height = ReadU16(segment + 1);
  jpeg->width = ReadU16(segment + 3);
  jpeg->componentCount = count;
  bool counted = count == 1 || count == maxComponents;
  if (!counted || length < 6 + count * 3 || !jpeg->width || !jpeg->height) return false;

  if ((u64)jpeg->width * jpeg->height > maxImagePixels) {
    StrBufAppendFormat(message, "larger than %lu megapixels", maxImagePixels / 1'000'000);
    return false;
  }

  for (u32 index = 0; index < count; index++) {
    const u8 *entry = segment + 6 + index * 3;
    Component *component = &jpeg->components[index];
    component->id = entry[0];
    component->horizontal = entry[1] >> 4;
    component->vertical = entry[1] & 15;
    component->quantTable = entry[2];

    bool sampled = component->horizontal >= 1 && component->horizontal <= 4 &&
      component->vertical >= 1 && component->vertical <= 4;
    if (!sampled || component->quantTable >= maxTables) return false;

    if (component->horizontal > jpeg->maxHorizontal) {
      jpeg->maxHorizontal = component->horizontal;
    }

    if (component->vertical > jpeg->maxVertical) jpeg->maxVertical = component->vertical;
  }

  jpeg->mcusWide = (jpeg->width + 8 * jpeg->maxHorizontal - 1) / (8 * jpeg->maxHorizontal);
  jpeg->mcusHigh = (jpeg->height + 8 * jpeg->maxVertical - 1) / (8 * jpeg->maxVertical);

  for (u32 index = 0; index < count; index++) {
    Component *component = &jpeg->components[index];
    u64 wide = (u64)jpeg->width * component->horizontal;
    u64 high = (u64)jpeg->height * component->vertical;

    component->sampleWidth = (u32)((wide + jpeg->maxHorizontal - 1) / jpeg->maxHorizontal);
    component->sampleHeight = (u32)((high + jpeg->maxVertical - 1) / jpeg->maxVertical);
    component->usedWide = (component->sampleWidth + 7) / 8;
    component->usedHigh = (component->sampleHeight + 7) / 8;
    component->blocksWide = jpeg->mcusWide * component->horizontal;
    component->blocksHigh = jpeg->mcusHigh * component->vertical;

    u64 blocks = (u64)component->blocksWide * component->blocksHigh;
    component->coefficients = MemoryAllocateZeroed(blocks * 64, sizeof(i16));
  }

  jpeg->framed = true;
  return true;
}

// a 16 or 32-bit number in the byte order of an EXIF segment.
static u32
ReadExifNumber(const u8 *data, u32 bytes, bool little)
{
  u32 number = 0;

  for (u32 index = 0; index < bytes; index++) {
    u32 byte = data[little ? bytes - 1 - index : index];
    number = number << 8 | byte;
  }

  return number;
}

// reads the orientation from an EXIF segment's first directory, when it has one.
static void
ReadExif(Jpeg *jpeg, const u8 *segment, u64 length)
{
  if (length < 14 || memcmp(segment, "Exif\0\0", 6) != 0) return;

  const u8 *tiff = segment + 6;
  u64 size = length - 6;
  bool little = tiff[0] == 'I' && tiff[1] == 'I';
  if (!little && (tiff[0] != 'M' || tiff[1] != 'M')) return;

  u64 directory = ReadExifNumber(tiff + 4, 4, little);
  if (directory > size - 2) return;

  u32 count = ReadExifNumber(tiff + directory, 2, little);

  for (u32 index = 0; index < count; index++) {
    u64 at = directory + 2 + (u64)index * 12;
    if (at + 12 > size) return;

    const u8 *entry = tiff + at;
    if (ReadExifNumber(entry, 2, little) != 0x0112) continue;

    u32 orientation = ReadExifNumber(entry + 8, 2, little);
    if (orientation >= 1 && orientation <= 8) jpeg->orientation = orientation;
    return;
  }
}

static void
ReadAdobe(Jpeg *jpeg, const u8 *segment, u64 length)
{
  if (length < 12 || memcmp(segment, "Adobe", 5) != 0) return;

  jpeg->adobe = true;
  jpeg->transform = segment[11];
}

static Component *
FindComponent(Jpeg *jpeg, u8 id)
{
  for (u32 index = 0; index < jpeg->componentCount; index++) {
    if (jpeg->components[index].id == id) return &jpeg->components[index];
  }

  return nullptr;
}

static bool
ReadScan(Jpeg *jpeg, const u8 *segment, u64 length, Scan *scan)
{
  if (!jpeg->framed || length < 1) return false;

  *scan = (Scan){.count = segment[0]};
  if (!scan->count || scan->count > jpeg->componentCount) return false;
  if (length < 4 + scan->count * 2ull) return false;

  for (u32 index = 0; index < scan->count; index++) {
    const u8 *entry = segment + 1 + index * 2;
    Component *component = FindComponent(jpeg, entry[0]);
    if (!component) return false;

    component->dcTable = entry[1] >> 4;
    component->acTable = entry[1] & 15;
    if (component->dcTable >= maxTables || component->acTable >= maxTables) return false;
    if (!jpeg->quantDefined[component->quantTable]) return false;

    scan->components[index] = component;
  }

  const u8 *band = segment + 1 + scan->count * 2;
  scan->start = band[0];
  scan->end = band[1];
  scan->high = band[2] >> 4;
  scan->low = band[2] & 15;

  // a sequential scan codes every coefficient; a progressive one codes the DC alone (of any
  // components), or a band of AC coefficients of one component.
  if (!jpeg->progressive) {
    scan->start = 0;
    scan->end = 63;
    scan->high = scan->low = 0;
  } else if (scan->start == 0) {
    if (scan->end != 0) return false;
  } else if (scan->end < scan->start || scan->end > 63 || scan->count != 1) {
    return false;
  }

  if (scan->low > 13) return false;

  // the tables the scan uses must be there: a DC table unless only refining DC, and an AC
  // table for any AC band.
  for (u32 index = 0; index < scan->count; index++) {
    const Component *component = scan->components[index];
    bool needsDc = scan->start == 0 && scan->high == 0;
    bool needsAc = scan->end > 0;
    if (needsDc && !jpeg->dc[component->dcTable].defined) return false;
    if (needsAc && !jpeg->ac[component->acTable].defined) return false;
  }

  return true;
}

// tops the buffer up to more than 48 bits, with zeros once the data or the scan ends.
static void
Fill(Bits *bits)
{
  while (bits->count <= 48) {
    u8 byte = 0;

    if (!bits->marker && bits->at < bits->size) {
      byte = bits->data[bits->at];

      if (byte != 0xFF) {
        bits->at++;
      } else if (bits->at + 1 < bits->size && bits->data[bits->at + 1] == 0) {
        bits->at += 2;
      } else {
        bits->marker = true;
        byte = 0;
      }
    }

    bits->buffer = bits->buffer << 8 | byte;
    bits->count += 8;
  }
}

static u32
Peek(Bits *bits, u32 count)
{
  if (bits->count < count) Fill(bits);
  return (u32)(bits->buffer >> (bits->count - count)) & ((1u << count) - 1);
}

static u32
ReadBits(Bits *bits, u32 count)
{
  if (!count) return 0;

  u32 value = Peek(bits, count);
  bits->count -= count;
  return value;
}

// the signed value of count bits that follow a Huffman symbol giving that count, or 0 for
// a count no value has.
static i32
ReadValue(Bits *bits, u32 count)
{
  if (!count || count > 16) return 0;

  i32 value = (i32)ReadBits(bits, count);
  return value < 1 << (count - 1) ? value - (1 << count) + 1 : value;
}

// the next symbol, or 0 for a code the table does not have.
static u8
ReadSymbol(Bits *bits, const Huffman *huffman)
{
  u32 fast = Peek(bits, fastBits);

  if (huffman->fastLengths[fast]) {
    bits->count -= huffman->fastLengths[fast];
    return huffman->fastSymbols[fast];
  }

  for (u32 length = fastBits + 1; length <= 16; length++) {
    i32 code = (i32)Peek(bits, length);
    if (code > huffman->largest[length]) continue;

    bits->count -= length;
    return huffman->symbols[huffman->offsets[length] + code];
  }

  bits->count -= 16;
  return 0;
}

// the coefficients of a sequential scan's block.
static void
DecodeBlock(Jpeg *jpeg, Component *component, i16 *block)
{
  Bits *bits = &jpeg->bits;
  u32 size = ReadSymbol(bits, &jpeg->dc[component->dcTable]);
  component->prediction = Clamp16(component->prediction + ReadValue(bits, size));
  block[0] = Clamp16(component->prediction);

  const Huffman *ac = &jpeg->ac[component->acTable];

  for (u32 at = 1; at < 64; at++) {
    u8 symbol = ReadSymbol(bits, ac);
    u32 zeros = symbol >> 4;
    u32 length = symbol & 15;

    // 0x00 ends the block, and 0xF0 is a run of sixteen zeros.
    if (!length) {
      if (zeros != 15) return;

      at += 15;
      continue;
    }

    at += zeros;
    if (at > 63) return;

    block[zigzag[at]] = Clamp16(ReadValue(bits, length));
  }
}

// a progressive scan's first bits of the DC coefficient.
static void
DecodeDcFirst(Jpeg *jpeg, const Scan *scan, Component *component, i16 *block)
{
  Bits *bits = &jpeg->bits;
  u32 size = ReadSymbol(bits, &jpeg->dc[component->dcTable]);
  component->prediction = Clamp16(component->prediction + ReadValue(bits, size));
  block[0] = Clamp16(component->prediction * (1 << scan->low));
}

// a progressive scan's next bit of the DC coefficient.
static void
DecodeDcRefine(Jpeg *jpeg, const Scan *scan, i16 *block)
{
  if (ReadBits(&jpeg->bits, 1)) block[0] = (i16)(block[0] | 1 << scan->low);
}

// a progressive scan's first bits of a band of AC coefficients; a run of blocks with
// nothing in the band (an end of bands) is counted down in endOfBands.
static void
DecodeAcFirst(Jpeg *jpeg, const Scan *scan, Component *component, i16 *block)
{
  if (jpeg->endOfBands) {
    jpeg->endOfBands--;
    return;
  }

  Bits *bits = &jpeg->bits;
  const Huffman *ac = &jpeg->ac[component->acTable];

  for (u32 at = scan->start; at <= scan->end; at++) {
    u8 symbol = ReadSymbol(bits, ac);
    u32 zeros = symbol >> 4;
    u32 length = symbol & 15;

    if (!length) {
      if (zeros == 15) {
        at += 15;
        continue;
      }

      // this block ends the run, so the run counts the blocks after it.
      jpeg->endOfBands = (1u << zeros) + ReadBits(bits, zeros) - 1;
      return;
    }

    at += zeros;
    if (at > 63) return;

    block[zigzag[at]] = Clamp16(ReadValue(bits, length) * (1 << scan->low));
  }
}

// adds the next bit of a coefficient that is already not zero, away from zero.
static void
RefineCoefficient(Bits *bits, i16 *coefficient, i32 bit)
{
  if (!ReadBits(bits, 1) || (*coefficient & bit)) return;

  *coefficient = Clamp16(*coefficient + (*coefficient >= 0 ? bit : -bit));
}

// a progressive scan's next bit of a band of AC coefficients: each coefficient that is not
// zero yet takes a bit, and new ones of ±1 at this bit appear after runs of zeros, as
// libjpeg's decode_mcu_AC_refine reads them.
static void
DecodeAcRefine(Jpeg *jpeg, const Scan *scan, Component *component, i16 *block)
{
  Bits *bits = &jpeg->bits;
  const Huffman *ac = &jpeg->ac[component->acTable];
  i32 bit = 1 << scan->low;
  u32 at = scan->start;

  if (!jpeg->endOfBands) {
    for (; at <= scan->end; at++) {
      u8 symbol = ReadSymbol(bits, ac);
      u32 zeros = symbol >> 4;
      i32 value = 0;

      if (symbol & 15) {
        value = ReadBits(bits, 1) ? bit : -bit;
      } else if (zeros != 15) {
        // the rest of this block's band, and the blocks of the run after it, only refine.
        jpeg->endOfBands = (1u << zeros) + ReadBits(bits, zeros);
        break;
      }

      // passes over the zeros of the run, refining the coefficients that are not zero on
      // the way, and puts the new coefficient at the first zero after them.
      for (; at <= scan->end; at++) {
        i16 *coefficient = &block[zigzag[at]];

        if (*coefficient) {
          RefineCoefficient(bits, coefficient, bit);
        } else if (zeros) {
          zeros--;
        } else {
          if (value) *coefficient = (i16)value;
          break;
        }
      }
    }
  }

  if (!jpeg->endOfBands) return;

  for (; at <= scan->end; at++) {
    i16 *coefficient = &block[zigzag[at]];
    if (*coefficient) RefineCoefficient(bits, coefficient, bit);
  }

  jpeg->endOfBands--;
}

static void
DecodeAny(Jpeg *jpeg, const Scan *scan, Component *component, u32 column, u32 row)
{
  i16 *block = component->coefficients + ((u64)row * component->blocksWide + column) * 64;

  if (!jpeg->progressive) DecodeBlock(jpeg, component, block);
  else if (scan->start == 0 && scan->high == 0) DecodeDcFirst(jpeg, scan, component, block);
  else if (scan->start == 0) DecodeDcRefine(jpeg, scan, block);
  else if (scan->high == 0) DecodeAcFirst(jpeg, scan, component, block);
  else DecodeAcRefine(jpeg, scan, component, block);
}

// starts again after a restart marker: the bits left in the buffer were padding, and the
// predictions and run of ends of bands start over.
static void
Restart(Jpeg *jpeg, const Scan *scan)
{
  Bits *bits = &jpeg->bits;
  bits->buffer = 0;
  bits->count = 0;
  bits->marker = false;

  bool restart = bits->at + 1 < bits->size && bits->data[bits->at] == 0xFF &&
    (bits->data[bits->at + 1] & 0xF8) == 0xD0;
  if (restart) bits->at += 2;

  for (u32 index = 0; index < scan->count; index++) scan->components[index]->prediction = 0;
  jpeg->endOfBands = 0;
}

// decodes a scan's data, which starts at at, MCU by MCU: one block of the component when it
// has the scan to itself, or every block of each component in an MCU otherwise.
static void
DecodeScan(Jpeg *jpeg, const Scan *scan, const u8 *data, u64 size, u64 at)
{
  jpeg->bits = (Bits){.data = data, .size = size, .at = at};
  jpeg->endOfBands = 0;
  for (u32 index = 0; index < scan->count; index++) scan->components[index]->prediction = 0;

  u32 interval = jpeg->restartInterval;
  u64 done = 0;

  if (scan->count == 1) {
    Component *component = scan->components[0];

    for (u32 row = 0; row < component->usedHigh; row++) {
      for (u32 column = 0; column < component->usedWide; column++, done++) {
        if (interval && done && done % interval == 0) Restart(jpeg, scan);
        DecodeAny(jpeg, scan, component, column, row);
      }
    }

    return;
  }

  for (u32 mcuRow = 0; mcuRow < jpeg->mcusHigh; mcuRow++) {
    for (u32 mcuColumn = 0; mcuColumn < jpeg->mcusWide; mcuColumn++, done++) {
      if (interval && done && done % interval == 0) Restart(jpeg, scan);

      for (u32 index = 0; index < scan->count; index++) {
        Component *component = scan->components[index];

        for (u32 y = 0; y < component->vertical; y++) {
          for (u32 x = 0; x < component->horizontal; x++) {
            u32 column = mcuColumn * component->horizontal + x;
            u32 row = mcuRow * component->vertical + y;
            DecodeAny(jpeg, scan, component, column, row);
          }
        }
      }
    }
  }
}

// where the marker after a scan's data starts: the first 0xFF not stuffed, nor a restart
// marker, nor fill before a marker.
static u64
SkipScanData(const u8 *data, u64 size, u64 at)
{
  for (; at + 1 < size; at++) {
    if (data[at] != 0xFF) continue;

    u8 next = data[at + 1];
    bool inside = next == 0 || next == 0xFF || (next & 0xF8) == 0xD0;
    if (!inside) return at;
  }

  return size;
}

// dequantises a block and turns its coefficients into samples, written eight rows of eight
// from out, stride bytes apart.
static void
TransformBlock(const i16 *block, const u16 *quant, u8 *out, u64 stride)
{
  i32 coefficients[64];
  bool flat = true;

  for (u32 index = 0; index < 64; index++) {
    coefficients[index] = Clamp16(block[index] * quant[index]);
    if (index && coefficients[index]) flat = false;
  }

  // a block of only DC is one value throughout.
  if (flat) {
    i64 sum = (i64)coefficients[0] * cosines[0][0] * cosines[0][0];
    u8 value = Clamp8(((sum + (1 << 23)) >> 24) + 128);
    for (u32 row = 0; row < 8; row++) memset(out + row * stride, value, 8);
    return;
  }

  // the rows first, keeping the scale of 4096, then the columns.
  i32 rows[64];

  for (u32 v = 0; v < 8; v++) {
    for (u32 x = 0; x < 8; x++) {
      i32 sum = 0;
      for (u32 u = 0; u < 8; u++) sum += cosines[x][u] * coefficients[v * 8 + u];
      rows[v * 8 + x] = sum;
    }
  }

  for (u32 y = 0; y < 8; y++) {
    for (u32 x = 0; x < 8; x++) {
      i64 sum = 0;
      for (u32 v = 0; v < 8; v++) sum += (i64)cosines[y][v] * rows[v * 8 + x];
      out[y * stride + x] = Clamp8(((sum + (1 << 23)) >> 24) + 128);
    }
  }
}

static void
TransformComponent(const Jpeg *jpeg, Component *component)
{
  u64 stride = (u64)component->blocksWide * 8;
  component->samples = MemoryResize(nullptr, stride * component->blocksHigh * 8, 1);
  const u16 *quant = jpeg->quant[component->quantTable];

  for (u32 row = 0; row < component->blocksHigh; row++) {
    for (u32 column = 0; column < component->blocksWide; column++) {
      u64 index = (u64)row * component->blocksWide + column;
      u8 *out = component->samples + row * 8 * stride + column * 8;
      TransformBlock(component->coefficients + index * 64, quant, out, stride);
    }
  }
}

// a component's samples for row y of the image: its own row at full resolution, or else
// interpolated between its nearest samples, centred as libjpeg's fancy upsampling does.
static void
SampleRow(const Jpeg *jpeg, const Component *component, u32 y, u8 *out)
{
  u64 stride = (u64)component->blocksWide * 8;
  u32 horizontal = component->horizontal;
  u32 vertical = component->vertical;

  if (horizontal == jpeg->maxHorizontal && vertical == jpeg->maxVertical) {
    memcpy(out, component->samples + y * stride, jpeg->width);
    return;
  }

  // positions in the component, in units of 1 / (2 * max) of a sample.
  i64 across = 2 * jpeg->maxHorizontal;
  i64 down = 2 * jpeg->maxVertical;
  i64 position = (2 * (i64)y + 1) * vertical - jpeg->maxVertical;
  if (position < 0) position = 0;

  u32 top = (u32)(position / down);
  i64 lower = position % down;
  u32 bottom = top + 1 < component->sampleHeight ? top + 1 : top;
  const u8 *upper = component->samples + top * stride;
  const u8 *under = component->samples + bottom * stride;

  for (u32 x = 0; x < jpeg->width; x++) {
    i64 spot = (2 * (i64)x + 1) * horizontal - jpeg->maxHorizontal;
    if (spot < 0) spot = 0;

    u32 left = (u32)(spot / across);
    i64 right = spot % across;
    u32 next = left + 1 < component->sampleWidth ? left + 1 : left;

    i64 above = upper[left] * (across - right) + upper[next] * right;
    i64 below = under[left] * (across - right) + under[next] * right;
    i64 scale = across * down;
    out[x] = (u8)((above * (down - lower) + below * lower + scale / 2) / scale);
  }
}

// converts the components to the bitmap: grey, or RGB from YCbCr unless the file says it is
// RGB already.
static void
ConvertColors(const Jpeg *jpeg, Bitmap *bitmap)
{
  BitmapInit(bitmap, jpeg->width, jpeg->height);
  u8 *rows = MemoryResize(nullptr, (u64)jpeg->width * jpeg->componentCount, 1);

  const Component *components = jpeg->components;
  bool rgb = (jpeg->adobe && jpeg->transform == 0) ||
    (components[0].id == 'R' && components[1].id == 'G' && components[2].id == 'B');

  for (u32 y = 0; y < jpeg->height; y++) {
    for (u32 index = 0; index < jpeg->componentCount; index++) {
      SampleRow(jpeg, &components[index], y, rows + (u64)index * jpeg->width);
    }

    u8 *out = bitmap->pixels + (u64)y * jpeg->width * 4;

    for (u32 x = 0; x < jpeg->width; x++, out += 4) {
      out[3] = 255;

      if (jpeg->componentCount == 1) {
        out[0] = out[1] = out[2] = rows[x];
        continue;
      }

      i32 luma = rows[x];
      i32 blue = rows[jpeg->width + x];
      i32 red = rows[2 * (u64)jpeg->width + x];

      if (rgb) {
        out[0] = (u8)luma;
        out[1] = (u8)blue;
        out[2] = (u8)red;
        continue;
      }

      // the JFIF conversion, with its factors scaled by 65536.
      blue -= 128;
      red -= 128;
      i64 scaled = (i64)luma << 16;
      out[0] = Clamp8((scaled + 91'881 * red + 32'768) >> 16);
      out[1] = Clamp8((scaled - 22'554 * blue - 46'802 * red + 32'768) >> 16);
      out[2] = Clamp8((scaled + 116'130 * blue + 32'768) >> 16);
    }
  }

  free(rows);
}

// reads the segments up to the end of the image, decoding each scan.
static bool
ReadSegments(Jpeg *jpeg, const u8 *data, u64 size, StrBuf *message)
{
  u64 at = 2;

  while (at + 1 < size) {
    // fill bytes may come before a marker.
    if (data[at] != 0xFF) return jpeg->scanned;
    while (at + 1 < size && data[at + 1] == 0xFF) at++;
    if (at + 1 >= size) break;

    u8 marker = data[at + 1];
    at += 2;
    if (marker == 0xD9) break;
    if ((marker & 0xF8) == 0xD0 || marker == 0x01) continue;

    // a segment cut short by the end of the file ends the image there.
    if (at + 2 > size) break;
    u64 length = ReadU16(data + at);
    if (length < 2 || length > size - at) break;

    const u8 *segment = data + at + 2;
    length -= 2;
    at += length + 2;
    bool read = true;

    switch (marker) {
      case 0xC0:
      case 0xC1:
      case 0xC2:
        jpeg->progressive = marker == 0xC2;
        read = ReadFrame(jpeg, segment, length, message);
        break;
      case 0xC3:
      case 0xC5:
      case 0xC6:
      case 0xC7:
      case 0xC9:
      case 0xCA:
      case 0xCB:
      case 0xCD:
      case 0xCE:
      case 0xCF:
        StrBufAppendString(
          message,
          "lossless, hierarchical or arithmetic coded JPEG is not supported"
        );
        return false;
      case 0xC4:
        read = ReadHuffman(jpeg, segment, length);
        break;
      case 0xDB:
        read = ReadQuant(jpeg, segment, length);
        break;
      case 0xDD:
        jpeg->restartInterval = length >= 2 ? ReadU16(segment) : 0;
        break;
      case 0xE1:
        ReadExif(jpeg, segment, length);
        break;
      case 0xEE:
        ReadAdobe(jpeg, segment, length);
        break;
      case 0xDA: {
        Scan scan;
        read = ReadScan(jpeg, segment, length, &scan);
        if (!read) break;

        DecodeScan(jpeg, &scan, data, size, at);
        jpeg->scanned = true;
        at = SkipScanData(data, size, jpeg->bits.at);
        break;
      }
    }

    // a damaged segment after a scan leaves what was decoded.
    if (!read) return jpeg->scanned;
  }

  return jpeg->scanned;
}

// appends how the image is coded and its chroma sampling ("progressive, 4:2:0").
static void
Describe(const Jpeg *jpeg, StrBuf *details)
{
  StrBufAppendString(details, jpeg->progressive ? "progressive" : "baseline");

  if (jpeg->componentCount == 1) {
    StrBufAppendString(details, ", grey");
    return;
  }

  const Component *luma = &jpeg->components[0];
  const Component *chroma = &jpeg->components[1];
  bool even = chroma->horizontal == 1 && chroma->vertical == 1;
  u32 across = luma->horizontal;
  u32 down = luma->vertical;

  if (even && across == 1 && down == 1) StrBufAppendString(details, ", 4:4:4");
  else if (even && across == 2 && down == 2) StrBufAppendString(details, ", 4:2:0");
  else if (even && across == 2 && down == 1) StrBufAppendString(details, ", 4:2:2");
  else if (even && across == 1 && down == 2) StrBufAppendString(details, ", 4:4:0");
  else if (even && across == 4 && down == 1) StrBufAppendString(details, ", 4:1:1");
}

static void
FreeJpeg(Jpeg *jpeg)
{
  for (u32 index = 0; index < maxComponents; index++) {
    free(jpeg->components[index].coefficients);
    free(jpeg->components[index].samples);
  }

  free(jpeg);
}

bool
JpegDecode(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message)
{
  *bitmap = (Bitmap){};

  // the tables make it too large to keep on the stack.
  Jpeg *jpeg = MemoryAllocateZeroed(1, sizeof(Jpeg));
  jpeg->orientation = 1;
  u64 reasons = message->length;

  if (!JpegIs(data, size) || !ReadSegments(jpeg, data, size, message)) {
    if (message->length == reasons) StrBufAppendString(message, "damaged JPEG");
    FreeJpeg(jpeg);
    return false;
  }

  for (u32 index = 0; index < jpeg->componentCount; index++) {
    TransformComponent(jpeg, &jpeg->components[index]);
  }

  ConvertColors(jpeg, bitmap);
  BitmapOrient(bitmap, jpeg->orientation);
  Describe(jpeg, details);
  if (jpeg->orientation != 1) StrBufAppendString(details, ", turned upright");

  FreeJpeg(jpeg);
  return true;
}
