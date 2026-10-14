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

static inline Cursor *
CursorSetPrimary(CursorSet *set)
{
  return &set->cursors[set->primary];
}
