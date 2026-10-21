#pragma once

// opening a new window of the terminal the editor runs in, in a given folder.
// the terminal is found from the editor's parent processes (foot, alacritty or ghostty),
// or else named by $TERMINAL.
// the window is not a child of the editor: it outlives it, and the editor never waits for
// it.
#include "strbuf.h"
#include "types.h"

// starts a terminal window in folder, an absolute path to a folder that exists, and appends
// what it opened, or why it could not, to message.
bool
LaunchTerminal(const char *folder, StrBuf *message);
