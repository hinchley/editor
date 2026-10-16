#pragma once

// review: the commands on changes since the last git commit (see changes.h): stepping
// through the changes of the current buffer (alt+shift+] and alt+shift+[), a panel beside
// it showing the change at the cursor (alt+shift+g), and a picker of the loaded files that
// differ from the last commit, with the lines they change (alt+u).
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

// the commands act on workspace and report to message.
void
ReviewInit(Workspace *workspace, StrBuf *message);

// goes to the start of the next change below the cursor's line, or (backward) the closest
// one above it, wrapping around the buffer.
void
ReviewStep(bool backward);

// shows or hides the change panel; width is the window's, which needs minSplitWidth
// columns for the panel to show.
void
ReviewTogglePanel(i32 width);

bool
ReviewPanelOpen(void);

// draws the change at the current buffer's primary cursor in the area from row 0 and
// column, of width and height cells.
void
ReviewDrawPanel(i32 column, i32 width, i32 height);

// lists the loaded files whose text differs from the last commit, reading those that are
// not open in a scan, and sums up the lines they change in the message bar.
void
ReviewPickChanged(void);
