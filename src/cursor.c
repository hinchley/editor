#include "cursor.h"

#include <stdlib.h>

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
