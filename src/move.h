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
