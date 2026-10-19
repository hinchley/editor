#pragma once

// issues: the commands on the diagnostics language servers report (see diagnostics.h):
// stepping through those of the current buffer (ctrl+shift+] and ctrl+shift+[), a panel
// beside it showing in full the ones on the cursor's line (alt+k), and a picker of every
// one in the open buffers (alt+shift+k).
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

// the commands act on workspace and report to message.
void
IssuesInit(Workspace *workspace, StrBuf *message);

// goes to the start of the next diagnostic after the cursor, or (backward) the closest one
// before it, wrapping around the buffer, and says what it is.
void
IssuesStep(bool backward);

// shows or hides the panel; width is the window's, which needs minSplitWidth columns for
// the panel to show.
void
IssuesTogglePanel(i32 width);

bool
IssuesPanelOpen(void);

// draws the diagnostics reaching the current buffer's cursor line in the area from row 0
// and column, of width and height cells: each one's severity, code and source, its message
// and its notes, wrapped to the width.
void
IssuesDrawPanel(i32 column, i32 width, i32 height);

// while the panel shows, asks the language server what the symbol at the cursor is once
// the cursor has rested for hoverDelay, for the panel to show below the diagnostics.
void
IssuesTick(void);

// milliseconds until IssuesTick has something to do, or -1 when it waits for nothing.
i32
IssuesTimeout(void);

// lists the diagnostics of every open buffer as "file:line: message", with a preview.
void
IssuesPick(void);
