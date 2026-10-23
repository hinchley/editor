#include "filter.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"

void
FilterAdd(Filter *filter, const char *pattern, u64 length)
{
  FilterPattern added = {};

  if (length && pattern[length - 1] == '/') {
    added.folder = true;
    length--;
  }

  if (length && pattern[0] == '/') {
    added.anchored = true;
    pattern++;
    length--;
  }

  if (!length) return;

  // a slash inside the pattern ties it to a path, as a leading one does.
  if (memchr(pattern, '/', length)) added.anchored = true;
  StrBufAppend(&added.text, pattern, length);

  if (filter->count == filter->capacity) {
    filter->capacity = filter->capacity ? filter->capacity * 2 : 8;
    filter->patterns = MemoryResize(filter->patterns, filter->capacity, sizeof(FilterPattern));
  }

  filter->patterns[filter->count++] = added;
}

static bool
IsBlank(char character)
{
  return character == ' ' || character == '\t' || character == '\r';
}

void
FilterParse(Filter *filter, const char *text, u64 length)
{
  u64 at = 0;

  while (at < length) {
    const char *newline = memchr(text + at, '\n', length - at);
    u64 end = newline ? (u64)(newline - text) : length;

    // a comment starts at a "#" that begins the line or follows a blank.
    u64 stop = at;

    while (stop < end && !(text[stop] == '#' && (stop == at || IsBlank(text[stop - 1])))) {
      stop++;
    }

    u64 start = at;
    while (start < stop && IsBlank(text[start])) start++;
    while (stop > start && IsBlank(text[stop - 1])) stop--;
    if (stop > start) FilterAdd(filter, text + start, stop - start);

    at = end + 1;
  }
}

void
FilterFree(Filter *filter)
{
  for (u32 index = 0; index < filter->count; index++) {
    StrBufFree(&filter->patterns[index].text);
  }

  free(filter->patterns);
  *filter = (Filter){};
}

// matches text against a pattern where "*" stands for any run of characters other than
// "/".
// a failed match backtracks only to the last "*", which is enough because a "*" can
// never cross a slash that the pattern itself has to match.
static bool
Glob(const char *pattern, u64 patternLength, const char *text, u64 textLength)
{
  u64 patternAt = 0;
  u64 textAt = 0;
  u64 starPattern = UINT64_MAX;
  u64 starText = 0;

  while (textAt < textLength) {
    if (patternAt < patternLength && pattern[patternAt] == '*') {
      starPattern = patternAt++;
      starText = textAt;
    } else if (patternAt < patternLength && pattern[patternAt] == text[textAt]) {
      patternAt++;
      textAt++;
    } else if (starPattern != UINT64_MAX && text[starText] != '/') {
      patternAt = starPattern + 1;
      textAt = ++starText;
    } else {
      return false;
    }
  }

  while (patternAt < patternLength && pattern[patternAt] == '*') patternAt++;
  return patternAt == patternLength;
}

// matches one pattern against the whole of a path (anchored) or its last name.
static bool
MatchesPath(const FilterPattern *pattern, const char *path, u64 length)
{
  u64 start = 0;

  if (!pattern->anchored) {
    for (u64 index = length; index > 0; index--) {
      if (path[index - 1] != '/') continue;

      start = index;
      break;
    }
  }

  return Glob(pattern->text.data, pattern->text.length, path + start, length - start);
}

static bool
MatchesPattern(const FilterPattern *pattern, const char *path, u64 length, bool isFolder)
{
  if (!pattern->folder) return !isFolder && MatchesPath(pattern, path, length);
  if (isFolder && MatchesPath(pattern, path, length)) return true;

  // a folder pattern also takes everything inside the folders it matches.
  for (u64 index = 0; index < length; index++) {
    if (path[index] == '/' && MatchesPath(pattern, path, index)) return true;
  }

  return false;
}

bool
FilterMatches(const Filter *filter, const char *path, u64 length, bool isFolder)
{
  for (u32 index = 0; index < filter->count; index++) {
    if (MatchesPattern(&filter->patterns[index], path, length, isFolder)) return true;
  }

  return false;
}
