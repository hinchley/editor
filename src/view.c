#include "view.h"

#include "settings.h"

// the line number gutter is never narrower than this many digits.
static constexpr i32 minGutterDigits = 3;

// lines at least this long keep their rows in the view's cache.
// shorter lines are laid out again whenever they are needed, which costs less than
// looking them up.
static constexpr u64 cacheMinLength = 4096;

static void
ForgetRows(View *view)
{
  for (u32 index = 0; index < viewCacheSize; index++) view->cache[index].valid = false;
}

// keeps the cursors and the first visible line on the same text when the buffer changes.
static void
FollowChange(void *context, const BufferChange *change)
{
  View *view = context;
  CursorSetAdjust(&view->cursors, change);
  ForgetRows(view);

  // a change wholly above the view shifts it; one reaching into it starts it at the change.
  ViewRow *top = &view->top;

  if (change->oldEnd.line < top->line) {
    top->line = top->line - change->oldEnd.line + change->newEnd.line;
  } else if (change->start.line < top->line) {
    *top = (ViewRow){change->start.line, 0};
  }
}

void
ViewInit(View *view, Buffer *buffer, const DisplayOptions *options)
{
  *view = (View){.buffer = buffer, .options = options};
  CursorSetInit(&view->cursors);
  BufferListen(buffer, FollowChange, view);
}

void
ViewFree(View *view)
{
  BufferUnlisten(view->buffer, FollowChange, view);
  CursorSetFree(&view->cursors);
  LayoutRowsFree(&view->rows);
  for (u32 index = 0; index < viewCacheSize; index++) LayoutRowsFree(&view->cache[index].rows);
}

void
ViewClamp(View *view)
{
  CursorSetClamp(&view->cursors, view->buffer);
  ForgetRows(view);
  u64 lastLine = BufferLineCount(view->buffer) - 1;
  if (view->top.line > lastLine) view->top = (ViewRow){lastLine, 0};
}

void
ViewCopy(View *target, const View *source)
{
  CursorSetCopy(&target->cursors, &source->cursors);
  target->top = source->top;
  target->leftColumn = source->leftColumn;
}

void
ViewSetArea(View *view, i32 row, i32 column, i32 width, i32 height)
{
  view->row = row;
  view->column = column;
  view->width = width;
  view->height = height;
}

i32
ViewGutterWidth(const View *view)
{
  if (!view->options->lineNumbers) return 0;

  i32 digits = 1;
  for (u64 count = BufferLineCount(view->buffer); count >= 10; count /= 10) digits++;
  if (digits < minGutterDigits) digits = minGutterDigits;

  // a space either side of the number.
  return digits + 2;
}

i32
ViewTextWidth(const View *view)
{
  i32 width = view->width - ViewGutterWidth(view);
  return width > 1 ? width : 1;
}

const LayoutRows *
ViewLayoutLine(View *view, u64 line)
{
  const StrBuf *text = BufferLine(view->buffer, line);
  i32 width = view->options->softWrap ? ViewTextWidth(view) : 0;

  if (width == 0 || text->length < cacheMinLength) {
    LayoutWrap(text->data, text->length, width, &view->rows);
    return &view->rows;
  }

  for (u32 index = 0; index < viewCacheSize; index++) {
    ViewRowCache *entry = &view->cache[index];
    if (entry->valid && entry->line == line && entry->width == width) return &entry->rows;
  }

  ViewRowCache *entry = &view->cache[view->cacheNext];
  view->cacheNext = (view->cacheNext + 1) % viewCacheSize;
  LayoutWrap(text->data, text->length, width, &entry->rows);
  entry->valid = true;
  entry->line = line;
  entry->width = width;
  return &entry->rows;
}

static u64
RowCount(View *view, u64 line)
{
  if (!view->options->softWrap) return 1;
  return ViewLayoutLine(view, line)->count;
}

ViewRow
ViewRowOf(View *view, Position position)
{
  if (!view->options->softWrap) return (ViewRow){position.line, 0};

  const LayoutRows *rows = ViewLayoutLine(view, position.line);
  return (ViewRow){position.line, LayoutRowOf(rows, position.byte)};
}

static i32
RowCompare(ViewRow a, ViewRow b)
{
  if (a.line != b.line) return a.line < b.line ? -1 : 1;
  if (a.row != b.row) return a.row < b.row ? -1 : 1;
  return 0;
}

