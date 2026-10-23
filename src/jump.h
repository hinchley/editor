#pragma once

// jump history: where the cursor was before each jump (go to line, start or end of file,
// a project search match, a symbol), so alt+[ and alt+] can go back and forward
// through those places, across buffers.
// places are kept by entry serial and position, and do not follow edits: a position is
// moved inside the text when it is used, and a place in a closed buffer is skipped.
// recording a jump after going back drops the places ahead, as undo drops redo.
#include "types.h"

// records the current place, as a command is about to jump away from it.
void
JumpRecord(void);

// goes to the place before the current one in the history.
// returns false, with a message, when there is none.
bool
JumpBack(void);

// goes to the place after the current one, after going back.
// returns false, with a message, when there is none.
bool
JumpForward(void);
