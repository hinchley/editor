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

  // the rows of the most recently laid out line.
  LayoutRows rows;
} View;

void
ViewInit(View *view, Buffer *buffer, const DisplayOptions *options);

void
ViewFree(View *view);

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

// scrolls so the primary cursor is shown with the scroll margin around it, and keeps the
// view from showing empty rows past the end of the buffer.
void
ViewScrollToCursor(View *view);

// scrolls by count rows without moving the cursor.
// returns false, without scrolling, when the cursor would end up inside the scroll margin
// or the view would pass either end of the buffer.
bool
ViewScroll(View *view, i64 count);

// where the primary cursor is on screen.
// returns false when it is outside the view.
bool
ViewCursorScreen(View *view, i32 *row, i32 *column);
