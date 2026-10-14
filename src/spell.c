#include "spell.h"

#include "memory.h"
#include "utf8.h"

// one word per line, sorted in byte order (build.sh checks it).
static const u8 words[] = {
#embed "../data/words.txt"
};

// longest word looked up, in bytes; anything longer is unknown.
static constexpr u64 maxWordSize = 96;

// how far either side of a word to look for signs of a url or email address.
static constexpr u64 linkReach = 256;

static constexpr u32 rightQuote = 0x2019;

// where each entry of the list starts, built on first use.
static u32 *entries;
static u32 entryCount;

static void
IndexWords(void)
{
  u64 count = 1;
  for (u64 at = 0; at < sizeof words; at++) count += words[at] == '\n';

  entries = MemoryAllocate(count * sizeof(u32));
  u64 start = 0;

  for (u64 at = 0; at <= sizeof words; at++) {
    if (at < sizeof words && words[at] != '\n') continue;
    if (at > start) entries[entryCount++] = (u32)start;
    start = at + 1;
  }
}

// compares a word with an entry in byte order.
static i32
CompareEntry(const char *word, u64 length, u32 entry)
{
  const u8 *text = words + entry;
  u64 room = sizeof words - entry;

  for (u64 index = 0; index < length; index++) {
    if (index == room || text[index] == '\n') return 1;
    if ((u8)word[index] != text[index]) return (u8)word[index] < text[index] ? -1 : 1;
  }

  return length == room || text[length] == '\n' ? 0 : -1;
}

static bool
InList(const char *word, u64 length)
{
  if (!entries) IndexWords();

  u32 low = 0;
  u32 high = entryCount;

  while (low < high) {
    u32 middle = low + (high - low) / 2;
    i32 order = CompareEntry(word, length, entries[middle]);
    if (order == 0) return true;
    if (order < 0) high = middle;
    else low = middle + 1;
  }

  return false;
}

// letters of the latin, greek and cyrillic scripts, and combining marks.
// other scripts are not checked against an english list.
static bool
IsLetter(u32 codepoint)
{
  if (codepoint < 0x80) return (codepoint | 0x20) >= 'a' && (codepoint | 0x20) <= 'z';
  if (codepoint == 0xD7 || codepoint == 0xF7) return false;
  if (codepoint >= 0xC0 && codepoint < 0x530) return true;
  return codepoint >= 0x1E00 && codepoint < 0x2000;
}

static bool
IsApostrophe(u32 codepoint)
{
  return codepoint == '\'' || codepoint == rightQuote;
}

// a character that belongs to a run read as one token.
static bool
IsTokenPart(u32 codepoint)
{
  bool digit = codepoint >= '0' && codepoint <= '9';
  return IsLetter(codepoint) || digit || codepoint == '_' || IsApostrophe(codepoint);
}

static u64
Next(const char *text, u64 length, u64 at, u32 *codepoint)
{
  return at + Utf8Decode(text + at, length - at, codepoint);
}

static bool
IsBlank(char character)
{
  return character == ' ' || character == '\t';
}

static bool
Contains(const char *text, u64 start, u64 end, const char *part, u64 partLength)
{
  for (u64 at = start; at + partLength <= end; at++) {
    u64 index = 0;
    while (index < partLength && text[at + index] == part[index]) index++;
    if (index == partLength) return true;
  }

  return false;
}

// true when the blank separated text around a word looks like a url or email address.
static bool
InLink(const char *text, u64 length, u64 start, u64 end)
{
  while (start > 0 && !IsBlank(text[start - 1]) && end - start < linkReach) start--;
  while (end < length && !IsBlank(text[end]) && end - start < 2 * linkReach) end++;

  bool url = Contains(text, start, end, "://", 3) || Contains(text, start, end, "www.", 4);
  return url || Contains(text, start, end, "@", 1);
}

// copies a word with typographic apostrophes made plain, optionally in lowercase.
static bool
MakeKey(const char *text, u64 start, u64 end, bool lower, char *key, u64 *keyLength)
{
  *keyLength = 0;

  for (u64 at = start; at < end;) {
    u32 codepoint;
    at = Next(text, end, at, &codepoint);
    if (codepoint == rightQuote) codepoint = '\'';
    if (lower) codepoint = Utf8Fold(codepoint);
    if (*keyLength + 4 > maxWordSize) return false;

    *keyLength += Utf8Encode(codepoint, key + *keyLength);
  }

  return true;
}

static bool
IsKnown(const char *text, u64 start, u64 end, bool capitalised)
{
  char key[maxWordSize];
  u64 keyLength;
  if (!MakeKey(text, start, end, false, key, &keyLength)) return false;
  if (InList(key, keyLength)) return true;

  return capitalised && MakeKey(text, start, end, true, key, &keyLength) &&
    InList(key, keyLength);
}

// trims the apostrophes around a run and decides whether it is a word to check.
static bool
NeedsCheck(const char *text, u64 *start, u64 *end, bool *capitalised)
{
  u32 codepoint;

  while (*start < *end) {
    u64 next = Next(text, *end, *start, &codepoint);
    if (!IsApostrophe(codepoint)) break;
    *start = next;
  }

  while (*end > *start) {
    u64 previous = Utf8PreviousStart(text, *end);
    Utf8Decode(text + previous, *end - previous, &codepoint);
    if (!IsApostrophe(codepoint)) break;
    *end = previous;
  }

  u64 letters = 0;
  u64 capitals = 0;

  for (u64 at = *start; at < *end;) {
    at = Next(text, *end, at, &codepoint);
    if (!IsLetter(codepoint) && !IsApostrophe(codepoint)) return false;

    bool capital = IsLetter(codepoint) && Utf8Fold(codepoint) != codepoint;
    if (letters == 0) *capitalised = capital;
    letters += IsLetter(codepoint);
    capitals += capital;
  }

  // words in capitals only are taken to be acronyms.
  return letters && !(letters > 1 && capitals == letters);
}

bool
SpellFind(const char *text, u64 length, u64 from, u64 *start, u64 *end)
{
  u32 codepoint;
  u64 at = from;

  // back up to the start of the run holding from, so runs are always read whole.
  while (at > 0) {
    u64 previous = Utf8PreviousStart(text, at);
    Utf8Decode(text + previous, at - previous, &codepoint);
    if (!IsTokenPart(codepoint)) break;
    at = previous;
  }

  while (at < length) {
    u64 runStart = at;
    at = Next(text, length, at, &codepoint);
    if (!IsTokenPart(codepoint)) continue;

    u64 runEnd = at;

    while (runEnd < length) {
      u64 next = Next(text, length, runEnd, &codepoint);
      if (!IsTokenPart(codepoint)) break;
      runEnd = next;
    }

    at = runEnd;
    *start = runStart;
    *end = runEnd;
    bool capitalised = false;
    if (!NeedsCheck(text, start, end, &capitalised) || *start < from) continue;
    if (IsKnown(text, *start, *end, capitalised)) continue;
    if (!InLink(text, length, *start, *end)) return true;
  }

  return false;
}
