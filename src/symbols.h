#pragma once

// symbols: the definitions a language's tags query finds (functions, types, headings and
// so on, see ARCHITECTURE.md), and the commands that go to them: in the current buffer
// the alt+s picker and ctrl+] and ctrl+[ to the next or previous definition, and across
// the loaded files go to definition (alt+d), the signature (alt+shift+d) and the
// alt+shift+s picker.
// each records a jump, so alt+[ comes back.
// the commands across files use an index of the symbols of every loaded file, built by a
// scan (see scan.h) the first time one is used.
// open documents are indexed again from their buffer whenever they changed, and a name
// that is not found first indexes again the files that changed on disk.
#include "strbuf.h"
#include "workspace.h"

// the commands act on workspace and report to message.
void
SymbolsInit(Workspace *workspace, StrBuf *message);

// opens a picker of the current buffer's symbols in the order they are defined, and goes
// to the name picked.
// the symbol named by the word at the cursor is selected (its definition rather than a
// prototype), or else the last one starting at or before the cursor.
void
SymbolsPickFile(void);

// goes to the start of the next definition below the cursor's line, or (backward) the
// start of the closest one before the cursor.
void
SymbolsStep(bool backward);

// goes to the definition of the name at the cursor: straight there when there is one, or
// through a picker with a preview, the current file's first.
// on a definition it goes to the prototypes, which otherwise count only when there is no
// definition.
void
SymbolsGoToDefinition(void);

// shows the signature of the name at the cursor, or else of the name before the innermost
// "(" left open before the cursor on its line, shortened to width columns.
// one in the current file is shown first, then a prototype, then a definition.
void
SymbolsShowSignature(i32 width);

// opens a picker of every symbol in the loaded files, by name, with a preview.
void
SymbolsPickProject(void);

// forgets the index, as when a folder replaces every buffer.
void
SymbolsClear(void);
