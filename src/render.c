#include "render.h"

#include <stdlib.h>
#include <string.h>

#include "layout.h"
#include "memory.h"
#include "screen.h"
#include "settings.h"
#include "theme.h"

static constexpr char spaceSymbol[] = "\u00B7";
static constexpr char tabSymbol[] = "\u2192";
static constexpr char rulerSymbol[] = "\u2502";
static constexpr char invalidSymbol[] = "\uFFFD";

// the status bar drops fields on the right rather than shorten the name below this.
static constexpr i32 minNameWidth = 8;

// bytes of the line being drawn.
typedef struct Span {
  u64 start;
  u64 end;
} Span;

typedef struct SpanList {
  Span *spans;
  u32 count;
  u32 capacity;
} SpanList;

// what every row of one view draw shares.
typedef struct Canvas {
  View *view;
  const char *text;
  i32 textColumn;
  i32 textWidth;

  // the screen column of the ruler within the text area, or -1 when it is not shown.
  i64 rulerX;

  // the screen cell of the terminal cursor, or -1 when it is outside the view.
  i32 cursorRow;
  i32 cursorColumn;

  // the selected parts of the current line, and whether its line break is selected.
  SpanList selected;
  bool lineBreakSelected;

  // the matches on the current line, in order, the first one that may still hold the
  // cluster being drawn, and the group they draw with.
  const MatchSet *marks;
  SpanList matched;
  u32 nextMatch;
  enum ThemeGroup matchGroup;
} Canvas;

static void
PutSymbol(i32 row, i32 column, const char *symbol, Style style)
{
  ScreenPutCluster(row, column, symbol, strlen(symbol), 1, style);
}

// draws a faint symbol (whitespace or the ruler).
// terminals draw the cursor in the colour of the cell under it, so the symbol under the
// cursor uses the default colour, or the cursor would be as faint as the symbol.
static void
PutFaintSymbol(const Canvas *canvas, i32 row, i32 column, const char *symbol, Style style)
{
  bool underCursor = row == canvas->cursorRow && column == canvas->cursorColumn;
  if (underCursor) style.foreground = colorDefault;
  PutSymbol(row, column, symbol, style);
}

// a style drawn over by a selection or match group (or left alone with ThemeNone).
// the group's background always wins, and its foreground when it has one.
static Style
Highlight(Style style, enum ThemeGroup group)
{
  if (group == ThemeNone) return style;

  Style mark = ThemeStyle(group);
  style.background = mark.background;
  if (mark.foreground != colorDefault) style.foreground = mark.foreground;
  return style;
}

static void
AddSpan(SpanList *list, u64 start, u64 end)
{
  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 4;
    list->spans = MemoryResize(list->spans, list->capacity, sizeof(Span));
  }

  list->spans[list->count++] = (Span){start, end};
}

static void
FindSelection(Canvas *canvas, u64 line, u64 length)
{
  CursorSet *set = &canvas->view->cursors;
  canvas->selected.count = 0;
  canvas->lineBreakSelected = false;

  for (u32 index = 0; index < set->count; index++) {
    Range range = CursorRange(&set->cursors[index]);
    if (range.start.line > line || range.end.line < line) continue;

    u64 start = range.start.line == line ? range.start.byte : 0;
    u64 end = range.end.line == line ? range.end.byte : length;
    if (range.end.line > line) canvas->lineBreakSelected = true;
    if (end > start) AddSpan(&canvas->selected, start, end);
  }
}

static void
FindMatches(Canvas *canvas, const StrBuf *text)
{
  canvas->matched.count = 0;
  canvas->nextMatch = 0;
  if (canvas->marks->kind == MatchKindNone) return;

  PatternMatch match = {};

  while (MatchInLine(canvas->marks, text->data, text->length, match.end, &match)) {
    AddSpan(&canvas->matched, match.start, match.end);
  }
}

static bool
IsSelected(const Canvas *canvas, u64 byte)
{
  const SpanList *list = &canvas->selected;

  for (u32 index = 0; index < list->count; index++) {
    if (byte >= list->spans[index].start && byte < list->spans[index].end) return true;
  }

  return false;
}

// clusters are drawn in order, so the matches before byte can be passed over for good.
static bool
IsMatched(Canvas *canvas, u64 byte)
{
  const SpanList *list = &canvas->matched;
  while (canvas->nextMatch < list->count && list->spans[canvas->nextMatch].end <= byte) {
    canvas->nextMatch++;
  }

  return canvas->nextMatch < list->count && list->spans[canvas->nextMatch].start <= byte;
}

// the group a byte is drawn over with: a match, the selection, or ThemeNone.
static enum ThemeGroup
MarkOf(Canvas *canvas, u64 byte)
{
  bool selected = canvas->selected.count && IsSelected(canvas, byte);
  bool matched = canvas->matched.count && IsMatched(canvas, byte);
  if (matched) return selected ? ThemeMatchSelected : canvas->matchGroup;
  return selected ? ThemeSelection : ThemeNone;
}

// fills the visible cells from x to x + width.
static void
FillCells(const Canvas *canvas, i32 row, i64 x, i64 width, Style style)
{
  for (i64 cell = x; cell < x + width && cell < canvas->textWidth; cell++) {
    if (cell >= 0) ScreenFill(row, canvas->textColumn + (i32)cell, 1, style);
  }
}

