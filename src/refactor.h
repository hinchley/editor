#pragma once

// refactor: the commands that change code as the buffer's language server says: renaming
// the symbol at the cursor wherever it is used (alt+shift+n), taking a fix for a
// diagnostic on the cursor's line (alt+y), and formatting the buffer (alt+shift+i).
// the changes go into each file's buffer as one undo step, opening the files that are not
// open, and nothing is saved.
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

// the commands act on workspace and report to message.
void
RefactorInit(Workspace *workspace, StrBuf *message);

// asks for a new name for the symbol at the cursor, starting from its name, and renames it.
void
RefactorRename(void);

// makes the fix the server offers for the diagnostics on the cursor's line, or picks one
// of several.
void
RefactorFix(void);

// formats the buffer, indenting with spaces when spaces is true or else with tabs.
void
RefactorFormat(bool spaces);
