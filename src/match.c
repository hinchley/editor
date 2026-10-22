#include "match.h"

#include "spell.h"

// the first unknown word from from, passing over words spell check does not look at.
static bool
FindUnknown(const MatchSet *set, const StrBuf *text, u64 line, u64 from, PatternMatch *match)
{
  while (SpellFind(text->data, text->length, from, &match->start, &match->end)) {
    if (!set->scope || HighlightSpellable(set->scope, line, match->start)) return true;
    from = match->end;
  }

  return false;
}

bool
MatchInLine(const MatchSet *set, const Buffer *buffer, u64 line, u64 from, PatternMatch *match)
{
  const StrBuf *text = BufferLine(buffer, line);
  const Pattern *pattern = set->pattern;

  switch (set->kind) {
    case MatchKindSearch:
      return PatternFind(pattern, text->data, text->length, from, match);
    case MatchKindSpell:
      return FindUnknown(set, text, line, from, match);
    case MatchKindNone:
      return false;
  }

  return false;
}

// the first match on a line at or after from.
static bool
FirstOnLine(const MatchSet *set, const Buffer *buffer, u64 line, u64 from, Range *found)
{
  PatternMatch match;
  if (!MatchInLine(set, buffer, line, from, &match)) return false;

  *found = (Range){{line, match.start}, {line, match.end}};
  return true;
}

// the last match on a line that starts before before.
static bool
LastOnLine(const MatchSet *set, const Buffer *buffer, u64 line, u64 before, Range *found)
{
  PatternMatch match;
  bool any = false;
  u64 from = 0;

  while (MatchInLine(set, buffer, line, from, &match) && match.start < before) {
    *found = (Range){{line, match.start}, {line, match.end}};
    any = true;
    from = match.end;
  }

  return any;
}

static bool
FindForward(
  const MatchSet *set,
  const Buffer *buffer,
  Position from,
  Range *found,
  bool *wrapped
)
{
  u64 lineCount = BufferLineCount(buffer);

  for (u64 line = from.line; line < lineCount; line++) {
    if (FirstOnLine(set, buffer, line, line == from.line ? from.byte : 0, found)) return true;
  }

  *wrapped = true;

  for (u64 line = 0; line <= from.line; line++) {
    if (FirstOnLine(set, buffer, line, 0, found)) return true;
  }

  return false;
}

static bool
FindBackward(
  const MatchSet *set,
  const Buffer *buffer,
  Position from,
  Range *found,
  bool *wrapped
)
{
  for (u64 line = from.line + 1; line-- > 0;) {
    u64 before = line == from.line ? from.byte : UINT64_MAX;
    if (LastOnLine(set, buffer, line, before, found)) return true;
  }

  *wrapped = true;

  for (u64 line = BufferLineCount(buffer); line-- > from.line;) {
    if (LastOnLine(set, buffer, line, UINT64_MAX, found)) return true;
  }

  return false;
}

bool
MatchFind(
  const MatchSet *set,
  const Buffer *buffer,
  Position from,
  bool backward,
  Range *found,
  bool *wrapped
)
{
  *wrapped = false;
  if (set->kind == MatchKindNone) return false;
  if (backward) return FindBackward(set, buffer, from, found, wrapped);
  return FindForward(set, buffer, from, found, wrapped);
}

u64
MatchCount(const MatchSet *set, const Buffer *buffer, u64 limit)
{
  u64 count = 0;

  for (u64 line = 0; line < BufferLineCount(buffer) && count < limit; line++) {
    PatternMatch match = {};
    while (count < limit && MatchInLine(set, buffer, line, match.end, &match)) count++;
  }

  return count;
}
