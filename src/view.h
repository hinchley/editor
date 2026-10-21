#pragma once

// a window onto a buffer: its screen area, its cursors and how far it is scrolled.
// the buffer is seen as a sequence of visual rows (one per line, or several when soft
// wrap is on), and scrolling and vertical movement are written once against those rows.
#include "buffer.h"
#include "cursor.h"
#include "layout.h"
#include "types.h"

// display settings shared by every view.
typedef struct DisplayOptions {
  bool softWrap;
  bool lineNumbers;
  bool showWhitespace;
  bool ruler;
  i32 rulerColumn;
} DisplayOptions;

// one visual row: a line and the index of a row within it.
typedef struct ViewRow {
  u64 line;
  u64 row;
} ViewRow;

// long lines whose rows a view keeps.
static constexpr u32 viewCacheSize = 4;

// the rows of one line laid out at width.
typedef struct ViewRowCache {
  bool valid;
  u64 line;
  i32 width;
  LayoutRows rows;
} ViewRowCache;

typedef struct View {
  Buffer *buffer;
  const DisplayOptions *options;
  CursorSet cursors;

  // the first visual row shown, and (without soft wrap) the first display column.
  ViewRow top;
  u64 leftColumn;

  // the screen area, gutter included.
  i32 row;
  i32 column;
  i32 width;
  i32 height;

  // the rows of the most recently laid out short line.
  LayoutRows rows;

  // the rows of recently laid out long lines, which are kept until the buffer changes.
  ViewRowCache cache[viewCacheSize];
  u32 cacheNext;
} View;

// sets up a view with one cursor at the start of buffer.
// the view follows changes to the buffer, keeping its cursors on the same text, until
// ViewFree.
void
ViewInit(View *view, Buffer *buffer, const DisplayOptions *options);

void
ViewFree(View *view);

// keeps the cursors and scroll position inside the buffer after its text was replaced
// without telling listeners (a reload).
void
ViewClamp(View *view);

// gives target the cursors and scroll position of source, a view of the same buffer.
void
ViewCopy(View *target, const View *source);

void
ViewSetArea(View *view, i32 row, i32 column, i32 width, i32 height);

// width of the line number gutter, including its padding, or 0 when it is hidden.
i32
ViewGutterWidth(const View *view);

// width of the text area right of the gutter.
i32
ViewTextWidth(const View *view);

// lays out a line at the view's width.
// the result lives in the view and is replaced by the next call.
const LayoutRows *
ViewLayoutLine(View *view, u64 line);

// the visual row holding a position.
ViewRow
ViewRowOf(View *view, Position position);

// the visual row count rows after (or before, when negative) from, stopping at the first
// or last row of the buffer.
// moved receives how many rows were actually stepped, always zero or positive.
ViewRow
ViewStep(View *view, ViewRow from, i64 count, u64 *moved);

// the column of a position within its visual row.
u64
ViewRowColumn(View *view, Position position);

// the position at a column within a visual row.
Position
ViewPositionAt(View *view, ViewRow row, u64 column);

// scrolls so the cursors are shown with the scroll margin around them, and keeps the view
// from showing empty rows past the end of the buffer.
// when the cursors do not all fit (or there is one), the primary cursor is kept in sight.
// when they fit but not with their margins, they are kept in sight without margins.
void
ViewScrollToCursor(View *view);

// true when the view could scroll by count rows without passing either end of the buffer,
// wherever its cursors are.
bool
ViewCanScroll(View *view, i64 count);

// scrolls by count rows without moving the cursor.
// returns false, without scrolling, when a cursor kept in sight would end up inside the
// scroll margin or out of view, or the view would pass either end of the buffer.
bool
ViewScroll(View *view, i64 count);

// where a position is on screen.
// returns false when it is outside the view.
bool
ViewScreenOf(View *view, Position position, i32 *row, i32 *column);

// the visual row shown at a screen cell, and in column the display column within that row:
// the first one for a cell in the gutter, and the row that far beyond the edge for a cell
// above or below the view (stopping at the first or last row of the buffer).
ViewRow
ViewRowAtScreen(View *view, i32 row, i32 screenColumn, u64 *column);

// the position shown at a screen cell, or the nearest one to it.
// a cell in the gutter gives the start of its row, one past the end of a row gives the
// end, and one below the last line gives the end of the buffer.
// a cell above or below the view gives the row that far beyond its edge, so a drag past
// an edge reaches text not shown yet.
Position
ViewPositionAtScreen(View *view, i32 row, i32 column);

// where the primary cursor is on screen.
// returns false when it is outside the view.
bool
ViewCursorScreen(View *view, i32 *row, i32 *column);
