#include "render.h"

#include <string.h>

#include "layout.h"
#include "screen.h"
#include "settings.h"
#include "theme.h"

static constexpr char spaceSymbol[] = "\u00B7";
static constexpr char tabSymbol[] = "\u2192";
static constexpr char rulerSymbol[] = "\u2502";
static constexpr char invalidSymbol[] = "\uFFFD";

// the status bar drops fields on the right rather than shorten the name below this.
static constexpr i32 minNameWidth = 8;

// what every row of one view draw shares.
typedef struct Canvas {
  View *view;
  const char *text;
  i32 textColumn;
  i32 textWidth;

  // the screen column of the ruler within the text area, or -1 when it is not shown.
  i64 rulerX;
} Canvas;

static void
PutSymbol(i32 row, i32 column, const char *symbol, enum ThemeGroup group)
{
  ScreenPutCluster(row, column, symbol, strlen(symbol), 1, ThemeStyle(group));
}

static void
DrawBlank(const Canvas *canvas, i32 row, const LayoutCluster *cluster, i64 x)
{
  // blank cells are already empty, so only a symbol in the first cell needs drawing.
  // a tab cut by the left edge loses its symbol, and the ruler shows through blanks.
  if (!canvas->view->options->showWhitespace) return;
  if (x < 0 || x >= canvas->textWidth || x == canvas->rulerX) return;

  const char *symbol = cluster->kind == LayoutKindSpace ? spaceSymbol : tabSymbol;
  PutSymbol(row, canvas->textColumn + (i32)x, symbol, ThemeWhitespace);
}

static void
DrawControl(const Canvas *canvas, i32 row, const LayoutCluster *cluster, i32 column)
{
  char symbol[2] = {'^', (char)(canvas->text[cluster->start] ^ 0x40)};
  ScreenPutText(row, column, symbol, 2, ThemeStyle(ThemeControl));
}

// draws one cluster at x columns into the text area.
static void
DrawCluster(const Canvas *canvas, i32 row, const LayoutCluster *cluster, i64 x)
{
  if (cluster->kind == LayoutKindSpace || cluster->kind == LayoutKindTab) {
    DrawBlank(canvas, row, cluster, x);
    return;
  }

  // a cluster cut by either edge of the text area shows as blank cells.
  if (x < 0 || x + cluster->width > canvas->textWidth) {
    for (i64 cell = x; cell < x + cluster->width && cell < canvas->textWidth; cell++) {
      if (cell >= 0) ScreenFill(row, canvas->textColumn + (i32)cell, 1, styleDefault);
    }

    return;
  }

  i32 column = canvas->textColumn + (i32)x;
  const char *bytes = canvas->text + cluster->start;
  u64 size = cluster->end - cluster->start;

  switch (cluster->kind) {
  case LayoutKindText:
    ScreenPutCluster(row, column, bytes, size, cluster->width, styleDefault);
    break;
  case LayoutKindControl:
    DrawControl(canvas, row, cluster, column);
    break;
  case LayoutKindInvalid:
    PutSymbol(row, column, invalidSymbol, ThemeControl);
    break;
  case LayoutKindSpace:
  case LayoutKindTab:
    break;
  }
}

static void
DrawGutter(const Canvas *canvas, i32 row, u64 line, bool first)
{
  i32 width = ViewGutterWidth(canvas->view);
  if (width == 0 || !first) return;

  StrBuf number = {};
  StrBufAppendFormat(&number, "%*lu ", width - 1, line + 1);
  Style style = ThemeStyle(ThemeGutter);
  ScreenPutText(row, canvas->view->column, number.data, number.length, style);
  StrBufFree(&number);
}

// draws the rows of a line from firstRow, starting at screen row, and returns the number
// of rows drawn.
static i32
DrawLine(Canvas *canvas, u64 line, u64 firstRow, i32 row)
{
  View *view = canvas->view;
  const StrBuf *text = BufferLine(view->buffer, line);
  const LayoutRows *rows = ViewLayoutLine(view, line);
  i32 bottom = view->row + view->height;
  canvas->text = text->data;

  LayoutIterator iterator = LayoutBegin(text->data, text->length);
  LayoutCluster cluster;
  bool more = LayoutNext(&iterator, &cluster);
  i32 drawn = 0;

  for (u64 index = 0; index < rows->count && row + drawn < bottom; index++) {
    LayoutRow layoutRow = LayoutGetRow(rows, index, text->length);
    bool visible = index >= firstRow;
    i32 screenRow = row + drawn;

    // wrapped rows start at their own first column; unwrapped lines scroll sideways.
    i64 offset = more ? (i64)cluster.column : 0;
    if (!view->options->softWrap) offset = (i64)view->leftColumn;

    if (visible) DrawGutter(canvas, screenRow, line, index == 0);

    while (more && (layoutRow.last || cluster.start < layoutRow.end)) {
      i64 x = (i64)cluster.column - offset;

      // nothing further along an unwrapped line can be on screen.
      if (x >= canvas->textWidth && layoutRow.last) break;
      if (visible) DrawCluster(canvas, screenRow, &cluster, x);
      more = LayoutNext(&iterator, &cluster);
    }

    if (visible) drawn++;
  }

  return drawn;
}

