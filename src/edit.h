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

// the line commands below first collapse to the primary cursor, and act on its line or
// every line its selection touches.

// copies the lines below themselves and moves the selection onto the copy.
void
EditDuplicateLines(const EditContext *edit);

void
EditDeleteLines(const EditContext *edit);

// moves the lines up or down past their neighbour, keeping them selected.
// returns false at the start or end of the buffer.
bool
EditMoveLines(const EditContext *edit, bool up);

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

// undo and redo return false when there is nothing to undo or redo.
bool
EditUndo(const EditContext *edit);

bool
EditRedo(const EditContext *edit);
