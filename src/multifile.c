#include "multifile.h"

#include <stdlib.h>

#include "app.h"
#include "edit.h"
#include "memory.h"
#include "picker.h"
#include "places.h"

// a file's part of the change: its entry, and the undo group that made it.
typedef struct Part {
  u64 serial;
  u64 group;
} Part;

// where a part's undo group is in its buffer's history.
enum PartState {
  // the buffer was closed, or reloaded, which forgot its history.
  PartStateGone,

  // done, and the last step, so undo takes it back.
  PartStateLast,

  // done, with other steps after it.
  PartStateDone,

  // undone, and the next step, so redo makes it again.
  PartStateNext,

  // undone, with other undone steps before it.
  PartStateUndone
};

// the last change: what it was, and its parts.
static StrBuf what;
static Part *parts;
static u64 partCount;
static u64 partCapacity;

// the change being made: what it is, whether it has changed a file yet, and the part
// being made, its entry and the last group of its history before it.
static StrBuf making;
static bool started;
static u64 editingSerial;
static u64 editingBefore;

// the changes that lead from a part's group to its buffer's text, which place the group's
// records in it.
static BufferChange *changes;
static u64 changeCapacity;

void
MultifileBegin(const char *newWhat)
{
  StrBufClear(&making);
  StrBufAppendString(&making, newWhat);
  started = false;
}

Document *
MultifileEditBegin(u64 index)
{
  StrBuf ignored = {};
  bool opened = index != workspaceNone && WorkspaceOpen(workspace, index, &ignored);
  StrBufFree(&ignored);
  if (!opened) return nullptr;

  Document *document = workspace->entries[index].document;
  editingSerial = workspace->entries[index].serial;
  editingBefore = UndoLastId(&document->undo);
  UndoBreak(&document->undo);
  UndoBegin(&document->undo, UndoKindOther, &document->view->cursors);
  return document;
}

void
MultifileEditEnd(Document *document)
{
  UndoEnd(&document->undo, &document->view->cursors);

  // a step that changed nothing made no group.
  u64 group = UndoLastId(&document->undo);
  if (group == editingBefore) return;

  if (!started) {
    started = true;
    partCount = 0;
    StrBufClear(&what);
    StrBufAppend(&what, making.data, making.length);
  }

  if (partCount == partCapacity) {
    partCapacity = partCapacity ? partCapacity * 2 : 16;
    parts = MemoryResize(parts, partCapacity, sizeof(Part));
  }

  parts[partCount++] = (Part){editingSerial, group};
}

// where part's group is in its history, and its document when it still has the group.
static enum PartState
StateOf(const Part *part, Document **document, u64 *group)
{
  u64 index = WorkspaceEntryOf(workspace, part->serial);
  *document = index == workspaceNone ? nullptr : workspace->entries[index].document;
  if (!*document) return PartStateGone;

  const Undo *undo = &(*document)->undo;
  *group = UndoFind(undo, part->group);
  if (*group == undo->count) return PartStateGone;
  if (*group + 1 == undo->current) return PartStateLast;
  if (*group < undo->current) return PartStateDone;
  return *group == undo->current ? PartStateNext : PartStateUndone;
}

// appends a record's change, or (back) the change that undoes it.
static void
AddChange(u64 *count, const UndoRecord *record, bool back)
{
  if (*count == changeCapacity) {
    changeCapacity = changeCapacity ? changeCapacity * 2 : 64;
    changes = MemoryResize(changes, changeCapacity, sizeof(BufferChange));
  }

  Position start = record->start;
  Position oldEnd = BufferTextEnd(start, StrBufText(&record->oldText), record->oldText.length);
  Position newEnd = BufferTextEnd(start, StrBufText(&record->newText), record->newText.length);
  changes[(*count)++] = (BufferChange){
    .start = start,
    .oldEnd = back ? newEnd : oldEnd,
    .newEnd = back ? oldEnd : newEnd
  };
}

