#pragma once

// commands that add cursors: at occurrences of the selection, or on the row above or below
// the others.
// a cursor added on its own becomes the primary one, so the view follows it.
#include "match.h"
#include "strbuf.h"
#include "view.h"

// an occurrence of the primary selection is one of the matches the document highlights
// (marks, a search pattern's matches or unknown words) when the selection is exactly one
// of them, and otherwise the selected text, matching case and as a whole word when the
// selection is one.
// a selection of text must be on one line.
// matches that a cursor other than the primary one holds are passed over.
// each reports to message when there is no word or no other occurrence.

// with no selection at the primary cursor, selects the word at every cursor.
// otherwise adds a cursor at the next occurrence after the primary selection, going round
// the end of the buffer.
void
MultiCursorAddNext(View *view, MatchSet marks, StrBuf *message);

// moves the primary cursor to the next occurrence, passing over the one it held.
// with no selection, first selects the word at every cursor.
void
MultiCursorSkip(View *view, MatchSet marks, StrBuf *message);

// adds a cursor at every occurrence in the buffer, keeping the primary cursor.
// with no selection, first selects the word at every cursor.
void
MultiCursorAddAll(View *view, MatchSet marks, StrBuf *message);

// adds a cursor on the visual row above the first cursor, or below the last one (above or
// below its whole selection), at that cursor's desired column.
// returns false at the start or end of the buffer.
bool
MultiCursorAddRow(View *view, bool up);
