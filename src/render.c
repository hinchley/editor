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

// a hint is only drawn with at least this many columns free after its line.
static constexpr i64 minHintWidth = 10;

// gutter marks for changed lines, and for lines removed above or below.
static constexpr char changedSymbol[] = "\u258E";
static constexpr char removedAboveSymbol[] = "\u2594";
static constexpr char removedBelowSymbol[] = "\u2581";

// the status bar drops fields on the right rather than shorten the name below this.
static constexpr i32 minNameWidth = 8;

// how far past the cluster being drawn matches are looked for at a time; more than a
// window shows of a line.
static constexpr u64 matchReach = 65536;

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

// the bytes of the line being drawn that a diagnostic covers, and the group of the worst
// one covering them.
typedef struct Curl {
  u64 start;
  u64 end;
  enum ThemeGroup group;
} Curl;

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

  // the syntax group of each byte of the current line, or nullptr when not highlighted.
  Highlight *highlight;
  const u8 *groups;

  // the changes since the last commit, or nullptr, and the mark of the current line.
  Changes *changes;
  enum ChangeMark change;

  // the language server's diagnostics, or nullptr; the parts of the current line they
  // cover (and the group of the worst one at its end, or ThemeNone); and the line's worst
  // diagnostic starting on it (ThemeNone without one), which colours its number and gives
  // the hint after it.
  const Diagnostics *diagnostics;
  Curl *curls;
  u32 curlCount;
  u32 curlCapacity;
  enum ThemeGroup curlAtEnd;
  enum ThemeGroup lineGroup;
  u32 lineWorst;
  u32 lineCount;

  // the match on the current line that may still hold the cluster being drawn (when
  // matched is set), where the next one is looked for from, the offset before which no
  // other starts, and the group matches draw with.
  const MatchSet *marks;
  u64 matchLine;
  PatternMatch match;
  bool matched;
  u64 matchFrom;
  u64 matchClear;
  enum ThemeGroup matchGroup;
} Canvas;

// the text area, gutter included, of each pane the last frame drew, by its index among
// the frame's panes; a pane not drawn has no width.
typedef struct PaneArea {
  i32 column;
  i32 width;
  i32 height;
} PaneArea;

static PaneArea drawnAreas[2];

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

// a style with the curly underline of a diagnostic group (or left alone with ThemeNone).
static Style
Curled(Style style, enum ThemeGroup group)
{
  if (group == ThemeNone) return style;

  style.attributes |= StyleAttributeCurly;
  style.underline = ThemeStyle(group).foreground;
  return style;
}

// a style drawn over by a selection or match group (or left alone with ThemeNone).
// the group's background always wins, and its foreground when it has one.
static Style
Overlay(Style style, enum ThemeGroup group)
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
FindMatches(Canvas *canvas, u64 line)
{
  canvas->matchLine = line;
  canvas->matched = false;
  canvas->matchFrom = 0;
  canvas->matchClear = canvas->marks->kind == MatchKindNone ? UINT64_MAX : 0;
}

static void
AddCurl(Canvas *canvas, u64 start, u64 end, enum ThemeGroup group)
{
  if (canvas->curlCount == canvas->curlCapacity) {
    canvas->curlCapacity = canvas->curlCapacity ? canvas->curlCapacity * 2 : 4;
    canvas->curls = MemoryResize(canvas->curls, canvas->curlCapacity, sizeof(Curl));
  }

  canvas->curls[canvas->curlCount++] = (Curl){start, end, group};
}

