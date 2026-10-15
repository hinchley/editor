#pragma once

// symbols: the definitions a language's tags query finds (functions, types, headings and
// so on, see ARCHITECTURE.md), and the commands that go to them in the current buffer:
// the alt+o picker and alt+] and alt+[ to the next or previous definition.
// each records a jump, so alt+left comes back.
#include "strbuf.h"
#include "workspace.h"

// the commands act on workspace and report to message.
void
SymbolsInit(Workspace *workspace, StrBuf *message);

// opens a picker of the current buffer's symbols in the order they are defined, with the
// last one starting at or before the cursor selected, and goes to the name picked.
void
SymbolsPickFile(void);

// goes to the start of the next definition below the cursor's line, or (backward) the
// start of the closest one before the cursor.
void
SymbolsStep(bool backward);
