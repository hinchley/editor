#pragma once

// cursor movement.
// every cursor of the view moves, and vertical movement works on visual rows, so the same
// code serves wrapped and unwrapped views.
#include "view.h"

enum Move {
  MoveUp,
  MoveDown,
  MoveLeft,
  MoveRight,
  MoveLineStart,
  MoveLineEnd,
  MovePageUp,
  MovePageDown,
  MoveWordLeft,
  MoveWordRight,
  MoveFileStart,
  MoveFileEnd
};

// moves the cursors and scrolls the view to keep the primary cursor in sight.
// with extend the anchors stay put, growing the selections.
void
MoveCursors(View *view, enum Move move, bool extend);

// where a movement would take a cursor's head.
Position
MovePosition(View *view, const Cursor *cursor, enum Move move);

// finds the word (a run of word characters) at or just before byte at in a line.
// returns false when there is none.
bool
MoveWordAround(const StrBuf *line, u64 at, u64 *start, u64 *end);

// collapses to the primary cursor and gives it a new anchor and head, scrolling to it.
void
MoveSelect(View *view, Position anchor, Position head);

// selects the whole buffer with a single cursor.
void
MoveSelectAll(View *view);

// collapses to the primary cursor and puts it on the bracket matching the one at (or just
// before) it.
// returns false, changing nothing, when there is no such bracket.
bool
MoveToMatchingBracket(View *view);

// collapses to the primary cursor and selects the text inside the closest brackets around
// its selection, or the brackets as well when that text is already selected.
// returns false, changing nothing, when no brackets enclose the selection.
bool
MoveSelectBrackets(View *view);

// collapses to the primary cursor at a line and display column (both from 0), clamped to
// the buffer.
// a column inside a tab or wide character lands on its start.
void
MoveGoTo(View *view, u64 line, u64 column);
