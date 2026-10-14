#pragma once

// copy and paste through the wayland clipboard (wl-copy and wl-paste), with an in-process
// copy as the fallback when there is no wayland session or the commands fail.
// the clipboard also remembers whether the last copy was line wise (whole lines copied
// without a selection), so pasting the same text again can insert whole lines.
#include "strbuf.h"
#include "types.h"

// copies text and returns false when only the in-process copy could be set.
bool
ClipboardSet(const char *text, u64 length, bool lineWise);

// appends the clipboard text and sets lineWise when it is the text of the last line wise
// copy.
// returns false when only the in-process copy could be read.
bool
ClipboardGet(StrBuf *text, bool *lineWise);
