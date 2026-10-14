#pragma once

// temporary key check screen for milestone M1.
// it names every key the terminal sends and tracks which bindings have been seen, so the
// keyboard handling can be verified in each terminal.
// it is replaced by the editor views in M3.
#include "input.h"
#include "types.h"

// handles one input event.
// returns false when the user asks to quit.
bool
KeyCheckHandle(const InputEvent *event);

// records that the window was resized.
void
KeyCheckResized(void);

// draws the whole screen into the screen grid.
void
KeyCheckDraw(i32 width, i32 height);