// the changes from the group at index to the text as it is now: the group's records and
// the steps done after it, or, for an undone group, the records undone from the last and
// the steps undone before it, from the last.
static u64
CollectChanges(const Undo *undo, u64 index)
{
  u64 count = 0;
  bool done = index < undo->current;

  if (done) {
    for (u64 group = index; group < undo->current; group++) {
      const UndoGroup *step = &undo->groups[group];
      for (u32 record = 0; record < step->count; record++) {
        AddChange(&count, &step->records[record], false);
      }
    }
  } else {
    for (u64 group = index + 1; group-- > undo->current;) {
      const UndoGroup *step = &undo->groups[group];
      for (u32 record = step->count; record-- > 0;) {
        AddChange(&count, &step->records[record], true);
      }
    }
  }

  return count;
}

// a place in the list: its entry and its range.
typedef struct Place {
  u64 serial;
  Range range;
} Place;

static int
ComparePlaces(const void *first, const void *second)
{
  const Place *a = first;
  const Place *b = second;
  return PositionCompare(a->range.start, b->range.start);
}

// appends the places of a part's group, as its buffer has them now, in order.
static void
AddPlaces(
  const Part *part,
  const Document *document,
  u64 index,
  Place **places,
  u64 *count,
  u64 *capacity
)
{
  const Undo *undo = &document->undo;
  u64 records = undo->groups[index].count;
  u64 total = CollectChanges(undo, index);
  bool done = index < undo->current;
  u64 first = *count;

  for (u64 record = 0; record < records; record++) {
    // the record's own change, which the changes after it move.
    u64 at = done ? record : records - 1 - record;
    Range range = {changes[at].start, changes[at].newEnd};

    for (u64 later = at + 1; later < total; later++) {
      range.start = BufferAdjust(range.start, &changes[later]);
      range.end = BufferAdjust(range.end, &changes[later]);
    }

    if (*count == *capacity) {
      *capacity = *capacity ? *capacity * 2 : 64;
      *places = MemoryResize(*places, *capacity, sizeof(Place));
    }

    (*places)[(*count)++] = (Place){part->serial, range};
  }

  qsort(*places + first, *count - first, sizeof(Place), ComparePlaces);
}

// undoes the parts whose step is the last, or (redo) redoes those whose step is the
// next, and says how many it changed and which it left as they are.
static void
Step(bool redo)
{
  u64 stepped = 0;
  u64 left = 0;
  u64 leftIndex = workspaceNone;

  for (u64 at = 0; at < partCount; at++) {
    Document *document;
    u64 group;
    enum PartState state = StateOf(&parts[at], &document, &group);

    if (state == (redo ? PartStateNext : PartStateLast)) {
      EditContext edit = {document->view, &document->undo, false};
      if (redo) EditRedo(&edit);
      else EditUndo(&edit);
      stepped++;
      continue;
    }

    // a part already as asked is neither changed nor left.
    bool done = state == PartStateLast || state == PartStateDone;
    bool undone = state == PartStateNext || state == PartStateUndone;
    if (redo ? done : undone) continue;

    if (!left++) leftIndex = WorkspaceEntryOf(workspace, parts[at].serial);
  }

  if (stepped) {
    StrBufAppendFormat(
      message,
      "%s the %s in %lu file%s (alt+shift+z lists it)",
      redo ? "redid" : "undid",
      what.data,
      stepped,
      StrBufPlural(stepped)
    );
  } else if (!left) {
    StrBufAppendFormat(message, "the %s is already %s", what.data, redo ? "done" : "undone");
  }

  if (!left) return;

  if (stepped) StrBufAppendString(message, "; ");
  StrBufAppendFormat(
    message,
    "%lu file%s changed or closed since, left as %s: ",
    left,
    left == 1 ? " was" : "s were",
    left == 1 ? "it is" : "they are"
  );

  if (leftIndex == workspaceNone) StrBufAppendString(message, "closed");
  else WorkspaceName(workspace, leftIndex, message);
  if (left > 1) StrBufAppendString(message, ", ...");
}

