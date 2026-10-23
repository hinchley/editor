#include "jump.h"

#include <string.h>

#include "app.h"
#include "move.h"
#include "settings.h"

typedef struct Place {
  u64 serial;
  Position position;
} Place;

// the places, oldest first, and where in them the cursor is: at count when it is past
// the newest (nothing was gone back over), or at the place it went back or forward to.
// going back from past the newest adds the cursor's place, which may take one more slot.
static Place places[jumpHistorySize + 1];
static u32 count;
static u32 at;

static Place
Here(void)
{
  const WorkspaceEntry *entry = &workspace->entries[workspace->current];
  Document *document = WorkspaceDocument(workspace);
  return (Place){entry->serial, CursorSetPrimary(&document->view->cursors)->head};
}

static bool
SamePlace(Place a, Place b)
{
  return a.serial == b.serial && PositionCompare(a.position, b.position) == 0;
}

// adds a place after the newest, unless it is the newest already, dropping the oldest
// places beyond limit.
static void
Push(Place place, u32 limit)
{
  if (count && SamePlace(places[count - 1], place)) return;

  places[count++] = place;
  if (count <= limit) return;

  u32 dropped = count - limit;
  memmove(places, places + dropped, limit * sizeof(Place));
  count = limit;
}

void
JumpRecord(void)
{
  // the place gone back to is replaced by where the cursor is now, which may have moved.
  if (at < count) count = at;
  Push(Here(), jumpHistorySize);
  at = count;
}

// a position inside the text, on the start of a code point.
static Position
Clamped(const Buffer *buffer, Position position)
{
  position = BufferClamp(buffer, position);
  const StrBuf *line = BufferLine(buffer, position.line);

  while (position.byte > 0 && ((u8)line->data[position.byte] & 0xC0) == 0x80) {
    position.byte--;
  }

  return position;
}

// true when a place can be gone to: its buffer is still loaded and the cursor is not
// there already.
static bool
Usable(Place place, Place here)
{
  u64 index = WorkspaceEntryOf(workspace, place.serial);
  if (index == workspaceNone) return false;

  const Document *document = workspace->entries[index].document;
  if (document) place.position = Clamped(&document->buffer, place.position);
  return !SamePlace(place, here);
}

static bool
Go(Place place)
{
  u64 index = WorkspaceEntryOf(workspace, place.serial);
  if (index != workspace->current && !WorkspaceSwitch(workspace, index, message)) return false;

  Document *document = WorkspaceDocument(workspace);
  Position position = Clamped(&document->buffer, place.position);
  MoveSelect(document->view, position, position);
  return true;
}

bool
JumpBack(void)
{
  Place here = Here();
  u32 target = at;

  do {
    if (target == 0) {
      StrBufAppendString(message, "no earlier place to go back to");
      return false;
    }

    target--;
  } while (!Usable(places[target], here));

  // going back from past the newest place first records where the cursor is, so forward
  // can return to it.
  if (at == count) Push(here, jumpHistorySize + 1);

  at = target;
  return Go(places[target]);
}

bool
JumpForward(void)
{
  Place here = Here();

  for (u32 next = at + 1; next < count; next++) {
    if (!Usable(places[next], here)) continue;

    at = next;
    return Go(places[next]);
  }

  StrBufAppendString(message, "no later place to go forward to");
  return false;
}
