#include "undo.h"

#include <stdlib.h>

#include "fatal.h"
#include "memory.h"

// the last id given to a group, in any buffer.
static u64 groupIds;

static void
FreeGroup(UndoGroup *group)
{
  for (u32 index = 0; index < group->count; index++) {
    StrBufFree(&group->records[index].oldText);
    StrBufFree(&group->records[index].newText);
  }

  free(group->records);
  CursorSetFree(&group->before);
  CursorSetFree(&group->after);
}

// frees the groups from first on.
static void
DropGroups(Undo *undo, u64 first)
{
  for (u64 index = first; index < undo->count; index++) FreeGroup(&undo->groups[index]);
  undo->count = first;
}

// joins change onto the last record when one simply continues the other, so typing a
// line costs one record instead of one per character.
static bool
Extend(UndoRecord *record, const BufferChange *change)
{
  bool insertion = change->oldLength == 0;
  bool deletion = change->newLength == 0;
  Position start = record->start;

  // typing straight after the text the record put in.
  if (insertion) {
    Position end = BufferTextEnd(start, StrBufText(&record->newText), record->newText.length);
    if (PositionCompare(change->start, end) != 0) return false;

    StrBufAppend(&record->newText, change->newText, change->newLength);
    return true;
  }

  if (!deletion || record->newText.length) return false;

  // backspace just before the text the record took out.
  if (PositionCompare(change->oldEnd, start) == 0) {
    StrBufInsert(&record->oldText, 0, change->oldText, change->oldLength);
    record->start = change->start;
    return true;
  }

  // delete at the same place again.
  if (PositionCompare(change->start, start) == 0) {
    StrBufAppend(&record->oldText, change->oldText, change->oldLength);
    return true;
  }

  return false;
}

static void
AddRecord(UndoGroup *group, const BufferChange *change)
{
  if (group->count && Extend(&group->records[group->count - 1], change)) return;

  if (group->count == group->capacity) {
    group->capacity = group->capacity ? group->capacity * 2 : 4;
    group->records = MemoryResize(group->records, group->capacity, sizeof(UndoRecord));
  }

  UndoRecord *record = &group->records[group->count++];
  *record = (UndoRecord){.start = change->start};

  // most records only insert or only delete, so the other text is left unallocated.
  if (change->oldLength) StrBufAppend(&record->oldText, change->oldText, change->oldLength);
  if (change->newLength) StrBufAppend(&record->newText, change->newText, change->newLength);
}

// finds the group the first change of an edit goes into: the last group when the edit
// continues it, or a new one, which discards anything that could be redone.
static void
StartGroup(Undo *undo)
{
  undo->started = true;

  bool atEnd = undo->current == undo->count && undo->count > 0;
  bool joins = undo->open && atEnd && undo->kind != UndoKindOther &&
    undo->groups[undo->count - 1].kind == undo->kind;
  if (joins) return;

  DropGroups(undo, undo->current);

  // the saved state was among the dropped groups.
  if (undo->savePoint != undoNoSavePoint && undo->savePoint > undo->current) {
    undo->savePoint = undoNoSavePoint;
  }

  if (undo->count == undo->capacity) {
    undo->capacity = undo->capacity ? undo->capacity * 2 : 64;
    undo->groups = MemoryResize(undo->groups, undo->capacity, sizeof(UndoGroup));
  }

  UndoGroup *group = &undo->groups[undo->count++];
  *group = (UndoGroup){.id = ++groupIds, .kind = undo->kind};
  CursorSetCopy(&group->before, &undo->before);
  undo->current = undo->count;
}

static void
Record(void *context, const BufferChange *change)
{
  Undo *undo = context;
  if (undo->replaying) return;
  if (!undo->depth) Fatal("the buffer changed outside an undo group");

  if (!undo->started) StartGroup(undo);
  AddRecord(&undo->groups[undo->count - 1], change);
}

void
UndoInit(Undo *undo, Buffer *buffer)
{
  *undo = (Undo){.buffer = buffer};
  BufferListen(buffer, Record, undo);
}

void
UndoFree(Undo *undo)
{
  BufferUnlisten(undo->buffer, Record, undo);
  DropGroups(undo, 0);
  free(undo->groups);
  CursorSetFree(&undo->before);
  *undo = (Undo){};
}

void
UndoClear(Undo *undo)
{
  DropGroups(undo, 0);
  undo->current = 0;
  undo->savePoint = 0;
  undo->open = false;
}

void
UndoBegin(Undo *undo, enum UndoKind kind, const CursorSet *cursors)
{
  if (undo->depth++) return;

  undo->kind = kind;
  undo->started = false;
  CursorSetCopy(&undo->before, cursors);
}

void
UndoEnd(Undo *undo, const CursorSet *cursors)
{
  if (--undo->depth || !undo->started) return;

  CursorSetCopy(&undo->groups[undo->count - 1].after, cursors);
  undo->open = undo->kind != UndoKindOther;
}

void
UndoBreak(Undo *undo)
{
  undo->open = false;
}

bool
UndoUndo(Undo *undo, CursorSet *cursors)
{
  undo->open = false;
  if (undo->current == 0) return false;

  UndoGroup *group = &undo->groups[--undo->current];
  undo->replaying = true;

  for (u32 index = group->count; index-- > 0;) {
    UndoRecord *record = &group->records[index];
    const char *newText = StrBufText(&record->newText);
    Position end = BufferTextEnd(record->start, newText, record->newText.length);
    Range range = {record->start, end};
    BufferReplace(undo->buffer, range, StrBufText(&record->oldText), record->oldText.length);
  }

  undo->replaying = false;
  CursorSetCopy(cursors, &group->before);
  return true;
}

bool
UndoRedo(Undo *undo, CursorSet *cursors)
{
  undo->open = false;
  if (undo->current == undo->count) return false;

  UndoGroup *group = &undo->groups[undo->current++];
  undo->replaying = true;

  for (u32 index = 0; index < group->count; index++) {
    UndoRecord *record = &group->records[index];
    const char *oldText = StrBufText(&record->oldText);
    Position end = BufferTextEnd(record->start, oldText, record->oldText.length);
    Range range = {record->start, end};
    BufferReplace(undo->buffer, range, StrBufText(&record->newText), record->newText.length);
  }

  undo->replaying = false;
  CursorSetCopy(cursors, &group->after);
  return true;
}

u64
UndoLastId(const Undo *undo)
{
  return undo->current ? undo->groups[undo->current - 1].id : 0;
}

u64
UndoFind(const Undo *undo, u64 id)
{
  u64 index = 0;
  while (index < undo->count && undo->groups[index].id != id) index++;
  return index;
}

void
UndoMarkSaved(Undo *undo)
{
  undo->savePoint = undo->current;
  undo->open = false;
}
