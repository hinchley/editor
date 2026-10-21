#include "pattern.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "utf8.h"

// the character an escape stands for, or 0 when the backslash is literal.
static char
Escaped(char character)
{
  switch (character) {
    case 't':
      return '\t';
    case 'r':
      return '\r';
    case 'n':
      return '\n';
    case '\\':
      return '\\';
    case '^':
      return '^';
    case '$':
      return '$';
    default:
      return '\0';
  }
}

static void
AddFolded(Pattern *pattern, u32 codepoint)
{
  if (pattern->foldedCount == pattern->foldedCapacity) {
    u64 capacity = pattern->foldedCapacity ? pattern->foldedCapacity * 2 : 16;
    pattern->folded = MemoryResize(pattern->folded, capacity, sizeof(u32));
    pattern->foldedCapacity = capacity;
  }

  pattern->folded[pattern->foldedCount++] = codepoint;
}

static void
Fold(Pattern *pattern)
{
  const char *text = pattern->text.data;
  u64 length = pattern->text.length;
  pattern->foldedCount = 0;

  for (u64 at = 0; at < length;) {
    u32 codepoint;
    at += Utf8Decode(text + at, length - at, &codepoint);
    AddFolded(pattern, Utf8Fold(codepoint));
  }
}

void
PatternSet(Pattern *pattern, const char *source, u64 length, PatternOptions options)
{
  StrBufClear(&pattern->source);
  StrBufAppend(&pattern->source, source, length);
  StrBufClear(&pattern->text);
  StrBufClear(&pattern->error);
  pattern->options = options;
  pattern->lineStart = length && source[0] == '^';
  pattern->lineEnd = false;

  if (options.regex) {
    pattern->lineStart = false;
    pattern->foldedCount = 0;
    bool ignoreCase = !options.caseSensitive;
    Regex *regex = &pattern->regex;
    bool compiled = length && RegexCompile(regex, source, length, ignoreCase, &pattern->error);
    pattern->impossible = !compiled;
    return;
  }

  for (u64 at = pattern->lineStart; at < length; at++) {
    char escaped = source[at] == '\\' && at + 1 < length ? Escaped(source[at + 1]) : '\0';

    if (escaped) {
      StrBufAppendByte(&pattern->text, escaped);
      at++;
    } else if (source[at] == '$' && at + 1 == length) {
      pattern->lineEnd = true;
    } else {
      StrBufAppendByte(&pattern->text, source[at]);
    }
  }

  const char *text = pattern->text.data;
  u64 size = pattern->text.length;
  pattern->impossible = !size || memchr(text, '\n', size);
  Fold(pattern);
}

void
PatternEscape(const char *text, u64 length, StrBuf *source)
{
  for (u64 at = 0; at < length; at++) {
    bool anchor = (at == 0 && text[at] == '^') || (at + 1 == length && text[at] == '$');
    if (anchor || text[at] == '\\') StrBufAppendByte(source, '\\');
    StrBufAppendByte(source, text[at]);
  }
}

void
PatternFree(Pattern *pattern)
{
  StrBufFree(&pattern->source);
  StrBufFree(&pattern->text);
  StrBufFree(&pattern->error);
  RegexFree(&pattern->regex);
  free(pattern->folded);
  *pattern = (Pattern){};
}

// true when a grapheme cluster starts at offset at.
static bool
IsBoundary(const char *text, u64 length, u64 at)
{
  if (at == 0 || at >= length) return true;
  if ((u8)text[at - 1] < 0x80 && (u8)text[at] < 0x80) return true;

  u64 start = Utf8PreviousBoundary(text, length, at);
  return Utf8NextBoundary(text, length, start) == at;
}

static bool
WordBefore(const char *text, u64 at)
{
  if (at == 0) return false;

  u64 start = Utf8PreviousStart(text, at);
  u32 codepoint;
  Utf8Decode(text + start, at - start, &codepoint);
  return Utf8IsWord(codepoint);
}

static bool
WordAfter(const char *text, u64 length, u64 at)
{
  if (at >= length) return false;

  u32 codepoint;
  Utf8Decode(text + at, length - at, &codepoint);
  return Utf8IsWord(codepoint);
}

// checks what surrounds a match of the text: the anchors, word edges and clusters.
static bool
Accepts(const Pattern *pattern, const char *text, u64 length, PatternMatch match)
{
  if (pattern->lineEnd && match.end != length) return false;

  bool wholeWord = pattern->options.wholeWord;

  if (wholeWord && (WordBefore(text, match.start) || WordAfter(text, length, match.end))) {
    return false;
  }

  return IsBoundary(text, length, match.start) && IsBoundary(text, length, match.end);
}

// reads one code point at at, case folded, and returns the offset after it.
static u64
ReadFolded(const char *text, u64 length, u64 at, u32 *codepoint)
{
  u8 byte = (u8)text[at];

  if (byte < 0x80) {
    *codepoint = byte >= 'A' && byte <= 'Z' ? byte | 0x20u : byte;
    return at + 1;
  }

  at += Utf8Decode(text + at, length - at, codepoint);
  *codepoint = Utf8Fold(*codepoint);
  return at;
}

