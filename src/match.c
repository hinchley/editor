#include "match.h"

#include "spell.h"

bool
MatchInLine(const MatchSet *set, const char *text, u64 length, u64 from, PatternMatch *match)
{
  switch (set->kind) {
  case MatchKindSearch: return PatternFind(set->pattern, text, length, from, match);
  case MatchKindSpell: return SpellFind(text, length, from, &match->start, &match->end);
  case MatchKindNone: return false;
  }

  return false;
}

// the first match on a line at or after from.
static bool
FirstOnLine(const MatchSet *set, const Buffer *buffer, u64 line, u64 from, Range *found)
{
  const StrBuf *text = BufferLine(buffer, line);
  PatternMatch match;
  if (!MatchInLine(set, text->data, text->length, from, &match)) return false;

  *found = (Range){{line, match.start}, {line, match.end}};
  return true;
}

// the last match on a line that starts before before.
static bool
LastOnLine(const MatchSet *set, const Buffer *buffer, u64 line, u64 before, Range *found)
{
  const StrBuf *text = BufferLine(buffer, line);
  PatternMatch match;
  bool any = false;
  u64 from = 0;

  while (MatchInLine(set, text->data, text->length, from, &match) && match.start < before) {
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
) {
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
) {
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
) {
  *wrapped = false;
  if (set->kind == MatchKindNone) return false;
  if (backward) return FindBackward(set, buffer, from, found, wrapped);
  return FindForward(set, buffer, from, found, wrapped);
}

u64
MatchCount(const MatchSet *set, const Buffer *buffer)
{
  u64 count = 0;

  for (u64 line = 0; line < BufferLineCount(buffer); line++) {
    const StrBuf *text = BufferLine(buffer, line);
    PatternMatch match = {};

    while (MatchInLine(set, text->data, text->length, match.end, &match)) count++;
  }

  return count;
}
