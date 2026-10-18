#pragma once

// preview: draws the lines of a loaded entry around one line, for a picker's preview of
// project search matches or symbols.
// an open entry is drawn from its buffer, and any other from its file, read again from
// disk (keeping the last one read).
#include "pattern.h"
#include "types.h"
#include "workspace.h"

void
PreviewInit(Workspace *workspace);

// draws the lines around line of the entry with serial in the area from row 0 and column,
// of width and height cells: a header naming the entry and the line, then the lines with
// their numbers, wrapped when soft wrap is on, with the bytes from start to end of that
// line picked out and kept in view (in the middle when wrapped, or else by scrolling
// sideways).
// when pattern is not nullptr, its matches are highlighted as well, and the bytes from
// start to end are picked out only where they match.
void
PreviewDraw(
  u64 serial,
  u64 line,
  u64 start,
  u64 end,
  const Pattern *pattern,
  i32 column,
  i32 width,
  i32 height
);

// forgets the file kept from the last preview.
void
PreviewForget(void);
