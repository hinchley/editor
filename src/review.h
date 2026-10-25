#pragma once

// review: the commands on changes since the last git commit (see changes.h): stepping
// through the changes of the current buffer (alt+shift+] and alt+shift+[), a panel beside
// it showing the change at the cursor (alt+g), restoring that change (alt+z),
// and a picker of every change in the loaded files, with the lines each one changes
// (ctrl+g).
#include "types.h"

// goes to the start of the next change below the cursor's line, or (backward) the closest
// one above it, wrapping around the buffer.
void
ReviewStep(bool backward);

// shows or hides the change panel; width is the window's, which needs minSplitWidth
// columns for the panel to show.
void
ReviewTogglePanel(i32 width);

// puts the lines of the change at the cursor back as they are in the last commit, as one
// undo step.
void
ReviewRevert(void);

bool
ReviewPanelOpen(void);

// draws the change at the current buffer's primary cursor in the area from row 0 and
// column, of width and height cells, wrapping its lines when soft wrap is on.
void
ReviewDrawPanel(i32 column, i32 width, i32 height);

// lists every change in the loaded files since the last commit, reading those that are
// not open in a scan, and sums up the lines they change in the message bar.
void
ReviewPickChanged(void);