// compares the folded pattern with the text at start (whose first code point is already
// known to match) and gives the end of the match.
static bool
FoldedMatches(const Pattern *pattern, const char *text, u64 length, u64 next, u64 *end)
{
  for (u64 index = 1; index < pattern->foldedCount; index++) {
    if (next >= length) return false;

    u32 codepoint;
    next = ReadFolded(text, length, next, &codepoint);
    if (codepoint != pattern->folded[index]) return false;
  }

  *end = next;
  return true;
}

static bool
FindFolded(const Pattern *pattern, const char *text, u64 length, u64 from, PatternMatch *match)
{
  u32 first = pattern->folded[0];

  for (u64 at = from; at < length;) {
    u32 codepoint;
    u64 next = ReadFolded(text, length, at, &codepoint);
    u64 end = 0;

    if (codepoint == first && FoldedMatches(pattern, text, length, next, &end)) {
      *match = (PatternMatch){at, end};
      if (Accepts(pattern, text, length, *match)) return true;
    }

    if (pattern->lineStart) return false;

    at = next;
  }

  return false;
}

static bool
FindExact(const Pattern *pattern, const char *text, u64 length, u64 from, PatternMatch *match)
{
  const char *wanted = pattern->text.data;
  u64 size = pattern->text.length;

  for (u64 at = from; at + size <= length; at++) {
    const char *first = memchr(text + at, wanted[0], length - size + 1 - at);
    if (!first) return false;

    at = (u64)(first - text);
    if (pattern->lineStart && at != 0) return false;

    *match = (PatternMatch){at, at + size};
    bool same = memcmp(first, wanted, size) == 0;
    if (same && Accepts(pattern, text, length, *match)) return true;
    if (pattern->lineStart) return false;
  }

  return false;
}

// a match the regular expression finds may still fail the checks of Accepts, and then
// the search goes on from the code point after its start.
static bool
FindRegex(const Pattern *pattern, const char *text, u64 length, u64 from, PatternMatch *match)
{
  RegexSpan span;

  for (u64 at = from; RegexFind(&pattern->regex, text, length, at, false, &span, 1);) {
    *match = (PatternMatch){span.start, span.end};
    if (Accepts(pattern, text, length, *match)) return true;

    u32 codepoint;
    at = span.start + Utf8Decode(text + span.start, length - span.start, &codepoint);
  }

  return false;
}

bool
PatternFind(
  const Pattern *pattern,
  const char *text,
  u64 length,
  u64 from,
  PatternMatch *match
)
{
  if (pattern->impossible || from >= length) return false;
  if (pattern->lineStart && from > 0) return false;

  if (pattern->options.regex) return FindRegex(pattern, text, length, from, match);
  if (pattern->options.caseSensitive) return FindExact(pattern, text, length, from, match);
  return FindFolded(pattern, text, length, from, match);
}

void
PatternExpand(
  const Pattern *pattern,
  const char *text,
  u64 length,
  PatternMatch match,
  const StrBuf *replacement,
  StrBuf *output
)
{
  const char *source = replacement->data ? replacement->data : "";
  u64 size = replacement->length;
  bool regex = pattern->options.regex;

  // the groups are found again, by matching at the start of the match, the first time the
  // replacement names one.
  RegexSpan groups[regexGroups];
  bool grouped = false;

  for (u64 at = 0; at < size; at++) {
    char next = at + 1 < size ? source[at + 1] : '\0';
    char escaped = source[at] == '\\' ? Escaped(next) : '\0';
    bool group = regex && source[at] == '\\' && next >= '0' && next <= '9';

    if (!group) {
      StrBufAppendByte(output, escaped ? escaped : source[at]);
      at += escaped != '\0';
      continue;
    }

    RegexSpan span = {match.start, match.end};

    if (next > '0') {
      if (!grouped) {
        const Regex *compiled = &pattern->regex;
        bool found = RegexFind(compiled, text, length, match.start, true, groups, regexGroups);
        RegexSpan unset = {regexUnset, regexUnset};
        for (u32 index = 0; index < regexGroups && !found; index++) groups[index] = unset;
        grouped = true;
      }

      span = groups[next - '0'];
    }

    if (span.start != regexUnset) {
      StrBufAppend(output, text + span.start, span.end - span.start);
    }
    at++;
  }
}

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
)
{
  PatternMatch hit = first;
  u64 copied = hit.start;
  StrBufClear(rebuilt);

  // matches are found in the original text, so a replacement holding the pattern is never
  // matched again.
  do {
    StrBufAppend(rebuilt, text + copied, hit.start - copied);
    PatternExpand(pattern, text, length, hit, replacement, rebuilt);
    copied = hit.end;
    (*count)++;
  } while (PatternFind(pattern, text, length, copied, &hit) && hit.start < limit);

  return copied;
}