static void
DrawBlank(
  const Canvas *canvas,
  i32 row,
  const LayoutCluster *cluster,
  i64 x,
  enum ThemeGroup mark
) {
  // blank cells are already empty, so unmarked blanks only need their symbol.
  if (mark != ThemeNone) FillCells(canvas, row, x, cluster->width, ThemeStyle(mark));

  // a tab cut by the left edge loses its symbol, and the ruler shows through blanks.
  if (!canvas->view->options->showWhitespace) return;
  if (x < 0 || x >= canvas->textWidth || (x == canvas->rulerX && mark == ThemeNone)) return;

  const char *symbol = cluster->kind == LayoutKindSpace ? spaceSymbol : tabSymbol;
  Style style = Highlight(ThemeStyle(ThemeWhitespace), mark);
  if (mark == ThemeSelection) style = ThemeStyle(ThemeSelectionWhitespace);
  PutFaintSymbol(canvas, row, canvas->textColumn + (i32)x, symbol, style);
}

static void
DrawControl(const Canvas *canvas, i32 row, i32 column, u64 byte, Style style)
{
  char symbol[2] = {'^', (char)(canvas->text[byte] ^ 0x40)};
  ScreenPutText(row, column, symbol, 2, style);
}

// draws one cluster at x columns into the text area.
static void
DrawCluster(Canvas *canvas, i32 row, const LayoutCluster *cluster, i64 x)
{
  enum ThemeGroup mark = MarkOf(canvas, cluster->start);

  if (cluster->kind == LayoutKindSpace || cluster->kind == LayoutKindTab) {
    DrawBlank(canvas, row, cluster, x, mark);
    return;
  }

  // a cluster cut by either edge of the text area shows as blank cells.
  if (x < 0 || x + cluster->width > canvas->textWidth) {
    FillCells(canvas, row, x, cluster->width, Highlight(styleDefault, mark));
    return;
  }

  i32 column = canvas->textColumn + (i32)x;
  const char *bytes = canvas->text + cluster->start;
  u64 size = cluster->end - cluster->start;
  Style text = Highlight(styleDefault, mark);
  Style control = Highlight(ThemeStyle(ThemeControl), mark);

  switch (cluster->kind) {
  case LayoutKindText:
    ScreenPutCluster(row, column, bytes, size, cluster->width, text);
    break;
  case LayoutKindControl:
    DrawControl(canvas, row, column, cluster->start, control);
    break;
  case LayoutKindInvalid:
    PutSymbol(row, column, invalidSymbol, control);
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
  FindSelection(canvas, line, text->length);
  FindMatches(canvas, text);

  LayoutIterator iterator = LayoutBegin(text->data, text->length);
  LayoutCluster cluster;
  bool more = LayoutNext(&iterator, &cluster);
  i32 drawn = 0;

  for (u64 index = 0; index < rows->count && row + drawn < bottom; index++) {
    LayoutRow layoutRow = LayoutGetRow(rows, index, text->length);
    bool visible = index >= firstRow;
    i32 screenRow = row + drawn;

    // wrapped rows start at their own first column; unwrapped lines scroll sideways.
    i64 offset = (i64)(more ? cluster.column : iterator.column);
    if (!view->options->softWrap) offset = (i64)view->leftColumn;

    if (visible) DrawGutter(canvas, screenRow, line, index == 0);

    while (more && (layoutRow.last || cluster.start < layoutRow.end)) {
      i64 x = (i64)cluster.column - offset;

      // nothing further along an unwrapped line can be on screen.
      if (x >= canvas->textWidth && layoutRow.last) break;
      if (visible) DrawCluster(canvas, screenRow, &cluster, x);
      more = LayoutNext(&iterator, &cluster);
    }

    // a selected line break shows as one selected cell after the end of the line.
    if (visible && layoutRow.last && !more && canvas->lineBreakSelected) {
      i64 x = (i64)iterator.column - offset;
      FillCells(canvas, screenRow, x, 1, ThemeStyle(ThemeSelection));
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
    PutFaintSymbol(canvas, row, column, rulerSymbol, ThemeStyle(ThemeRuler));
  }
}

static void
DrawView(View *view, const MatchSet *marks)
{
  const DisplayOptions *options = view->options;
  Canvas canvas = {
    .view = view,
    .marks = marks,
    .matchGroup = marks->kind == MatchKindSpell ? ThemeMisspelt : ThemeMatch,
    .textColumn = view->column + ViewGutterWidth(view),
    .textWidth = ViewTextWidth(view),
    .rulerX = -1,
    .cursorRow = -1,
    .cursorColumn = -1
  };

  ViewCursorScreen(view, &canvas.cursorRow, &canvas.cursorColumn);

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

  free(canvas.selected.spans);
  free(canvas.matched.spans);
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
  LayoutShorten(name->data, name->length, nameWidth, &left);
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
  DrawView(view, frame->marks);
  DrawStatus(view, frame->status, frame->height - 2);

  const StrBuf *message = frame->message;
  Style style = ThemeStyle(ThemeMessage);
  ScreenPutText(frame->height - 1, 0, message->data, message->length, style);

  i32 row;
  i32 column;
  if (ViewCursorScreen(view, &row, &column)) ScreenSetCursor(row, column);
}
