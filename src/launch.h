#pragma once

// opening a new foot window in a given folder.
// the window is not a child of the editor: it outlives it, and the editor never waits for
// it.
#include "strbuf.h"
#include "types.h"

// starts a foot window in folder, an absolute path to a folder that exists, and appends
// what it opened, or why it could not, to message.
bool
LaunchTerminal(const char *folder, StrBuf *message);
