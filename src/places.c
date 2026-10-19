#include "places.h"

#include "jump.h"
#include "memory.h"
#include "move.h"
#include "picker.h"
#include "preview.h"

typedef struct Place {
  u64 serial;
  Range range;
} Place;

static Workspace *workspace;
static StrBuf *message;

static Place *places;
static u64 placeCount;
static u64 placeCapacity;

void
PlacesInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

void
PlacesBegin(void)
{
  placeCount = 0;
  PickerBegin();
}

void
PlacesAdd(u64 serial, Range range, const char *text, u64 length, const char *note,
  const char *mark)
{
  if (placeCount == placeCapacity) {
    placeCapacity = placeCapacity ? placeCapacity * 2 : 64;
    places = MemoryResize(places, placeCapacity, sizeof(Place));
  }

  places[placeCount] = (Place){serial, range};
  PickerAdd(text, length, note, placeCount++);
  if (mark) PickerMark(mark);
}

static void
DrawPlace([[maybe_unused]] void *context, u64 id, i32 column, i32 width, i32 height)
{
  Range range = places[id].range;
  u64 end = range.end.line == range.start.line ? range.end.byte : UINT64_MAX;
  PreviewDraw(places[id].serial, range.start.line, range.start.byte, end, nullptr, column, width,
    height);
}

void
PlacesGo(u64 serial, Position position)
{
  u64 index = WorkspaceEntryOf(workspace, serial);

  if (index == workspaceNone) {
    StrBufAppendString(message, "that buffer has been closed");
    return;
  }

  JumpRecord();
  if (index != workspace->current && !WorkspaceSwitch(workspace, index, message)) return;

  View *view = WorkspaceDocument(workspace)->view;
  Position at = BufferClamp(view->buffer, position);
  MoveSelect(view, at, at);
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (picked) PlacesGo(places[id].serial, places[id].range.start);
}

void
PlacesOpen(const char *title, u64 selected)
{
  PickerOpen(title, "filter: ", Picked, nullptr);
  PickerShowPreview(DrawPlace, nullptr);
  if (selected < placeCount) PickerSelect(selected);
}
