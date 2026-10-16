#include "inflate.h"

#include <string.h>

// the longest code in deflate, and the size of the alphabets.
static constexpr i32 maxBits = 15;
static constexpr i32 literalCodes = 288;
static constexpr i32 distanceCodes = 30;

// a canonical huffman code: how many codes there are of each length, and the symbols in
// order of their codes.
typedef struct Huffman {
  u16 counts[maxBits + 1];
  u16 symbols[literalCodes];
} Huffman;

// the stream being decoded, read a bit at a time from the least significant end.
typedef struct Inflater {
  const u8 *data;
  u64 size;
  u64 at;
  u32 bits;
  i32 bitCount;
  StrBuf *out;
  u64 limit;
  bool failed;
} Inflater;

static const u16 lengthBase[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u8 lengthExtra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const u16 distanceBase[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const u8 distanceExtra[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

// the order the code length code lengths come in.
static const u8 lengthOrder[19] = {
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static u32
Bits(Inflater *inflater, i32 count)
{
  u32 value = inflater->bits;

  while (inflater->bitCount < count) {
    if (inflater->at == inflater->size) {
      inflater->failed = true;
      return 0;
    }

    value |= (u32)inflater->data[inflater->at++] << inflater->bitCount;
    inflater->bitCount += 8;
  }

  inflater->bits = (u32)((u64)value >> count);
  inflater->bitCount -= count;
  return value & ((1u << count) - 1);
}

// builds the code from a length per symbol.
// returns false for a set of lengths that is over-subscribed (more codes than fit).
// an incomplete code is allowed, as deflate permits one for a single distance code.
static bool
Build(Huffman *code, const u8 *lengths, i32 count)
{
  memset(code->counts, 0, sizeof(code->counts));
  for (i32 symbol = 0; symbol < count; symbol++) code->counts[lengths[symbol]]++;
  if (code->counts[0] == count) return true;

  i32 left = 1;

  for (i32 length = 1; length <= maxBits; length++) {
    left = (left << 1) - code->counts[length];
    if (left < 0) return false;
  }

  u16 offsets[maxBits + 1];
  offsets[1] = 0;
  for (i32 length = 1; length < maxBits; length++) {
    offsets[length + 1] = (u16)(offsets[length] + code->counts[length]);
  }

  for (i32 symbol = 0; symbol < count; symbol++) {
    if (lengths[symbol]) code->symbols[offsets[lengths[symbol]]++] = (u16)symbol;
  }

  return true;
}

// decodes one symbol, a bit at a time (codes are stored most significant bit first).
static i32
Decode(Inflater *inflater, const Huffman *code)
{
  i32 value = 0;
  i32 first = 0;
  i32 index = 0;

  for (i32 length = 1; length <= maxBits; length++) {
    value |= (i32)Bits(inflater, 1);
    if (inflater->failed) return -1;

    i32 count = code->counts[length];
    if (value - count < first) return code->symbols[index + (value - first)];

    index += count;
    first = (first + count) << 1;
    value <<= 1;
  }

  inflater->failed = true;
  return -1;
}

static bool
Room(Inflater *inflater, u64 count)
{
  if (inflater->limit && inflater->out->length + count > inflater->limit) {
    inflater->failed = true;
    return false;
  }

  return true;
}

static void
Stored(Inflater *inflater)
{
  // a stored block starts at the next byte.
  inflater->bits = 0;
  inflater->bitCount = 0;

  if (inflater->size - inflater->at < 4) {
    inflater->failed = true;
    return;
  }

  const u8 *header = inflater->data + inflater->at;
  u32 length = header[0] | (u32)header[1] << 8;
  u32 complement = header[2] | (u32)header[3] << 8;
  inflater->at += 4;

  if (length != (~complement & 0xFFFF) || inflater->size - inflater->at < length) {
    inflater->failed = true;
    return;
  }

  if (!Room(inflater, length)) return;

  StrBufAppend(inflater->out, (const char *)inflater->data + inflater->at, length);
  inflater->at += length;
}

static void
Codes(Inflater *inflater, const Huffman *literals, const Huffman *distances)
{
  for (;;) {
    i32 symbol = Decode(inflater, literals);
    if (inflater->failed) return;

    if (symbol < 256) {
      if (!Room(inflater, 1)) return;
      StrBufAppendByte(inflater->out, (char)symbol);
      continue;
    }

    if (symbol == 256) return;

    symbol -= 257;
    if (symbol >= 29) {
      inflater->failed = true;
      return;
    }

    u32 length = lengthBase[symbol] + Bits(inflater, lengthExtra[symbol]);
    i32 distanceSymbol = Decode(inflater, distances);
    if (inflater->failed) return;

    if (distanceSymbol >= distanceCodes) {
      inflater->failed = true;
      return;
    }

    u32 distance = distanceBase[distanceSymbol] + Bits(inflater, distanceExtra[distanceSymbol]);
    StrBuf *out = inflater->out;
    if (inflater->failed || distance > out->length || !Room(inflater, length)) {
      inflater->failed = true;
      return;
    }

    // the copy may overlap what it writes, so it goes a byte at a time.
    StrBufReserve(out, out->length + length);
    u64 from = out->length - distance;
    char *to = out->data + out->length;
    for (u32 index = 0; index < length; index++) to[index] = out->data[from + index];
    out->length += length;
    out->data[out->length] = '\0';
  }
}

static void
Fixed(Inflater *inflater)
{
  static Huffman literals;
  static Huffman distances;
  static bool built;

  if (!built) {
    u8 lengths[literalCodes];
    i32 symbol = 0;
    for (; symbol < 144; symbol++) lengths[symbol] = 8;
    for (; symbol < 256; symbol++) lengths[symbol] = 9;
    for (; symbol < 280; symbol++) lengths[symbol] = 7;
    for (; symbol < literalCodes; symbol++) lengths[symbol] = 8;
    Build(&literals, lengths, literalCodes);

    for (symbol = 0; symbol < distanceCodes; symbol++) lengths[symbol] = 5;
    Build(&distances, lengths, distanceCodes);
    built = true;
  }

  Codes(inflater, &literals, &distances);
}

static void
Dynamic(Inflater *inflater)
{
  i32 literalCount = (i32)Bits(inflater, 5) + 257;
  i32 distanceCount = (i32)Bits(inflater, 5) + 1;
  i32 lengthCount = (i32)Bits(inflater, 4) + 4;

  if (inflater->failed || literalCount > 286 || distanceCount > distanceCodes) {
    inflater->failed = true;
    return;
  }

  u8 lengths[literalCodes + distanceCodes] = {};
  for (i32 index = 0; index < lengthCount; index++) {
    lengths[lengthOrder[index]] = (u8)Bits(inflater, 3);
  }

  Huffman lengthCode;
  if (inflater->failed || !Build(&lengthCode, lengths, 19)) {
    inflater->failed = true;
    return;
  }

  // the literal and distance lengths, run length coded with the code just read.
  i32 total = literalCount + distanceCount;
  memset(lengths, 0, sizeof(lengths));

  for (i32 index = 0; index < total;) {
    i32 symbol = Decode(inflater, &lengthCode);
    if (inflater->failed) return;

    if (symbol < 16) {
      lengths[index++] = (u8)symbol;
      continue;
    }

    u8 repeated = 0;
    i32 count;

    if (symbol == 16) {
      if (index == 0) {
        inflater->failed = true;
        return;
      }

      repeated = lengths[index - 1];
      count = 3 + (i32)Bits(inflater, 2);
    } else if (symbol == 17) {
      count = 3 + (i32)Bits(inflater, 3);
    } else {
      count = 11 + (i32)Bits(inflater, 7);
    }

    if (inflater->failed || index + count > total) {
      inflater->failed = true;
      return;
    }

    while (count--) lengths[index++] = repeated;
  }

  // a block without an end of block code could never end.
  Huffman literals;
  Huffman distances;
  bool built = lengths[256] != 0 && Build(&literals, lengths, literalCount) &&
    Build(&distances, lengths + literalCount, distanceCount);

  if (!built) {
    inflater->failed = true;
    return;
  }

  Codes(inflater, &literals, &distances);
}

bool
InflateZlib(const u8 *data, u64 size, u64 expected, StrBuf *out)
{
  // the header: deflate with a window of at most 32 KB, no preset dictionary, and a check
  // that makes the two bytes a multiple of 31.
  if (size < 2) return false;
  if ((data[0] & 0x0F) != 8 || (data[0] >> 4) > 7) return false;
  if ((data[1] & 0x20) || ((u32)data[0] << 8 | data[1]) % 31) return false;

  Inflater inflater = {
    .data = data,
    .size = size,
    .at = 2,
    .out = out,
    .limit = expected ? out->length + expected : 0
  };

  if (expected) StrBufReserve(out, out->length + expected);
  bool last = false;

  while (!last && !inflater.failed) {
    last = Bits(&inflater, 1);
    u32 type = Bits(&inflater, 2);
    if (inflater.failed) break;

    switch (type) {
    case 0:
      Stored(&inflater);
      break;
    case 1:
      Fixed(&inflater);
      break;
    case 2:
      Dynamic(&inflater);
      break;
    default:
      inflater.failed = true;
      break;
    }
  }

  if (inflater.failed) return false;
  return !inflater.limit || out->length == inflater.limit;
}