static ViewRow
StepForward(View *view, ViewRow at, u64 count, u64 *moved)
{
  u64 lastLine = BufferLineCount(view->buffer) - 1;
  *moved = 0;

  while (count > 0) {
    u64 rowCount = RowCount(view, at.line);
    u64 available = rowCount - 1 - at.row;

    if (count <= available) {
      at.row += count;
      *moved += count;
      break;
    }

    if (at.line == lastLine) {
      at.row = rowCount - 1;
      *moved += available;
      break;
    }

    *moved += available + 1;
    count -= available + 1;
    at = (ViewRow){at.line + 1, 0};
  }

  return at;
}

static ViewRow
StepBackward(View *view, ViewRow at, u64 count, u64 *moved)
{
  *moved = 0;

  while (count > 0) {
    if (count <= at.row) {
      at.row -= count;
      *moved += count;
      break;
    }

    if (at.line == 0) {
      *moved += at.row;
      at.row = 0;
      break;
    }

    *moved += at.row + 1;
    count -= at.row + 1;
    at.line--;
    at.row = RowCount(view, at.line) - 1;
  }

  return at;
}

ViewRow
ViewStep(View *view, ViewRow from, i64 count, u64 *moved)
{
  u64 rowCount = RowCount(view, from.line);
  if (from.row >= rowCount) from.row = rowCount - 1;

  // without wrapping every line is one row, so long jumps need no per line work.
  if (!view->options->softWrap) {
    u64 lastLine = BufferLineCount(view->buffer) - 1;
    u64 line = from.line;

    if (count >= 0) line = (u64)count > lastLine - line ? lastLine : line + (u64)count;
    else line = (u64)-count > line ? 0 : line - (u64)-count;

    *moved = line > from.line ? line - from.line : from.line - line;
    return (ViewRow){line, 0};
  }

  if (count >= 0) return StepForward(view, from, (u64)count, moved);
  return StepBackward(view, from, (u64)-count, moved);
}

// rows from a to b (b may be before a), with the magnitude capped just past limit.
static i64
Distance(View *view, ViewRow a, ViewRow b, u64 limit)
{
  if (RowCompare(a, b) > 0) return -Distance(view, b, a, limit);
  if (!view->options->softWrap) return (i64)(b.line - a.line);

  u64 distance = 0;

  while (a.line < b.line) {
    distance += RowCount(view, a.line) - a.row;
    if (distance > limit) return (i64)limit + 1;
    a = (ViewRow){a.line + 1, 0};
  }

  return (i64)(distance + b.row - a.row);
}

u64
ViewRowColumn(View *view, Position position)
{
  const StrBuf *text = BufferLine(view->buffer, position.line);
  u64 column = LayoutColumn(text->data, text->length, position.byte);
  if (!view->options->softWrap) return column;

  const LayoutRows *rows = ViewLayoutLine(view, position.line);
  u64 start = rows->starts[LayoutRowOf(rows, position.byte)];
  return column - LayoutColumn(text->data, text->length, start);
}

Position
ViewPositionAt(View *view, ViewRow row, u64 column)
{
  const StrBuf *text = BufferLine(view->buffer, row.line);
  const LayoutRows *rows = ViewLayoutLine(view, row.line);
  u64 index = row.row < rows->count ? row.row : rows->count - 1;
  LayoutRow layoutRow = LayoutGetRow(rows, index, text->length);
  return (Position){row.line, LayoutByteAt(text->data, text->length, layoutRow, column)};
}

static ViewRow
LastRow(View *view)
{
  u64 line = BufferLineCount(view->buffer) - 1;
  return (ViewRow){line, RowCount(view, line) - 1};
}

// the margins around the cursor row, which shrink near the ends of the buffer.
typedef struct Margins {
  u64 above;
  u64 below;
} Margins;

static Margins
MarginsAround(View *view, ViewRow cursor)
{
  i64 margin = scrollOffset < (view->height - 1) / 2 ? scrollOffset : (view->height - 1) / 2;
  if (margin < 0) margin = 0;

  Margins margins;
  ViewStep(view, cursor, -margin, &margins.above);
  ViewStep(view, cursor, margin, &margins.below);
  return margins;
}

static void
ScrollColumns(View *view)
{
  if (view->options->softWrap) {
    view->leftColumn = 0;
    return;
  }

  Cursor *cursor = CursorSetPrimary(&view->cursors);
  const StrBuf *text = BufferLine(view->buffer, cursor->head.line);
  u64 column = LayoutColumn(text->data, text->length, cursor->head.byte);
  i32 width = ViewTextWidth(view);
  u64 margin = (u64)(scrollOffset < (width - 1) / 2 ? scrollOffset : (width - 1) / 2);

  if (column < view->leftColumn + margin) {
    view->leftColumn = column > margin ? column - margin : 0;
  } else if (column + margin >= view->leftColumn + (u64)width) {
    view->leftColumn = column + margin + 1 - (u64)width;
  }
}

