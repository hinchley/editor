#pragma once

// what the mouse does to a view: a press of the left button puts the cursor where it lands,
// or with alt adds a cursor there (or removes the one there); a second press on the same
// cell selects the word there, and a third the line, as a press in the gutter does at
// once; dragging from the press selects the text passed over, by words or lines after a
// second or third press, or with ctrl+alt the same columns of every row passed over.
// the app finds the view under the pointer, so nothing here holds on to a view between
// events.
#include "view.h"

// what a press of the left button does.
enum MouseMode {
  // puts a single cursor down.
  MouseModePlace,

  // adds a cursor, or removes the one there.
  MouseModeAdd,

  // puts a single cursor down, and a drag from it gives every row from the press to the
  // pointer a cursor selecting the same columns.
  MouseModeColumn
};

// a press of the left button at a screen cell, which starts a drag from any cursor it puts
// down.
// a press on the same cell soon after one that put a cursor down selects the word there
// with that cursor, and the press after that the line; a press in the gutter selects its
// line straight away.
void
MousePress(View *view, i32 row, i32 column, enum MouseMode mode);

// grows the selection of the cursor the press put down to the screen cell, from the text
// the press chose, or gives every row passed over its cursor, while a drag is going on.
// a cell outside the view reaches the text beyond its edge.
// a cursor the selection runs into is merged into it.
void
MouseDrag(View *view, i32 row, i32 column);

// ends a drag, leaving its selection.
void
MouseRelease(void);

// ends a drag and forgets the last press, so the next one starts a new click, as anything
// but the mouse does: neither the drag nor the press follows edits.
void
MouseInterrupt(void);
