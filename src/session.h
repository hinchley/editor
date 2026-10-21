#pragma once

// the commands that change which buffers are loaded or current: loading the command line,
// new buffers, opening files and folders, switching between buffers, the buffer pickers,
// closing a buffer, the split view, and quitting.
// every command reports in the message bar, and the ones that need an answer ask in a
// prompt or picker.
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

typedef void (*SessionQuitter)(void);

// loads the paths from the command line into workspace and makes one of them current.
// this happens before the terminal changes, so a path that cannot be used, or more than
// maxFileCount files, ends the program with a message and the terminal untouched.
// with no paths there is one unnamed buffer.
// several paths, or any folder, open the buffer picker with the first file that could be
// opened behind it.
void
SessionStart(Workspace *workspace, StrBuf *message, char *const *paths, u64 pathCount);

// adds an unnamed buffer and makes it current.
void
SessionNew(void);

// asks for a file or folder to open, with tab completion.
// a file is added (or found among the loaded ones) and made current.
// for a folder it asks whether to add its files to the buffers (those not loaded already)
// or replace every buffer with them, after offering to save unsaved changes; with only
// unnamed buffers without changes it replaces them without asking.
void
SessionOpen(void);

// the folder the buffers were loaded from (at startup or by Ctrl-O), as given, or nullptr
// when there was none or several.
const char *
SessionFolder(void);

// walks the folders the buffers were loaded from again (at startup or by Ctrl-O) and adds
// the files that appeared in them since, leaving the current buffer alone.
// files that were already found by the last walk are not added again, so a closed buffer
// stays closed.
void
SessionLoadNew(void);

// closes a folder the buffers were loaded from (at startup or by Ctrl-O): with one, that
// one, and with several, the one picked from a list (the folder of the current buffer
// selected).
// every buffer of a file inside it is closed, after offering to save their unsaved
// changes, and Alt-Shift-R no longer walks it.
// closing every buffer leaves a new unnamed one.
void
SessionCloseFolder(void);

// switches to the next (step 1) or previous (step -1) open buffer in load order.
void
SessionCycle(i32 step);

// switches to the buffer used before the current one.
void
SessionLastUsed(void);

// opens the buffer picker over every buffer, or only those with unsaved changes.
// buffers are listed most recently used first, then by path.
void
SessionPick(bool unsavedOnly);

// closes the current buffer, removing it from the list, after offering to save unsaved
// changes, and switches to the buffer used before it.
// closing the last buffer leaves a new unnamed one.
void
SessionClose(void);

// splits a window of width columns, showing the current buffer (or, with fresh, a new
// unnamed one) in a new view on the right that takes focus.
// refused when the window is already split or narrower than minSplitWidth.
void
SessionSplit(bool fresh, i32 width);

// moves focus to the other view of the split.
void
SessionFocusOther(void);

// swaps the two sides of the split.
void
SessionSwapViews(void);

// closes the focused view of the split; its buffer stays loaded.
void
SessionCloseView(void);

// runs quit, first offering to save unsaved changes.
// quit only runs if every save succeeds.
void
SessionQuit(SessionQuitter quit);
