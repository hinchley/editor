#pragma once

// editing commands.
// each command edits at every cursor of a view and is recorded in the undo history as one
// step, so an edit made with several cursors undoes in one go.
#include "strbuf.h"
#include "types.h"
#include "undo.h"
#include "view.h"

// what a command edits: the view (its buffer and cursors) and the buffer's history.
typedef struct EditContext {
  View *view;
  Undo *undo;

  // indentation inserts spaces instead of a tab character.
  bool expandTab;
} EditContext;

// replaces each selection with typed text.
void
EditInsert(const EditContext *edit, const char *text, u64 length);

// pastes text at each cursor, with "\r\n" turned into "\n".
// line wise text (whole lines copied without a selection) goes above the cursor's line,
// unless the cursor has a selection, which the text replaces.
// with several cursors and as many lines of text, each cursor gets one line, in order.
void
EditPaste(const EditContext *edit, const char *text, u64 length, bool lineWise);

// breaks the line, carrying its indentation over, plus one level after an opening
// bracket, and opens an indented block when the cursor sits between a pair of brackets.
void
EditNewline(const EditContext *edit);

// opens an empty line below each cursor's line, as a line break at its end would, or above
// it, with the line's indentation, and puts the cursor on it.
void
EditOpenLine(const EditContext *edit, bool above);

// indents every line touched by a selection, or without any selection inserts a tab (or
// spaces up to the next tab stop) at each cursor.
void
EditIndent(const EditContext *edit);

// removes one level of indentation from every line touched by a cursor.
void
EditDedent(const EditContext *edit);

// comments out every line touched by a cursor, putting start (and end, unless it is
// empty) around it, or removes the comments when every such line that is not blank has
// one already.
// blank lines are left alone, unless there are only blank lines.
void
EditToggleComment(const EditContext *edit, const char *start, const char *end);

// deletes each selection, or the character before each cursor.
// in leading spaces with expandTab, it deletes back to the previous tab stop.
void
EditDeleteBackward(const EditContext *edit);

// deletes each selection, or the character after each cursor.
void
EditDeleteForward(const EditContext *edit);

// deletes each selection, or back to the start of the previous word.
void
EditDeleteWordBackward(const EditContext *edit);

// deletes each selection, or on to the start of the next word.
void
EditDeleteWordForward(const EditContext *edit);

// the line commands below act on every line a cursor's selection touches, or its line,
// taking the runs of lines of every cursor together.

// copies each run of lines above itself, so the cursors keep their places on the lower
// copy.
void
EditDuplicateLines(const EditContext *edit);

// deletes the lines of the primary cursor, collapsing to it first.
void
EditDeleteLines(const EditContext *edit);

// moves each run of lines up or down past its neighbour, keeping the cursors on them.
// returns false, moving nothing, when a run is at the start or end of the buffer.
bool
EditMoveLines(const EditContext *edit, bool up);

// what sort orders lines by: their text, or their display width (tabs to the next tab
// stop, wide characters as two columns) with text order breaking ties.
enum SortKey {
  SortKeyText,
  SortKeyLength
};

// how sort orders lines.
// text compares bytes, or with ignoreCase code points after simple case folding with byte
// order breaking ties.
// descending reverses the order of the key only, so lines of equal width stay in ascending
// text order.
// unique, for the text key only, keeps one of each run of equal lines (equal ignoring
// case with ignoreCase).
typedef struct SortOptions {
  enum SortKey key;
  bool descending;
  bool unique;
  bool ignoreCase;
} SortOptions;

// what a sort did: the number of lines sorted and how many duplicates it removed.
typedef struct SortResult {
  u64 lines;
  u64 removed;
} SortResult;

// sorts the lines the newest cursor's selection touches and selects them.
// the lines are replaced in one go, so cursors of another view inside them move to their
// start.
SortResult
EditSortLines(const EditContext *edit, SortOptions options);

// joins the selected lines, or the line with the next one, and returns false when there
// is no next line.
// each join replaces the line break and the next line's leading blanks with one space,
// or with nothing when either side of the join is empty, the line ends in a blank, or the
// next line starts with a closing bracket.
bool
EditJoinLines(const EditContext *edit);

// copies each selection (joined with "\n") into text, or without any selection the line
// of each cursor, each ending in "\n".
// returns true when the copy is line wise.
bool
EditCopy(View *view, StrBuf *text);

// copies like EditCopy, then deletes what was copied.
bool
EditCut(const EditContext *edit, StrBuf *text);

// removes spaces and tabs from the end of every line and returns how many lines changed.
u64
EditTrimTrailingWhitespace(const EditContext *edit);

// removes spaces and tabs from the end of the lines each cursor touches and returns how
// many lines changed.
u64
EditTrimLines(const EditContext *edit);

// converts each selection to upper or lower case with the simple one to one mappings of
// Utf8Upper and Utf8Lower, and returns how many lines changed.
// only the changed part of each line is replaced, and each cursor stays on the same code
// points (with its direction) even when a conversion changes their byte length.
u64
EditChangeCase(const EditContext *edit, bool upper);

// lines up the cursors in columns by inserting spaces before them, and returns how many
// lines changed.
// the first cursor on each line (the start of its selection) is padded out to the display
// column of the furthest one, then the second cursor of each line that has one, and so on,
// each needing cursors on two or more lines.
// cursors and selections move along with their text, and the whole alignment is one undo
// step.
u64
EditAlignCursors(const EditContext *edit);

// undo and redo return false when there is nothing to undo or redo.
bool
EditUndo(const EditContext *edit);

bool
EditRedo(const EditContext *edit);
