#pragma once

// undo and redo history of one buffer.
// the history listens to the buffer and records every change made between UndoBegin and
// UndoEnd as one group, which undoes and redoes in a single step.
// each group also keeps the cursors from before and after it, so undo puts them back.
// consecutive typing, backspace or delete edits join the previous group until something
// else happens (UndoBreak).
#include "buffer.h"
#include "cursor.h"
#include "strbuf.h"
#include "types.h"

enum UndoKind {
  // never joins another group.
  UndoKindOther,
  UndoKindTyping,
  UndoKindDeleteBackward,
  UndoKindDeleteForward
};

// one replacement: oldText at start became newText.
typedef struct UndoRecord {
  Position start;
  StrBuf oldText;
  StrBuf newText;
} UndoRecord;

typedef struct UndoGroup {
  // tells the group apart from every other group of every buffer: ids are never reused.
  u64 id;
  UndoRecord *records;
  u32 count;
  u32 capacity;
  enum UndoKind kind;
  CursorSet before;
  CursorSet after;
} UndoGroup;

typedef struct Undo {
  Buffer *buffer;

  // groups before current are applied, and the ones from current on can be redone.
  UndoGroup *groups;
  u64 count;
  u64 capacity;
  u64 current;

  // the value of current when the text matched the file, or undoNoSavePoint once that
  // state can no longer be reached.
  u64 savePoint;

  // nesting depth of UndoBegin, the kind and cursors given to the outermost one, and
  // whether a change has arrived since.
  u32 depth;
  enum UndoKind kind;
  CursorSet before;
  bool started;

  // the last group may take the next edit of the same kind.
  bool open;

  // set while undo or redo replays changes, which are not recorded.
  bool replaying;
} Undo;

static constexpr u64 undoNoSavePoint = UINT64_MAX;

// starts an empty history for buffer, with its current text as the saved state.
void
UndoInit(Undo *undo, Buffer *buffer);

void
UndoFree(Undo *undo);

// forgets every group and makes the current text the saved state.
void
UndoClear(Undo *undo);

// starts recording one edit; cursors are where the cursors are before it.
// calls may nest, and only the outermost pair forms a group.
// a change to the buffer outside UndoBegin and UndoEnd is a programming error.
void
UndoBegin(Undo *undo, enum UndoKind kind, const CursorSet *cursors);

// ends the edit; cursors are where the cursors are after it.
void
UndoEnd(Undo *undo, const CursorSet *cursors);

// stops the next edit from joining the last group.
void
UndoBreak(Undo *undo);

// undoes the last group and puts the cursors back as they were before it.
// returns false when there is nothing to undo.
bool
UndoUndo(Undo *undo, CursorSet *cursors);

// redoes the next group and puts the cursors where they were after it.
// returns false when there is nothing to redo.
bool
UndoRedo(Undo *undo, CursorSet *cursors);

// the id of the group undo would undo, or 0 when there is none.
u64
UndoLastId(const Undo *undo);

// the index of the group with id, or undo->count when the history no longer has it.
u64
UndoFind(const Undo *undo, u64 id);

// marks the current text as the one on disk.
void
UndoMarkSaved(Undo *undo);

// true when the text is the one last loaded or saved.
static inline bool
UndoAtSavePoint(const Undo *undo)
{
  return undo->current == undo->savePoint;
}
