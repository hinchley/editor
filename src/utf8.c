#include "utf8.h"

#include "unicodedata.h"

u32
Utf8SequenceLength(u8 lead)
{
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) return 2;
  if (lead >= 0xE0 && lead <= 0xEF) return 3;
  if (lead >= 0xF0 && lead <= 0xF4) return 4;
  return 0;
}

u32
Utf8Decode(const char *text, u64 length, u32 *codepoint)
{
  const u8 *bytes = (const u8 *)text;
  u32 size = Utf8SequenceLength(bytes[0]);
  *codepoint = utf8Replacement;
  if (size == 0 || size > length) return 1;

  if (size == 1) {
    *codepoint = bytes[0];
    return 1;
  }

  u32 value = bytes[0] & (0x7F >> size);

  for (u32 index = 1; index < size; index++) {
    if ((bytes[index] & 0xC0) != 0x80) return 1;
    value = (value << 6) | (bytes[index] & 0x3F);
  }

  // reject overlong forms, surrogates and values past the unicode range.
  static constexpr u32 minimums[] = {0, 0, 0x80, 0x800, 0x10000};
  if (value < minimums[size] || value > 0x10FFFF) return 1;
  if (value >= 0xD800 && value <= 0xDFFF) return 1;

  *codepoint = value;
  return size;
}

u32
Utf8Encode(u32 codepoint, char output[4])
{
  if (codepoint < 0x80) {
    output[0] = (char)codepoint;
    return 1;
  }

  if (codepoint < 0x800) {
    output[0] = (char)(0xC0 | (codepoint >> 6));
    output[1] = (char)(0x80 | (codepoint & 0x3F));
    return 2;
  }

  if (codepoint < 0x10000) {
    output[0] = (char)(0xE0 | (codepoint >> 12));
    output[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    output[2] = (char)(0x80 | (codepoint & 0x3F));
    return 3;
  }

  output[0] = (char)(0xF0 | (codepoint >> 18));
  output[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
  output[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
  output[3] = (char)(0x80 | (codepoint & 0x3F));
  return 4;
}

u32
Utf8Width(u32 codepoint)
{
  // ascii is by far the most common case and is always one column.
  if (codepoint < 0x300) return 1;

  u32 low = 0;
  u32 high = unicodeWidthCount;

  while (low < high) {
    u32 middle = low + (high - low) / 2;
    const UnicodeRange *range = &unicodeWidths[middle];
    if (codepoint < range->first) high = middle;
    else if (codepoint > range->last) low = middle + 1;
    else return range->value;
  }

  return 1;
}
