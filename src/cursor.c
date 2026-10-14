#include "cursor.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"

void
CursorSetInit(CursorSet *set)
{
  set->capacity = 4;
  set->cursors = MemoryAllocateZeroed(set->capacity, sizeof(Cursor));
  set->count = 1;
  set->primary = 0;
}

void
CursorSetFree(CursorSet *set)
{
  free(set->cursors);
  *set = (CursorSet){};
}

void
CursorSetCopy(CursorSet *target, const CursorSet *source)
{
  if (target->capacity < source->count) {
    target->cursors = MemoryResize(target->cursors, source->count, sizeof(Cursor));
    target->capacity = source->count;
  }

  memcpy(target->cursors, source->cursors, source->count * sizeof(Cursor));
  target->count = source->count;
  target->primary = source->primary;
}

void
CursorSetAdd(CursorSet *set, Cursor cursor)
{
  if (set->count == set->capacity) {
    set->capacity = set->capacity ? set->capacity * 2 : 4;
    set->cursors = MemoryResize(set->cursors, set->capacity, sizeof(Cursor));
  }

  set->primary = set->count;
  set->cursors[set->count++] = cursor;
}

void
CursorSetCollapse(CursorSet *set)
{
  set->cursors[0] = set->cursors[set->primary];
  set->count = 1;
  set->primary = 0;
}

void
CursorSetAdjust(CursorSet *set, const BufferChange *change)
{
  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    cursor->anchor = BufferAdjust(cursor->anchor, change);
    cursor->head = BufferAdjust(cursor->head, change);
  }
}

void
CursorSetClamp(CursorSet *set, const Buffer *buffer)
{
  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    cursor->anchor = BufferClamp(buffer, cursor->anchor);
    cursor->head = BufferClamp(buffer, cursor->head);
  }
}

static i32
CompareCursors(const void *a, const void *b)
{
  return PositionCompare(CursorRange(a).start, CursorRange(b).start);
}

// true when next (which starts at or after cursor) should become part of cursor.
// selections that only touch stay apart, but a cursor without a selection joins anything
// it meets.
static bool
Overlaps(const Cursor *cursor, const Cursor *next)
{
  i32 order = PositionCompare(CursorRange(next).start, CursorRange(cursor).end);
  if (order < 0) return true;
  if (order > 0) return false;
  return !CursorHasSelection(cursor) || !CursorHasSelection(next);
}

// grows cursor to cover next as well, keeping the direction of cursor.
static void
Join(Cursor *cursor, const Cursor *next)
{
  Range range = CursorRange(cursor);
  Range other = CursorRange(next);
  if (PositionCompare(other.end, range.end) > 0) range.end = other.end;

  bool backward = PositionCompare(cursor->head, cursor->anchor) < 0;
  cursor->anchor = backward ? range.end : range.start;
  cursor->head = backward ? range.start : range.end;
}

void
CursorSetMerge(CursorSet *set)
{
  if (set->count < 2) return;

  // the primary cursor is found again by value after sorting.
  Cursor primary = set->cursors[set->primary];
  qsort(set->cursors, set->count, sizeof(Cursor), CompareCursors);
  set->primary = 0;
  u32 kept = 0;

  for (u32 index = 0; index < set->count; index++) {
    Cursor *cursor = &set->cursors[index];
    bool isPrimary = memcmp(cursor, &primary, sizeof(Cursor)) == 0;

    if (index > 0 && Overlaps(&set->cursors[kept - 1], cursor)) {
      Join(&set->cursors[kept - 1], cursor);
      if (isPrimary) set->primary = kept - 1;
      continue;
    }

    set->cursors[kept] = *cursor;
    if (isPrimary) set->primary = kept;
    kept++;
  }

  set->count = kept;
}