static void
DrawRuler(const Canvas *canvas)
{
  if (canvas->rulerX < 0) return;

  const View *view = canvas->view;
  i32 column = canvas->textColumn + (i32)canvas->rulerX;

  for (i32 row = view->row; row < view->row + view->height; row++) {
    PutSymbol(row, column, rulerSymbol, ThemeRuler);
  }
}

static void
DrawView(View *view)
{
  const DisplayOptions *options = view->options;
  Canvas canvas = {
    .view = view,
    .textColumn = view->column + ViewGutterWidth(view),
    .textWidth = ViewTextWidth(view),
    .rulerX = -1
  };

  // the ruler sits in the cell just after rulerColumn, drawn first so text covers it.
  if (options->ruler) {
    i64 rulerX = (i64)options->rulerColumn - (i64)view->leftColumn;
    if (rulerX >= 0 && rulerX < canvas.textWidth) canvas.rulerX = rulerX;
  }

  DrawRuler(&canvas);

  i32 row = view->row;
  u64 firstRow = view->top.row;
  u64 lineCount = BufferLineCount(view->buffer);

  for (u64 line = view->top.line; line < lineCount && row < view->row + view->height; line++) {
    row += DrawLine(&canvas, line, firstRow, row);
    firstRow = 0;
  }
}

static void
AppendField(StrBuf *status, const char *field)
{
  if (status->length) StrBufAppendString(status, "  ");
  StrBufAppendString(status, field);
}

static void
DescribePosition(View *view, const StatusInfo *info, bool full, StrBuf *right)
{
  Position head = CursorSetPrimary(&view->cursors)->head;
  const StrBuf *line = BufferLine(view->buffer, head.line);
  u64 column = LayoutColumn(line->data, line->length, head.byte);

  if (full) {
    AppendField(right, view->options->softWrap ? "wrap" : "nowrap");
    if (info->fileType) AppendField(right, info->fileType);
    AppendField(right, info->lineEnding);
  }

  if (right->length) StrBufAppendString(right, "  ");
  StrBufAppendFormat(right, "%lu:%lu", head.line + 1, column + 1);
  if (full) StrBufAppendFormat(right, "  %lu lines", BufferLineCount(view->buffer));
  if (full) StrBufAppendFormat(right, "  %lu:%lu", info->openCount, info->loadedCount);
  StrBufAppendByte(right, ' ');
}

static void
DrawStatus(View *view, const StatusInfo *info, i32 row)
{
  Style style = ThemeStyle(ThemeStatus);
  ScreenFill(row, view->column, view->width, style);

  // the flags after the name always show; the name is shortened to fit what is left.
  StrBuf flags = {};
  if (info->modified) StrBufAppendString(&flags, " [+]");
  if (info->readOnly) StrBufAppendString(&flags, " [RO]");

  // a narrow window shows only the cursor position on the right, and then nothing.
  StrBuf right = {};
  i32 rightWidth = 0;
  i32 nameWidth = 0;

  for (i32 attempt = 0; attempt < 3; attempt++) {
    StrBufClear(&right);
    if (attempt < 2) DescribePosition(view, info, attempt == 0, &right);
    rightWidth = (i32)right.length;
    nameWidth = view->width - rightWidth - (i32)flags.length - 2;
    if (nameWidth >= minNameWidth) break;
  }

  StrBuf left = {};
  StrBufAppendByte(&left, ' ');
  const StrBuf *name = info->name;
  if (name->length) LayoutShorten(name->data, name->length, nameWidth, &left);
  else StrBufAppendString(&left, "[unnamed]");
  StrBufAppend(&left, flags.data, flags.length);

  ScreenPutText(row, view->column, left.data, left.length, style);
  i32 rightColumn = view->column + view->width - rightWidth;
  if (rightWidth) ScreenPutText(row, rightColumn, right.data, right.length, style);

  StrBufFree(&left);
  StrBufFree(&flags);
  StrBufFree(&right);
}

void
RenderFrame(const Frame *frame)
{
  ScreenBegin();

  if (frame->width < minWindowWidth || frame->height < minWindowHeight) {
    ScreenPutString(0, 0, "terminal too small", styleDefault);
    return;
  }

  // the bottom two rows hold the status bar and the message bar.
  View *view = frame->view;
  ViewSetArea(view, 0, 0, frame->width, frame->height - 2);
  ViewScrollToCursor(view);
  DrawView(view);
  DrawStatus(view, frame->status, frame->height - 2);

  const StrBuf *message = frame->message;
  Style style = ThemeStyle(ThemeMessage);
  ScreenPutText(frame->height - 1, 0, message->data, message->length, style);

  i32 row;
  i32 column;
  if (ViewCursorScreen(view, &row, &column)) ScreenSetCursor(row, column);
}
