#pragma once

// the search pattern shared by find, replace and the match commands.
// a pattern is literal text: there are no wildcards.
// the escapes \t, \r, \n, \\, \^ and \$ stand for a tab, a carriage return, a line break,
// a backslash and the two anchor characters, and any other backslash is literal.
// a "^" at the start ties a match to the start of a line, and a "$" at the end to its end.
// a match never crosses a line break, so a pattern holding one never matches.
#include "strbuf.h"
#include "types.h"

typedef struct PatternOptions {
  bool caseSensitive;

  // a match must not have a word character just before or after it.
  bool wholeWord;
} PatternOptions;

typedef struct Pattern {
  // the text as typed, and how it is matched.
  StrBuf source;
  PatternOptions options;

  // the literal text, and its code points case folded.
  StrBuf text;
  u32 *folded;
  u64 foldedCount;
  u64 foldedCapacity;

  bool lineStart;
  bool lineEnd;

  // empty, or holding a line break, so it never matches.
  bool impossible;
} Pattern;

// a match within one line, in bytes.
typedef struct PatternMatch {
  u64 start;
  u64 end;
} PatternMatch;

// parses source into pattern, replacing what it held.
void
PatternSet(Pattern *pattern, const char *source, u64 length, PatternOptions options);

void
PatternFree(Pattern *pattern);

// finds the first match in a line that starts at or after from.
// matches start and end on grapheme cluster boundaries and are never empty.
bool
PatternFind(
  const Pattern *pattern,
  const char *text,
  u64 length,
  u64 from,
  PatternMatch *match
);

// replaces first, a match in a line of text, and every match after it that starts before
// limit, all at once.
// rebuilt is set to what replaces the text from first.start up to the returned end of the
// last match, and the number of matches replaced is added to count.
u64
PatternReplaceLine(
  const Pattern *pattern,
  const char *text,
  u64 length,
  PatternMatch first,
  u64 limit,
  const StrBuf *replacement,
  StrBuf *rebuilt,
  u64 *count
);

// appends the source of a pattern that matches text literally, escaping backslashes and
// the anchor characters where they would act as anchors.
void
PatternEscape(const char *text, u64 length, StrBuf *source);

// appends source with its escapes replaced, for replacement text.
// \n becomes a line break here.
void
PatternUnescape(const char *source, u64 length, StrBuf *output);
