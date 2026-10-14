#pragma once

// the matches highlighted in a buffer: those of a search pattern, or unknown words.
// matches are never stored.
// they are found again from the text whenever they are drawn or stepped through, so
// edits can never leave them out of date.
#include "buffer.h"
#include "highlight.h"
#include "pattern.h"
#include "types.h"

enum MatchKind {
  MatchKindNone,
  MatchKindSearch,
  MatchKindSpell
};

typedef struct MatchSet {
  enum MatchKind kind;

  // the pattern of a search set.
  const Pattern *pattern;

  // the highlighter of a spell set, which limits it to the text spell check looks at, or
  // nullptr to check everything.
  Highlight *scope;
} MatchSet;

// finds the first match in a line of a buffer that starts at or after from.
bool
MatchInLine(
  const MatchSet *set,
  const Buffer *buffer,
  u64 line,
  u64 from,
  PatternMatch *match
);

// finds the first match starting at or after from, or with backward the last one starting
// before it, going round the end of the buffer at most once.
// wrapped tells whether it did.
bool
MatchFind(
  const MatchSet *set,
  const Buffer *buffer,
  Position from,
  bool backward,
  Range *found,
  bool *wrapped
);

// counts every match in the buffer.
u64
MatchCount(const MatchSet *set, const Buffer *buffer);
