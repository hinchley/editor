#pragma once

// the cursors of a view.
// a view always has a set of cursors, with one of them primary, so commands are written
// once for any number of cursors.
#include "buffer.h"
#include "types.h"

// the selection runs from anchor to head, and is empty when they are equal.
// desiredColumn is the column within its visual row that vertical movement aims for, so
// moving through shorter rows does not lose it.
typedef struct Cursor {
  Position anchor;
  Position head;
  u64 desiredColumn;
} Cursor;

typedef struct CursorSet {
  Cursor *cursors;
  u32 count;
  u32 capacity;
  u32 primary;
} CursorSet;

// sets up a single cursor at the start of the buffer.
void
CursorSetInit(CursorSet *set);

void
CursorSetFree(CursorSet *set);

// makes target a copy of source.
void
CursorSetCopy(CursorSet *target, const CursorSet *source);

// adds a cursor and makes it the primary one.
void
CursorSetAdd(CursorSet *set, Cursor cursor);

// removes a cursor, which must not be the only one.
// when it was the primary one, the cursor before it (or else the new first) becomes primary.
void
CursorSetRemove(CursorSet *set, u32 index);

// keeps only the primary cursor.
void
CursorSetCollapse(CursorSet *set);

// moves every cursor to follow a change to the buffer.
void
CursorSetAdjust(CursorSet *set, const BufferChange *change);

// keeps every cursor inside the buffer, after its text was replaced without a change.
void
CursorSetClamp(CursorSet *set, const Buffer *buffer);

// true when any cursor has a selection.
bool
CursorSetHasSelection(const CursorSet *set);

// sorts the cursors by position and merges any that overlap or meet.
void
CursorSetMerge(CursorSet *set);

static inline Cursor *
CursorSetPrimary(CursorSet *set)
{
  return &set->cursors[set->primary];
}

static inline bool
CursorHasSelection(const Cursor *cursor)
{
  return PositionCompare(cursor->anchor, cursor->head) != 0;
}

// the selection in text order.
static inline Range
CursorRange(const Cursor *cursor)
{
  if (PositionCompare(cursor->anchor, cursor->head) <= 0) {
    return (Range){cursor->anchor, cursor->head};
  }

  return (Range){cursor->head, cursor->anchor};
}

// puts the cursor at a position with nothing selected.
static inline void
CursorPlace(Cursor *cursor, Position position)
{
  cursor->anchor = position;
  cursor->head = position;
}
