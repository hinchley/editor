#include "utf8.h"

#include "unicodedata.h"

// the variation selector that asks for emoji presentation.
static constexpr u32 emojiPresentation = 0xFE0F;

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

u64
Utf8PreviousStart(const char *text, u64 at)
{
  // find the nearest byte that is not a continuation byte, at most three back.
  u64 start = at - 1;
  while (start > 0 && at - start < 4 && ((u8)text[start] & 0xC0) == 0x80) start--;

  // it starts the previous code point only if it decodes as a sequence ending at at.
  // otherwise the last byte is an invalid byte of its own.
  u32 codepoint;
  if (start + Utf8Decode(text + start, at - start, &codepoint) == at) return start;
  return at - 1;
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

// binary searches a sorted range table, returning fallback when no range holds codepoint.
static u8
RangeFind(const UnicodeRange *ranges, u32 count, u32 codepoint, u8 fallback)
{
  u32 low = 0;
  u32 high = count;

  while (low < high) {
    u32 middle = low + (high - low) / 2;
    const UnicodeRange *range = &ranges[middle];
    if (codepoint < range->first) high = middle;
    else if (codepoint > range->last) low = middle + 1;
    else return range->value;
  }

  return fallback;
}

u32
Utf8Width(u32 codepoint)
{
  // ascii is by far the most common case and is always one column.
  if (codepoint < 0x300) return 1;
  return RangeFind(unicodeWidths, unicodeWidthCount, codepoint, 1);
}

static enum GraphemeBreak
BreakOf(u32 codepoint)
{
  if (codepoint >= 0x80) {
    u8 value = RangeFind(
      unicodeGraphemeBreaks,
      unicodeGraphemeBreakCount,
      codepoint,
      GraphemeBreakOther
    );
    return (enum GraphemeBreak)value;
  }

  if (codepoint == '\r') return GraphemeBreakCr;
  if (codepoint == '\n') return GraphemeBreakLf;
  if (codepoint < 0x20 || codepoint == 0x7F) return GraphemeBreakControl;
  return GraphemeBreakOther;
}

static bool
IsPictographic(u32 codepoint)
{
  // the first extended pictographic code point is U+00A9.
  if (codepoint < 0xA9) return false;
  return RangeFind(unicodePictographics, unicodePictographicCount, codepoint, 0) != 0;
}

static u32
CodepointBefore(const char *text, u64 at, u64 *start)
{
  *start = Utf8PreviousStart(text, at);
  u32 codepoint;
  Utf8Decode(text + *start, at - *start, &codepoint);
  return codepoint;
}

// rule GB11: a zwj that ends a pictographic character and its extenders joins the next
// pictographic character.
static bool
JoinsPictographic(const char *text, u64 zwjStart)
{
  u64 at = zwjStart;

  while (at > 0) {
    u64 start;
    u32 codepoint = CodepointBefore(text, at, &start);
    if (BreakOf(codepoint) != GraphemeBreakExtend) return IsPictographic(codepoint);
    at = start;
  }

  return false;
}

// rules GB12 and GB13: regional indicators pair up from the start of their run.
static bool
EndsUnpairedIndicator(const char *text, u64 at)
{
  u64 count = 0;

  while (at > 0) {
    u64 start;
    u32 codepoint = CodepointBefore(text, at, &start);
    if (BreakOf(codepoint) != GraphemeBreakRegionalIndicator) break;
    count++;
    at = start;
  }

  return count % 2 == 1;
}

static bool
IsControlBreak(enum GraphemeBreak value)
{
  return value == GraphemeBreakControl || value == GraphemeBreakCr || value == GraphemeBreakLf;
}

// decides whether a cluster boundary lies between the code points either side of at.
static bool
IsBoundary(const char *text, u64 length, u64 at)
{
  if (at == 0 || at >= length) return true;

  u64 previousStart;
  u32 previous = CodepointBefore(text, at, &previousStart);
  u32 next;
  Utf8Decode(text + at, length - at, &next);

  enum GraphemeBreak a = BreakOf(previous);
  enum GraphemeBreak b = BreakOf(next);

  if (a == GraphemeBreakCr && b == GraphemeBreakLf) return false;
  if (IsControlBreak(a) || IsControlBreak(b)) return true;

  // hangul syllable sequences.
  bool bJamo = b == GraphemeBreakL || b == GraphemeBreakV || b == GraphemeBreakLv ||
    b == GraphemeBreakLvt;
  if (a == GraphemeBreakL && bJamo) return false;

  bool vowelOrTrailing = b == GraphemeBreakV || b == GraphemeBreakT;
  if ((a == GraphemeBreakLv || a == GraphemeBreakV) && vowelOrTrailing) return false;
  if ((a == GraphemeBreakLvt || a == GraphemeBreakT) && b == GraphemeBreakT) return false;

  if (b == GraphemeBreakExtend || b == GraphemeBreakZwj) return false;
  if (b == GraphemeBreakSpacingMark || a == GraphemeBreakPrepend) return false;

  if (a == GraphemeBreakZwj && IsPictographic(next)) {
    return !JoinsPictographic(text, previousStart);
  }

  if (a == GraphemeBreakRegionalIndicator && b == GraphemeBreakRegionalIndicator) {
    return !EndsUnpairedIndicator(text, at);
  }

  return true;
}

u64
Utf8NextBoundary(const char *text, u64 length, u64 at)
{
  if (at >= length) return length;

  u32 codepoint;
  at += Utf8Decode(text + at, length - at, &codepoint);

  while (!IsBoundary(text, length, at)) at += Utf8Decode(text + at, length - at, &codepoint);
  return at;
}

u64
Utf8PreviousBoundary(const char *text, u64 length, u64 at)
{
  if (at == 0) return 0;

  at = Utf8PreviousStart(text, at);
  while (!IsBoundary(text, length, at)) at = Utf8PreviousStart(text, at);
  return at;
}

u32
Utf8ClusterWidth(const char *text, u64 length)
{
  if (length == 0) return 0;

  u32 first;
  u32 index = Utf8Decode(text, length, &first);
  u32 width = Utf8Width(first);

  // only clusters that may change width need the rest of their code points.
  bool indicator = BreakOf(first) == GraphemeBreakRegionalIndicator;
  if (width == 2 || (!indicator && !IsPictographic(first))) return width;

  while (index < length) {
    u32 codepoint;
    index += Utf8Decode(text + index, length - index, &codepoint);
    if (indicator && BreakOf(codepoint) == GraphemeBreakRegionalIndicator) return 2;
    if (!indicator && codepoint == emojiPresentation) return 2;
  }

  return width;
}

// the mapping of codepoint in a sorted table, or codepoint itself when it has none.
static u32
MapOf(const UnicodeMapping *mappings, u32 count, u32 codepoint)
{
  u32 low = 0;
  u32 high = count;

  while (low < high) {
    u32 middle = low + (high - low) / 2;
    const UnicodeMapping *mapping = &mappings[middle];
    if (codepoint < mapping->from) high = middle;
    else if (codepoint > mapping->from) low = middle + 1;
    else return mapping->to;
  }

  return codepoint;
}

u32
Utf8Fold(u32 codepoint)
{
  if (codepoint >= 'A' && codepoint <= 'Z') return codepoint + 32;
  if (codepoint < 0x80) return codepoint;
  return MapOf(unicodeFolds, unicodeFoldCount, codepoint);
}

u32
Utf8Upper(u32 codepoint)
{
  if (codepoint >= 'a' && codepoint <= 'z') return codepoint - 32;
  if (codepoint < 0x80) return codepoint;
  return MapOf(unicodeUppers, unicodeUpperCount, codepoint);
}

u32
Utf8Lower(u32 codepoint)
{
  if (codepoint >= 'A' && codepoint <= 'Z') return codepoint + 32;
  if (codepoint < 0x80) return codepoint;
  return MapOf(unicodeLowers, unicodeLowerCount, codepoint);
}