// finds the parts of a line the diagnostics cover, and its worst diagnostic.
// a diagnostic of no width covers the cluster it is before, or the cell after the end of
// the line.
static void
FindDiagnostics(Canvas *canvas, u64 line, u64 length)
{
  canvas->curlCount = 0;
  canvas->curlAtEnd = ThemeNone;
  canvas->lineGroup = ThemeNone;
  const Diagnostics *diagnostics = canvas->diagnostics;
  if (!diagnostics || !diagnostics->count) return;

  u32 first;
  u32 end;
  DiagnosticsAround(diagnostics, line, &first, &end);

  for (u32 index = first; index < end; index++) {
    const Diagnostic *diagnostic = &diagnostics->items[index];
    Range range = diagnostic->range;
    if (range.end.line < line) continue;

    u64 start = range.start.line == line ? range.start.byte : 0;
    u64 stop = range.end.line == line ? range.end.byte : length;
    enum ThemeGroup group = DiagnosticSeverityGroup(diagnostic->severity);

    // groups are in order of severity, worst first.
    if (stop > start) {
      AddCurl(canvas, start, stop, group);
    } else if (start < length) {
      AddCurl(canvas, start, start + 1, group);
    } else if (canvas->curlAtEnd == ThemeNone || group < canvas->curlAtEnd) {
      canvas->curlAtEnd = group;
    }
  }

  if (DiagnosticsOnLine(diagnostics, line, &canvas->lineWorst, &canvas->lineCount)) {
    const Diagnostic *worst = &diagnostics->items[canvas->lineWorst];
    canvas->lineGroup = DiagnosticSeverityGroup(worst->severity);
  }
}

