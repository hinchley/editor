#pragma once

// commands that add cursors: at the next occurrence of the selected text, or on the row
// above or below the others.
// the new cursor becomes the primary one, so the view follows it.
#include "strbuf.h"
#include "view.h"

// with no selection at the primary cursor, selects the word at every cursor.
// otherwise adds a cursor selecting the next occurrence of the primary selection after it,
// going round the end of the buffer and passing over text other cursors already hold.
// the text matches case, and as a whole word when the selection is one.
// reports to message when there is no word or no other occurrence.
void
MultiCursorAddNext(View *view, StrBuf *message);

// adds a cursor on the visual row above the first cursor, or below the last one (above or
// below its whole selection), at that cursor's desired column.
// returns false at the start or end of the buffer.
bool
MultiCursorAddRow(View *view, bool up);
