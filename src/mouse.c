#include "mouse.h"

#include <time.h>

#include "move.h"
#include "multicursor.h"
#include "settings.h"

// the drag going on: what its press does, how many presses made it (1 to 3), and the text
// its press chose (the press position, or the word or line there), which the drag keeps
// selected; or for a column drag, the row and display column of its press.
// none of them follow edits, so anything but the mouse ends the drag (MouseInterrupt).
static bool dragging;
static enum MouseMode mode;
static u32 clicks;
static Range origin;
static ViewRow columnRow;
static u64 columnStart;

// the last press, when it put a cursor down: when it was, its cell and its position.
// a repeat on the same cell uses the same position, though putting the cursor down may have
// scrolled other text under the cell.
typedef struct Press {
  bool placed;
  i64 time;
  i32 row;
  i32 column;
  Position position;
} Press;

static Press lastPress;

static i64
NowMilliseconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1'000'000;
}

// the text a press chooses at position: the position itself after one press, the word
// there (or else the position) after two, and the line with its line break after three.
static Range
UnitAt(View *view, Position position, u32 presses)
{
  Range unit = {position, position};
  const Buffer *buffer = view->buffer;
  if (presses == 1) return unit;

  if (presses == 3) {
    bool lastLine = position.line + 1 == BufferLineCount(buffer);
    Position end = lastLine ? BufferEnd(buffer) : (Position){position.line + 1, 0};
    return (Range){{position.line, 0}, end};
  }

  u64 start;
  u64 end;
  const StrBuf *text = BufferLine(buffer, position.line);
  if (!MoveWordAround(text, position.byte, &start, &end)) return unit;
  return (Range){{position.line, start}, {position.line, end}};
}

// gives the primary cursor a selection, merging it with any it runs into.
static void
SelectPrimary(View *view, Position anchor, Position head)
{
  Cursor *cursor = CursorSetPrimary(&view->cursors);
  cursor->anchor = anchor;
  cursor->head = head;
  cursor->desiredColumn = ViewRowColumn(view, head);
  CursorSetMerge(&view->cursors);
  ViewScrollToCursor(view);
}

// a column press puts a single cursor down, and starts no double click.
static void
PressColumn(View *view, i32 row, i32 column)
{
  columnRow = ViewRowAtScreen(view, row, column, &columnStart);
  Position position = ViewPositionAt(view, columnRow, columnStart);
  MoveSelect(view, position, position);
  dragging = true;
  lastPress = (Press){};
}

void
MousePress(View *view, i32 row, i32 column, enum MouseMode newMode)
{
  mode = newMode;

  if (mode == MouseModeColumn) {
    PressColumn(view, row, column);
    return;
  }

  i64 now = NowMilliseconds();
  bool again = lastPress.placed && now - lastPress.time <= multiClickInterval &&
    row == lastPress.row && column == lastPress.column;

  bool gutter = column < view->column + ViewGutterWidth(view);
  clicks = gutter ? 3 : again ? clicks % 3 + 1 : 1;
  Position position = again ? lastPress.position : ViewPositionAtScreen(view, row, column);
  origin = UnitAt(view, position, clicks);

  // a repeat selects with the cursor the first press put down, the primary one; removing a
  // cursor leaves nothing to drag or repeat.
  if (again) {
    SelectPrimary(view, origin.start, origin.end);
    dragging = true;
  } else if (mode == MouseModeAdd) {
    dragging = MultiCursorToggleAt(view, origin.start);
    if (dragging && gutter) SelectPrimary(view, origin.start, origin.end);
  } else {
    MoveSelect(view, origin.start, origin.end);
    dragging = true;
  }

  lastPress = (Press){dragging, now, row, column, position};
}

// the cursor a column drag gives a visual row, selecting from the press's display column to
// endColumn.
static Cursor
ColumnCursor(View *view, ViewRow row, u64 endColumn)
{
  Position anchor = ViewPositionAt(view, row, columnStart);
  return (Cursor){anchor, ViewPositionAt(view, row, endColumn), endColumn};
}

// gives every visual row from the column press to the one at a screen cell a cursor, each
// selecting from the press's display column to the cell's.
// the cursor on the cell's row comes last, so it is the primary one, which the view follows.
static void
DragColumns(View *view, i32 row, i32 column)
{
  u64 endColumn;
  ViewRow pointer = ViewRowAtScreen(view, row, column, &endColumn);
  bool up = pointer.line < columnRow.line ||
    (pointer.line == columnRow.line && pointer.row < columnRow.row);

  CursorSet *set = &view->cursors;
  CursorSetCollapse(set);
  ViewRow at = columnRow;
  set->cursors[0] = ColumnCursor(view, at, endColumn);

  while (at.line != pointer.line || at.row != pointer.row) {
    u64 moved;
    at = ViewStep(view, at, up ? -1 : 1, &moved);
    if (!moved) break;

    CursorSetAdd(set, ColumnCursor(view, at, endColumn));
  }

  CursorSetMerge(set);
  ViewScrollToCursor(view);
}

void
MouseDrag(View *view, i32 row, i32 column)
{
  if (!dragging) return;

  if (mode == MouseModeColumn) {
    DragColumns(view, row, column);
    return;
  }

  // the selection holds the press's text and grows from its far side, a unit at a time.
  Range unit = UnitAt(view, ViewPositionAtScreen(view, row, column), clicks);

  if (PositionCompare(unit.start, origin.start) < 0) {
    SelectPrimary(view, origin.end, unit.start);
    return;
  }

  bool beyond = PositionCompare(unit.end, origin.end) > 0;
  SelectPrimary(view, origin.start, beyond ? unit.end : origin.end);
}

void
MouseRelease(void)
{
  dragging = false;
}

void
MouseInterrupt(void)
{
  dragging = false;
  lastPress.placed = false;
}
