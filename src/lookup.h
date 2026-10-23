#pragma once

// lookup: the commands that ask the buffer's language server about the code at the
// cursor: where its symbol is defined (alt+d), what it is (alt+shift+d), where it is used
// (alt+u), and which file pairs with the buffer, a header's source or a source's header
// (alt+a, clangd only).
// the server answers in its own time; an answer that comes after the cursor moved, the
// text changed or another buffer was made current is dropped.
// without a server to answer, or when it knows no definition or signature, the first two
// fall back on the symbol index (see symbols.h).
#include "types.h"

// goes to the definition of the symbol at the cursor: straight there when there is one,
// or through a picker of places.
void
LookupDefinition(void);

// shows the declaration of the symbol at the cursor in the message bar, on one line cut
// to width columns.
void
LookupSignature(i32 width);

// lists every use of the symbol at the cursor (its declaration too), the current file's
// first, starting on the one at the cursor.
void
LookupReferences(void);

// switches to the source file of a header, or the header of a source file.
void
LookupOtherFile(void);