// the rows the view keeps in sight, upper to lower, with the margins wanted around them.
typedef struct Sight {
  ViewRow upper;
  ViewRow lower;
  u64 above;
  u64 below;
} Sight;

// every cursor when they all fit in the view (with the margins when there is room for
// them), or else just the primary one.
static Sight
SightOf(View *view)
{
  CursorSet *set = &view->cursors;
  ViewRow primary = ViewRowOf(view, CursorSetPrimary(set)->head);
  Margins margins = MarginsAround(view, primary);
  Sight sight = {primary, primary, margins.above, margins.below};
  if (set->count < 2) return sight;

  Position first = set->cursors[0].head;
  Position last = first;

  for (u32 index = 1; index < set->count; index++) {
    Position head = set->cursors[index].head;
    if (PositionCompare(head, first) < 0) first = head;
    if (PositionCompare(head, last) > 0) last = head;
  }

  u64 height = (u64)view->height;
  if (last.line - first.line >= height) return sight;

  ViewRow upper = ViewRowOf(view, first);
  ViewRow lower = ViewRowOf(view, last);
  i64 span = Distance(view, upper, lower, height);
  if (span >= (i64)height) return sight;

  u64 above = MarginsAround(view, upper).above;
  u64 below = MarginsAround(view, lower).below;
  if ((u64)span + above + below >= height) above = below = 0;
  return (Sight){upper, lower, above, below};
}

// true when the view starting at top shows the sight with its margins.
static bool
InSight(View *view, ViewRow top, const Sight *sight)
{
  u64 height = (u64)view->height;
  i64 upper = Distance(view, top, sight->upper, height);
  i64 lower = Distance(view, top, sight->lower, height);
  return upper >= (i64)sight->above && lower <= (i64)(height - 1 - sight->below);
}

void
ViewScrollToCursor(View *view)
{
  if (view->height <= 0) return;

  u64 moved;
  u64 height = (u64)view->height;
  Sight sight = SightOf(view);
  view->top = ViewStep(view, view->top, 0, &moved);

  i64 upper = Distance(view, view->top, sight.upper, height);
  i64 lower = Distance(view, view->top, sight.lower, height);
  i64 lowest = (i64)(height - 1 - sight.below);

  if (upper < (i64)sight.above) {
    view->top = ViewStep(view, sight.upper, -(i64)sight.above, &moved);
  } else if (lower > lowest) {
    view->top = ViewStep(view, sight.lower, -lowest, &moved);
  }

  // never leave empty rows below the end of the buffer while there is text above.
  ViewRow highest = ViewStep(view, LastRow(view), -(i64)(height - 1), &moved);
  if (RowCompare(view->top, highest) > 0) view->top = highest;

  ScrollColumns(view);
}

bool
ViewScroll(View *view, i64 count)
{
  if (view->height <= 0) return false;

  u64 moved;
  u64 height = (u64)view->height;
  ViewRow top = ViewStep(view, view->top, count, &moved);
  if (moved == 0) return false;

  ViewRow highest = ViewStep(view, LastRow(view), -(i64)(height - 1), &moved);
  if (count > 0 && RowCompare(top, highest) > 0) return false;

  Sight sight = SightOf(view);
  if (!InSight(view, top, &sight)) return false;

  view->top = top;
  return true;
}

bool
ViewScreenOf(View *view, Position position, i32 *row, i32 *column)
{
  // a line before the first shown, or further below it than the view is high, is off
  // screen without measuring how far.
  u64 top = view->top.line;
  if (position.line < top || position.line - top >= (u64)view->height) return false;

  ViewRow cursor = ViewRowOf(view, position);
  i64 distance = Distance(view, view->top, cursor, (u64)view->height);
  if (distance < 0 || distance >= view->height) return false;

  u64 rowColumn = ViewRowColumn(view, position);
  if (rowColumn < view->leftColumn) return false;

  u64 x = rowColumn - view->leftColumn;
  if (x >= (u64)ViewTextWidth(view)) return false;

  *row = view->row + (i32)distance;
  *column = view->column + ViewGutterWidth(view) + (i32)x;
  return true;
}

bool
ViewCursorScreen(View *view, i32 *row, i32 *column)
{
  return ViewScreenOf(view, CursorSetPrimary(&view->cursors)->head, row, column);
}