// the group of the worst diagnostic covering byte, or ThemeNone.
// groups are in order of severity, worst first.
static enum ThemeGroup
CurlOf(const Canvas *canvas, u64 byte)
{
  enum ThemeGroup group = ThemeNone;

  for (u32 index = 0; index < canvas->curlCount; index++) {
    const Curl *curl = &canvas->curls[index];
    bool covers = byte >= curl->start && byte < curl->end;
    if (covers && (group == ThemeNone || curl->group < group)) group = curl->group;
  }

  return group;
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
// matches are looked for from where the last one ended to matchReach past the cluster
// drawn, so a long line is not searched beyond what is drawn of it.
static bool
IsMatched(Canvas *canvas, u64 byte)
{
  PatternMatch *match = &canvas->match;

  for (;;) {
    if (canvas->matched && match->end > byte) return match->start <= byte;

    if (canvas->matched) {
      canvas->matched = false;
      canvas->matchFrom = match->end;
      canvas->matchClear = match->end;
    }

    if (byte < canvas->matchClear) return false;

    u64 limit = byte + matchReach;
    const Buffer *buffer = canvas->view->buffer;
    u64 line = canvas->matchLine;
    canvas->matched =
      MatchInLine(canvas->marks, buffer, line, canvas->matchFrom, limit, match);
    if (!canvas->matched) canvas->matchClear = limit;
  }
}

// the group a byte is drawn over with: a match, the selection, or ThemeNone.
static enum ThemeGroup
MarkOf(Canvas *canvas, u64 byte)
{
  bool selected = canvas->selected.count && IsSelected(canvas, byte);
  bool matched = IsMatched(canvas, byte);
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
  enum ThemeGroup mark,
  enum ThemeGroup curl
)
{
  // blank cells are already empty, so plain blanks only need their symbol.
  bool plain = mark == ThemeNone && curl == ThemeNone;
  if (!plain) FillCells(canvas, row, x, cluster->width, Curled(ThemeStyle(mark), curl));

  // a tab cut by the left edge loses its symbol, and the ruler shows through blanks.
  if (!canvas->view->options->showWhitespace) return;
  if (x < 0 || x >= canvas->textWidth || (x == canvas->rulerX && plain)) return;

  const char *symbol = cluster->kind == LayoutKindSpace ? spaceSymbol : tabSymbol;
  Style style = Overlay(ThemeStyle(ThemeWhitespace), mark);
  if (mark == ThemeSelection) style = ThemeStyle(ThemeSelectionWhitespace);
  PutFaintSymbol(canvas, row, canvas->textColumn + (i32)x, symbol, Curled(style, curl));
}

void
RenderCluster(
  i32 row,
  i32 column,
  const char *text,
  const LayoutCluster *cluster,
  Style style,
  Style control
)
{
  if (cluster->kind == LayoutKindText) {
    u64 size = cluster->end - cluster->start;
    ScreenPutCluster(row, column, text + cluster->start, size, cluster->width, style);
  } else if (cluster->kind == LayoutKindControl) {
    char symbol[2] = {'^', (char)(text[cluster->start] ^ 0x40)};
    ScreenPutText(row, column, symbol, 2, control);
  } else if (cluster->kind == LayoutKindInvalid) {
    PutSymbol(row, column, invalidSymbol, control);
  }
}

// draws one cluster at x columns into the text area.
static void
DrawCluster(Canvas *canvas, i32 row, const LayoutCluster *cluster, i64 x)
{
  enum ThemeGroup mark = MarkOf(canvas, cluster->start);
  enum ThemeGroup curl = canvas->curlCount ? CurlOf(canvas, cluster->start) : ThemeNone;

  if (cluster->kind == LayoutKindSpace || cluster->kind == LayoutKindTab) {
    DrawBlank(canvas, row, cluster, x, mark, curl);
    return;
  }

  // a cluster cut by either edge of the text area shows as blank cells.
  if (x < 0 || x + cluster->width > canvas->textWidth) {
    FillCells(canvas, row, x, cluster->width, Curled(Overlay(styleDefault, mark), curl));
    return;
  }

  i32 column = canvas->textColumn + (i32)x;
  Style syntax = styleDefault;
  if (canvas->groups) syntax = ThemeStyle(HighlightGroup(canvas->groups, cluster->start));

  Style text = Curled(Overlay(syntax, mark), curl);
  Style control = Curled(Overlay(ThemeStyle(ThemeControl), mark), curl);

  RenderCluster(row, column, canvas->text, cluster, text, control);
}

// draws a change mark in the cell before the line number: a bar beside every row of a
// changed line, and a line above or below the first row of one that lines were removed
// beside.
static void
DrawChange(const Canvas *canvas, i32 row, bool first)
{
  const char *symbol = changedSymbol;
  enum ThemeGroup group = ThemeChangeModified;

  switch (canvas->change) {
    case ChangeMarkNone:
      return;
    case ChangeMarkAdded:
      group = ThemeChangeAdded;
      break;
    case ChangeMarkModified:
      break;
    case ChangeMarkRemovedAbove:
    case ChangeMarkRemovedBelow:
      if (!first) return;

      bool above = canvas->change == ChangeMarkRemovedAbove;
      symbol = above ? removedAboveSymbol : removedBelowSymbol;
      group = ThemeChangeRemoved;
      break;
  }

  PutSymbol(row, canvas->view->column, symbol, ThemeStyle(group));
}

static void
DrawGutter(const Canvas *canvas, i32 row, u64 line, bool first)
{
  i32 width = ViewGutterWidth(canvas->view);
  if (width == 0) return;

  DrawChange(canvas, row, first);
  if (!first) return;

  StrBuf number = {};
  StrBufAppendFormat(&number, "%*lu ", width - 1, line + 1);
  // the first cell is the change mark's, already blank or drawn.
  // a line with a diagnostic starting on it is numbered in the worst one's colour.
  bool diagnosed = canvas->lineGroup != ThemeNone;
  Style style = ThemeStyle(diagnosed ? canvas->lineGroup : ThemeGutter);
  ScreenPutText(row, canvas->view->column + 1, number.data + 1, number.length - 1, style);
  StrBufFree(&number);
}

// draws the first line of the line's worst diagnostic, after a symbol for its severity
// and with a count of the others starting on the line, from x columns into the text area
// to its edge, cut with an ellipsis when it does not fit.
static void
DrawHint(const Canvas *canvas, i32 row, i64 x)
{
  i64 width = canvas->textWidth - x;
  if (x < 0 || width < minHintWidth) return;

  const Diagnostics *diagnostics = canvas->diagnostics;
  const Diagnostic *diagnostic = &diagnostics->items[canvas->lineWorst];
  const char *symbol = DiagnosticSeveritySymbol(diagnostic->severity);
  const char *message = DiagnosticsText(diagnostics, diagnostic->message);

  StrBuf hint = {};
  StrBufAppendFormat(&hint, "%s ", symbol);
  StrBufAppend(&hint, message, strcspn(message, "\n"));
  if (canvas->lineCount > 1) StrBufAppendFormat(&hint, " (+%u)", canvas->lineCount - 1);
  StrBuf shown = {};
  LayoutCut(hint.data, hint.length, (i32)width, &shown);

  Style style = ThemeStyle(canvas->lineGroup);
  style.attributes |= StyleAttributeFaint;
  ScreenPutText(row, canvas->textColumn + (i32)x, shown.data, shown.length, style);
  StrBufFree(&shown);
  StrBufFree(&hint);
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
  FindMatches(canvas, line);
  FindDiagnostics(canvas, line, text->length);
  canvas->change = ChangesMarkOf(canvas->changes, line);

  // drawing starts at the first row shown, or (without soft wrap) at the first column
  // shown, so the text before it is passed over without reading its clusters.
  bool wrapped = view->options->softWrap;
  u64 firstIndex = firstRow < rows->count ? firstRow : rows->count - 1;
  LayoutIterator iterator = wrapped
    ? LayoutBeginAt(text->data, text->length, rows->starts[firstIndex])
    : LayoutBeginAtColumn(text->data, text->length, view->leftColumn);
  canvas->groups = HighlightLine(canvas->highlight, line, iterator.at);
  LayoutCluster cluster;
  bool more = LayoutNext(&iterator, &cluster);
  i32 drawn = 0;

  for (u64 index = firstIndex; index < rows->count && row + drawn < bottom; index++) {
    LayoutRow layoutRow = LayoutGetRow(rows, index, text->length);
    i32 screenRow = row + drawn;

    // wrapped rows start at their own first column; unwrapped lines scroll sideways.
    i64 offset = (i64)(more ? cluster.column : iterator.column);
    if (!wrapped) offset = (i64)view->leftColumn;

    DrawGutter(canvas, screenRow, line, index == 0);

    while (more && (layoutRow.last || cluster.start < layoutRow.end)) {
      i64 x = (i64)cluster.column - offset;

      // nothing further along an unwrapped line can be on screen.
      if (x >= canvas->textWidth && layoutRow.last) break;
      DrawCluster(canvas, screenRow, &cluster, x);
      more = LayoutNext(&iterator, &cluster);
    }

    // a selected line break shows as one selected cell after the end of the line, and a
    // diagnostic there as a curled cell.
    i64 end = (i64)iterator.column - offset;
    bool atEnd = layoutRow.last && !more;

    if (atEnd && (canvas->lineBreakSelected || canvas->curlAtEnd != ThemeNone)) {
      enum ThemeGroup group = canvas->lineBreakSelected ? ThemeSelection : ThemeNone;
      FillCells(canvas, screenRow, end, 1, Curled(ThemeStyle(group), canvas->curlAtEnd));
    }

    // the hint goes after the end of the line's last row, past the cursor's cell.
    if (atEnd && canvas->lineGroup != ThemeNone) DrawHint(canvas, screenRow, end + 2);

    drawn++;
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
DrawView(const FramePane *pane)
{
  View *view = pane->view;
  const MatchSet *marks = pane->marks;
  const DisplayOptions *options = view->options;
  Canvas canvas = {
    .view = view,
    .highlight = pane->highlight,
    .changes = pane->changes,
    .diagnostics = pane->diagnostics,
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
  free(canvas.curls);
}

// with several cursors the terminal cursor stays hidden and each one is drawn as a block,
// over whatever colours its cell has.
static void
DrawCursorBlocks(View *view)
{
  CursorSet *set = &view->cursors;
  Style style = ThemeStyle(ThemeCursor);

  for (u32 index = 0; index < set->count; index++) {
    i32 row;
    i32 column;

    if (ViewScreenOf(view, set->cursors[index].head, &row, &column)) {
      ScreenSetStyle(row, column, style);
    }
  }
}

static void
AppendField(StrBuf *status, const char *field)
{
  if (status->length) StrBufAppendString(status, "  ");
  StrBufAppendString(status, field);
}

// how many buffers are open of those loaded, when more than one is loaded.
static void
AppendBuffers(const StatusInfo *info, StrBuf *right)
{
  if (info->loadedCount < 2) return;

  if (right->length) StrBufAppendString(right, "  ");
  StrBufAppendFormat(right, "buf %lu/%lu", info->openCount, info->loadedCount);
}

// the fields of an image's status: its format and size, and the share of its size it is
// shown at, unless it is whole at its own size.
static void
DescribeImage(const StatusInfo *info, u32 scale, bool full, StrBuf *right)
{
  if (full) StrBufAppendString(right, info->image);

  if (scale) {
    if (right->length) StrBufAppendString(right, "  ");
    StrBufAppendFormat(right, "%u%%", scale);
  }

  if (full) AppendBuffers(info, right);
  if (right->length) StrBufAppendByte(right, ' ');
}

static void
DescribePosition(View *view, const StatusInfo *info, bool full, StrBuf *right)
{
  Position head = CursorSetPrimary(&view->cursors)->head;
  const StrBuf *line = BufferLine(view->buffer, head.line);
  u64 column = LayoutColumn(line->data, line->length, head.byte);

  // only what differs from the defaults shows, and line:column comes last, so it stays in
  // place as the others come and go.
  if (full) {
    u32 cursors = view->cursors.count;
    if (cursors > 1) StrBufAppendFormat(right, "%u cursors (esc)", cursors);
    if (info->changes) AppendField(right, info->changes);
    if (info->diagnostics) AppendField(right, info->diagnostics);
    if (view->options->softWrap) AppendField(right, "wrap");
    if (info->fileType) AppendField(right, info->fileType);
    if (info->lineEnding) AppendField(right, info->lineEnding);
    u64 lines = BufferTextLineCount(view->buffer);
    if (right->length) StrBufAppendString(right, "  ");
    StrBufAppendFormat(right, "%lu line%s", lines, StrBufPlural(lines));
    AppendBuffers(info, right);
  }

  if (right->length) StrBufAppendString(right, "  ");
  StrBufAppendFormat(right, "%lu:%lu ", head.line + 1, column + 1);
}

// the unfocused view of a split shows only its name and flags, dimmed.
// scale is the share of an image's size it is shown at, as a percentage to show, or 0.
static void
DrawStatus(View *view, const StatusInfo *info, u32 scale, bool focused, i32 row)
{
  Style style = ThemeStyle(focused ? ThemeStatus : ThemeStatusDim);
  ScreenFill(row, view->column, view->width, style);

  // the flags after the name always show; the name is shortened to fit what is left.
  StrBuf flags = {};
  if (info->modified) StrBufAppendString(&flags, " [+]");
  if (info->readOnly) StrBufAppendString(&flags, " [RO]");

  // a narrow window shows only the cursor position on the right, and then nothing.
  StrBuf right = {};
  i32 rightWidth = 0;
  i32 nameWidth = 0;

  for (i32 attempt = focused ? 0 : 2; attempt < 3; attempt++) {
    StrBufClear(&right);
    if (attempt < 2 && info->image) DescribeImage(info, scale, attempt == 0, &right);
    else if (attempt < 2) DescribePosition(view, info, attempt == 0, &right);
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

// draws a pane's image in the middle of its area, and returns the share of the image's
// size it is shown at, as a percentage to show, or 0.
static u32
DrawImage(const Frame *frame, Image *image, i32 column, i32 width, i32 height)
{
  if (!frame->cellWidth || !frame->cellHeight) {
    static constexpr char text[] = "foot did not report its size in pixels";
    u64 length = sizeof(text) - 1 < (u64)width ? sizeof(text) - 1 : (u64)width;
    ScreenPutText(0, column, text, length, ThemeStyle(ThemeMessage));
    return 0;
  }

  i32 cellWidth = frame->cellWidth;
  i32 cellHeight = frame->cellHeight;
  const ImageRendering *rendering = ImageRender(image, width, height, cellWidth, cellHeight);
  i32 row = (height - rendering->rows) / 2;
  i32 left = column + (width - rendering->columns) / 2;
  const StrBuf *sixel = &rendering->sixel;
  ScreenPutImage(row, left, rendering->columns, rendering->rows, sixel, rendering->key);
  return rendering->scale;
}

static void
DrawPane(const Frame *frame, const FramePane *pane, i32 column, i32 width, i32 height)
{
  bool focused = pane == &frame->panes[frame->focused];
  drawnAreas[pane - frame->panes] = (PaneArea){column, width, height};
  View *view = pane->view;
  ViewSetArea(view, 0, column, width, height);
  u32 scale = 0;

  if (pane->image) {
    scale = DrawImage(frame, pane->image, column, width, height);
  } else {
    ViewScrollToCursor(view);
    DrawView(pane);
  }

  DrawStatus(view, pane->status, scale, focused, height);
}

void
RenderFrame(const Frame *frame)
{
  ScreenBegin();
  drawnAreas[0] = drawnAreas[1] = (PaneArea){};

  if (frame->width < minWindowWidth || frame->height < minWindowHeight) {
    ScreenPutString(0, 0, "terminal too small", styleDefault);
    return;
  }

  // the bottom two rows hold the status bars and the message bar.
  const FramePane *focused = &frame->panes[frame->focused];
  i32 height = frame->height - 2;

  // two sides share the width either side of a one column separator.
  i32 leftWidth = (frame->width - 1) / 2;
  i32 rightColumn = leftWidth + 1;
  i32 rightWidth = frame->width - rightColumn;
  bool wide = frame->width >= minSplitWidth;
  Style separator = ThemeStyle(ThemeGutter);

  if (frame->panel.draw && wide) {
    DrawPane(frame, focused, 0, leftWidth, height);
    frame->panel.draw(frame->panel.context, rightColumn, rightWidth, height + 1);
    for (i32 row = 0; row <= height; row++) PutSymbol(row, leftWidth, rulerSymbol, separator);
  } else if (frame->paneCount == 2 && wide) {
    DrawPane(frame, &frame->panes[0], 0, leftWidth, height);
    DrawPane(frame, &frame->panes[1], rightColumn, rightWidth, height);
    for (i32 row = 0; row <= height; row++) PutSymbol(row, leftWidth, rulerSymbol, separator);
  } else {
    DrawPane(frame, focused, 0, frame->width, height);
  }

  const StrBuf *message = frame->message;
  Style style = ThemeStyle(ThemeMessage);
  ScreenPutText(frame->height - 1, 0, message->data, message->length, style);

  // only the focused view shows its cursors, and an image has none.
  View *view = focused->view;
  if (focused->image) return;

  if (view->cursors.count > 1) {
    DrawCursorBlocks(view);
    return;
  }

  i32 row;
  i32 column;
  if (ViewCursorScreen(view, &row, &column)) ScreenSetCursor(row, column);
}

u32
RenderPaneAt(i32 row, i32 column)
{
  for (u32 index = 0; index < 2; index++) {
    const PaneArea *area = &drawnAreas[index];
    if (row < 0 || row >= area->height) continue;
    if (column >= area->column && column < area->column + area->width) return index;
  }

  return renderNoPane;
}

u32
RenderStatusAt(i32 row, i32 column)
{
  // the status bar is the row below the text.
  for (u32 index = 0; index < 2; index++) {
    const PaneArea *area = &drawnAreas[index];
    if (row != area->height) continue;
    if (column >= area->column && column < area->column + area->width) return index;
  }

  return renderNoPane;
}
