#pragma once

// regular expressions for search and the #match? predicates of queries, matched one line
// at a time over code points.
// a pike vm (a thompson nfa simulation whose threads keep their priority) finds the
// leftmost match, preferring alternatives and repetitions as perl does, in time
// proportional to the line's length times the program's size, so no pattern can make a
// search take exponential time.
//
// syntax: literals, "\" before any punctuation, the escapes \t \r \n \f \v, "." (any code
// point), [...] and [^...] with ranges and \d \w \s inside, \d \D \w \W \s \S, the anchors
// ^ $ \b \B, the repetitions * + ? {m} {m,} {m,n} (each lazy with a "?" after it), groups
// ( ) and (?: ), and alternation |.
// \w and \b use the word characters of Utf8IsWord (so every non-ascii character is one),
// \d is ascii 0 to 9 and \s is space, tab, \r, \f and \v.
// there are no backreferences, lookaround, posix classes or unicode properties.
#include "strbuf.h"
#include "types.h"

// the most groups whose text a match can report: the whole match is group 0.
static constexpr u32 regexGroups = 10;

// where a group matched, in bytes, or regexUnset at both ends when it took no part.
typedef struct RegexSpan {
  u64 start;
  u64 end;
} RegexSpan;

static constexpr u64 regexUnset = UINT64_MAX;

typedef struct RegexInstruction RegexInstruction;
typedef struct RegexClass RegexClass;
typedef struct RegexRange RegexRange;

typedef struct Regex {
  RegexInstruction *program;
  u32 count;
  u32 capacity;

  RegexClass *classes;
  u32 classCount;
  RegexRange *ranges;
  u32 rangeCount;

  bool ignoreCase;

  // every match starts at the start of the line.
  bool lineStart;

  // every match starts with this ascii byte (and its other case when ignoring case),
  // or 0.
  u8 firstByte;

  // otherwise, when every match starts with a code point one instruction consumes (a
  // class, say), that instruction's index plus one, or 0.
  u32 firstStep;
} Regex;

// compiles source into regex, replacing what it held.
// returns false and appends the reason to error when source is not a valid pattern.
bool
RegexCompile(Regex *regex, const char *source, u64 length, bool ignoreCase, StrBuf *error);

void
RegexFree(Regex *regex);

// finds the first non-empty match in a line that starts at or after from (or exactly at
// from, when anchored) and before limit, and fills spans with the first spanCount groups
// (1 to regexGroups).
// ^, $ and \b see the whole line, not just the part from from, and a match may end after
// limit.
bool
RegexFind(
  const Regex *regex,
  const char *text,
  u64 length,
  u64 from,
  u64 limit,
  bool anchored,
  RegexSpan *spans,
  u32 spanCount
);