void
MultifileNoteStep(u64 serial, u64 group, bool redo)
{
  bool ours = false;
  u64 others = 0;

  for (u64 at = 0; at < partCount; at++) {
    if (parts[at].serial == serial) {
      ours = parts[at].group == group;
      continue;
    }

    Document *document;
    u64 index;
    enum PartState state = StateOf(&parts[at], &document, &index);
    bool done = state == PartStateLast || state == PartStateDone;
    bool undone = state == PartStateNext || state == PartStateUndone;
    if (redo ? undone : done) others++;
  }

  if (!ours || !others) return;

  StrBufAppendFormat(
    message,
    "%s this file's part of the %s, not the other %lu file%s (alt+shift+z, then %s, %s it in "
    "every file)",
    redo ? "redid" : "undid",
    what.data,
    others,
    StrBufPlural(others),
    redo ? "ctrl+y" : "ctrl+z",
    redo ? "redoes" : "undoes"
  );
}

static bool
ListKey([[maybe_unused]] void *context, Key key)
{
  bool undo = key.code == 'z';
  if (key.modifiers != KeyModifierControl || (!undo && key.code != 'y')) return false;

  PickerClose();
  Step(!undo);
  return true;
}

// appends a place's line to note, as a list shows it.
static void
AppendLine(StrBuf *note, const Buffer *buffer, u64 line)
{
  const StrBuf *text = BufferLine(buffer, line);
  PickerAppendLine(note, text->data ? text->data : "", text->length);
}

void
MultifileReview(void)
{
  if (!partCount) {
    StrBufAppendString(message, "no change has been made across files yet");
    return;
  }

  Place *places = nullptr;
  u64 count = 0;
  u64 capacity = 0;
  u64 files = 0;
  u64 undone = 0;
  bool canUndo = false;
  bool canRedo = false;

  for (u64 at = 0; at < partCount; at++) {
    Document *document;
    u64 group;
    enum PartState state = StateOf(&parts[at], &document, &group);
    if (state == PartStateGone) continue;

    files++;
    if (state == PartStateNext || state == PartStateUndone) undone++;
    canUndo = canUndo || state == PartStateLast;
    canRedo = canRedo || state == PartStateNext;
    AddPlaces(&parts[at], document, group, &places, &count, &capacity);
  }

  if (!files) {
    StrBufAppendFormat(
      message,
      "the files of the last %s have been closed or reloaded since",
      what.data
    );
    return;
  }

  StrBuf item = {};
  StrBuf note = {};
  PlacesBegin();

  for (u64 at = 0; at < count; at++) {
    u64 index = WorkspaceEntryOf(workspace, places[at].serial);
    const Buffer *buffer = &workspace->entries[index].document->buffer;
    Position start = BufferClamp(buffer, places[at].range.start);

    StrBufClear(&item);
    StrBufClear(&note);
    WorkspaceName(workspace, index, &item);
    StrBufAppendFormat(&item, ":%lu", start.line + 1);
    AppendLine(&note, buffer, start.line);
    PlacesAdd(places[at].serial, places[at].range, item.data, item.length, note.data, nullptr);
  }

  StrBuf title = {};
  StrBufAppendFormat(&title, "the %s", what.data);
  if (undone == files) StrBufAppendString(&title, ", undone");
  else if (undone) StrBufAppendFormat(&title, ", undone in %lu of %lu files", undone, files);
  StrBufAppendFormat(
    &title,
    ": %lu place%s in %lu file%s",
    count,
    StrBufPlural(count),
    files,
    StrBufPlural(files)
  );
  if (canUndo) StrBufAppendString(&title, " (ctrl+z undoes it in every file)");
  else if (canRedo) StrBufAppendString(&title, " (ctrl+y redoes it in every file)");

  PlacesOpen(title.data, 0);
  PickerHandleKeys(ListKey, nullptr);

  StrBufFree(&title);
  StrBufFree(&item);
  StrBufFree(&note);
  free(places);
}
