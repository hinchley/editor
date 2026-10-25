#include "spell.h"

#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "file.h"
#include "utf8.h"

// longest word looked up, in bytes; anything longer is unknown.
static constexpr u64 maxWordSize = 96;

// how far either side of a word to look for signs of a url or email address.
static constexpr u64 linkReach = 256;

static constexpr u32 rightQuote = 0x2019;

// the word list, one word per line, read on first use and again when it changes on disk
// (or the built-in one, while there is none on disk).
// its lines are kept in byte order, and the empty line after a final line break is left
// out of the search.
static File wordsFile;
static Buffer words;
static bool loaded;

// orders two words by their bytes, a shorter word before a longer one it starts.
static i32
CompareWords(const char *left, u64 leftLength, const char *right, u64 rightLength)
{
  // an empty line's data may be nullptr, which memcmp must not be given.
  u64 shorter = leftLength < rightLength ? leftLength : rightLength;
  i32 order = shorter ? memcmp(left, right, shorter) : 0;
  if (order) return order;
  return leftLength < rightLength ? -1 : leftLength > rightLength;
}

static i32
CompareLines(const void *left, const void *right)
{
  const StrBuf *first = left;
  const StrBuf *second = right;
  return CompareWords(first->data, first->length, second->data, second->length);
}

// the lines holding words: all but an empty last line.
static u64
WordCount(void)
{
  u64 count = BufferLineCount(&words);
  return count && !BufferLine(&words, count - 1)->length ? count - 1 : count;
}

// a list edited by hand may be out of order, so it is sorted in memory.
static void
SortWords(void)
{
  u64 count = WordCount();

  for (u64 index = 1; index < count; index++) {
    if (CompareLines(&words.lines[index - 1], &words.lines[index]) > 0) {
      qsort(words.lines, count, sizeof(StrBuf), CompareLines);
      return;
    }
  }
}

bool
SpellReady(StrBuf *message)
{
  if (!wordsFile.path.length) FileConfigPath("words.txt", &wordsFile.path);
  if (loaded && !FileChangedOnDisk(&wordsFile)) return true;

  enum FileStatus status = FileLoad(&wordsFile, &words, message);

  // with no list in the config folder, a built-in one is used until it is first changed,
  // which saves it there.
  if (status == FileStatusNew && builtinWords.data) {
    BufferSetText(&words, (const char *)builtinWords.data, builtinWords.length);
    status = FileStatusOk;
  } else if (status == FileStatusNew) {
    StrBufAppendFormat(message, "no word list: put one at %s", wordsFile.path.data);
  }

  // a list read before stays in use when it cannot be read again.
  if (status != FileStatusOk) return loaded;

  SortWords();
  loaded = true;
  return true;
}

// finds where a word is in the list, or where it would go.
static bool
Locate(const char *word, u64 length, u64 *index)
{
  u64 low = 0;
  u64 high = WordCount();

  while (low < high) {
    u64 middle = low + (high - low) / 2;
    const StrBuf *line = BufferLine(&words, middle);
    i32 order = CompareWords(word, length, line->data, line->length);

    if (order == 0) {
      *index = middle;
      return true;
    }

    if (order < 0) high = middle;
    else low = middle + 1;
  }

  *index = low;
  return false;
}

static bool
InList(const char *word, u64 length)
{
  u64 index;
  return Locate(word, length, &index);
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

// the start of the run holding or ending at at.
static u64
RunStart(const char *text, u64 at)
{
  u32 codepoint;

  while (at > 0) {
    u64 previous = Utf8PreviousStart(text, at);
    Utf8Decode(text + previous, at - previous, &codepoint);
    if (!IsTokenPart(codepoint)) break;
    at = previous;
  }

  return at;
}

// the end of the run from at, which is at itself when no run starts there.
static u64
RunEnd(const char *text, u64 length, u64 at)
{
  u32 codepoint;

  while (at < length) {
    u64 next = Next(text, length, at, &codepoint);
    if (!IsTokenPart(codepoint)) break;
    at = next;
  }

  return at;
}

bool
SpellFind(const char *text, u64 length, u64 from, u64 *start, u64 *end)
{
  if (!loaded) return false;

  u32 codepoint;

  // back up to the start of the run holding from, so runs are always read whole.
  u64 at = RunStart(text, from);

  while (at < length) {
    u64 runStart = at;
    at = Next(text, length, at, &codepoint);
    if (!IsTokenPart(codepoint)) continue;

    u64 runEnd = RunEnd(text, length, at);
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

bool
SpellWordAt(const char *text, u64 length, u64 at, u64 *start, u64 *end)
{
  *start = RunStart(text, at);
  *end = RunEnd(text, length, *start);

  char key[maxWordSize];
  u64 keyLength;
  bool capitalised = false;
  if (!NeedsCheck(text, start, end, &capitalised)) return false;
  return MakeKey(text, *start, *end, false, key, &keyLength);
}

bool
SpellEntry(const char *text, u64 start, u64 end, StrBuf *entry)
{
  char key[maxWordSize];
  u64 keyLength;
  bool capitalised = false;
  NeedsCheck(text, &start, &end, &capitalised);
  MakeKey(text, start, end, false, key, &keyLength);
  StrBufClear(entry);
  StrBufAppend(entry, key, keyLength);
  if (InList(key, keyLength)) return true;

  if (!capitalised || !MakeKey(text, start, end, true, key, &keyLength)) return false;
  if (!InList(key, keyLength)) return false;

  StrBufClear(entry);
  StrBufAppend(entry, key, keyLength);
  return true;
}

// puts a word in the list at index, after the line break of the last word when there is
// no empty line after it.
static void
Insert(u64 index, const char *word, u64 length)
{
  StrBuf text = {};

  if (index < BufferLineCount(&words)) {
    StrBufAppend(&text, word, length);
    StrBufAppendByte(&text, '\n');
    BufferReplace(&words, (Range){{index, 0}, {index, 0}}, text.data, text.length);
  } else {
    StrBufAppendByte(&text, '\n');
    StrBufAppend(&text, word, length);
    Position end = BufferEnd(&words);
    BufferReplace(&words, (Range){end, end}, text.data, text.length);
  }

  StrBufFree(&text);
}

// takes the word at index out of the list with a line break next to it.
static void
Remove(u64 index)
{
  Range range = {{index, 0}, {index + 1, 0}};

  if (index + 1 == BufferLineCount(&words)) {
    range.end = (Position){index, BufferLine(&words, index)->length};
    if (index > 0) range.start = (Position){index - 1, BufferLine(&words, index - 1)->length};
  }

  BufferReplace(&words, range, "", 0);
}

bool
SpellChange(const char *word, u64 length, bool add, StrBuf *message)
{
  if (!SpellReady(message)) return false;

  u64 index;
  if (Locate(word, length, &index) == add) return true;

  if (add) Insert(index, word, length);
  else Remove(index);

  // a failure to make the config folder shows as the save's.
  if (!wordsFile.stamp.exists) FileMakeParents(wordsFile.path.data, 0777);
  if (FileSave(&wordsFile, &words, false, message) == FileStatusOk) return true;

  // the list in memory stays the one on disk.
  if (add) Remove(index);
  else Insert(index, word, length);
  return false;
}
