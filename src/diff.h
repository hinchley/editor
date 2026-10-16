#pragma once

// diff: the lines that differ between two texts, as a list of hunks.
// lines are compared as whole byte strings. the common start and end are passed over
// first, then the rest is compared with Myers' algorithm (finding the middle of an
// optimal path and dividing there), which gives a shortest edit script.
// a part that would cost more than a limit to compare is given up on and reported as one
// hunk, so two unrelated texts take bounded time.
#include "types.h"

// count lines from oldStart of the old text replaced by newCount lines from newStart of
// the new one. one of the counts may be zero (lines added, or lines removed).
typedef struct DiffHunk {
  u64 oldStart;
  u64 oldCount;
  u64 newStart;
  u64 newCount;
} DiffHunk;

typedef struct DiffHunks {
  DiffHunk *hunks;
  u64 count;
  u64 capacity;
} DiffHunks;

// one side of a diff: its line count, and a function giving the text of a line (which
// must stay valid while the diff runs).
typedef struct DiffSource {
  u64 count;
  void (*line)(const void *context, u64 index, const char **text, u64 *length);
  const void *context;
} DiffSource;

// replaces out's hunks with those between the old and new sides, in order, with no two
// adjacent.
// maxCost bounds the edit distance looked for in any part.
void
DiffRun(const DiffSource *old, const DiffSource *new, u64 maxCost, DiffHunks *out);

void
DiffFree(DiffHunks *hunks);
