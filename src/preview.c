#include "preview.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "file.h"
#include "layout.h"
#include "memory.h"
#include "screen.h"
#include "theme.h"

static constexpr u64 noEntry = UINT64_MAX;

static constexpr char invalidSymbol[] = "�";

// a file read from disk (of the entry with the serial entry), with where each line starts.
typedef struct Lines {
  u64 entry;
  bool readable;
  StrBuf text;
  u64 *starts;
  u64 count;
  u64 capacity;
} Lines;

static Lines lines = {.entry = noEntry};

static const char *
TextOf(const StrBuf *text)
{
  return text->data ? text->data : "";
}

void
PreviewForget(void)
{
  lines.entry = noEntry;
  StrBufFree(&lines.text);
}

// reads the entry's file, unless it is the one already read.
static void
ReadLines(u64 entry)
{
  if (lines.entry == entry) return;

  lines.entry = entry;
  lines.count = 0;
  u64 index = WorkspaceEntryOf(workspace, entry);
  const char *path = index == workspaceNone ? "" : TextOf(&workspace->entries[index].path);
  FileStamp stamp;
  lines.readable = index != workspaceNone &&
    FileRead(path, &lines.text, &stamp, nullptr) == FileStatusOk;
  if (!lines.readable) return;

  const char *text = TextOf(&lines.text);
  u64 length = lines.text.length;

  for (u64 at = 0;;) {
    if (lines.count == lines.capacity) {
      lines.capacity = lines.capacity ? lines.capacity * 2 : 1024;
      lines.starts = MemoryResize(lines.starts, lines.capacity, sizeof(u64));
    }

    lines.starts[lines.count++] = at;
    const char *newline = memchr(text + at, '\n', length - at);
    if (!newline) break;

    at = (u64)(newline - text) + 1;
  }
}

// the text of a line of the entry's file, from its buffer when open, or false when there
// is no such line.
static bool
LineOf(u64 entry, u64 line, const char **text, u64 *length)
{
  u64 index = WorkspaceEntryOf(workspace, entry);
  bool gone = index == workspaceNone;
  const Document *document = gone ? nullptr : workspace->entries[index].document;

  if (document) {
    if (line >= BufferLineCount(&document->buffer)) return false;

    const StrBuf *found = BufferLine(&document->buffer, line);
    *text = TextOf(found);
    *length = found->length;
    return true;
  }

  ReadLines(entry);
  if (!lines.readable || line >= lines.count) return false;

  const char *all = TextOf(&lines.text);
  u64 start = lines.starts[line];
  u64 end = line + 1 < lines.count ? lines.starts[line + 1] - 1 : lines.text.length;
  if (end > start && all[end - 1] == '\r' && end < lines.text.length) end--;
  *text = all + start;
  *length = end - start;
  return true;
}

// where a preview line is drawn: the screen row it starts on, the row below the last it
// may take, its text area, its first visual row shown, the first column shown (when it is
// not wrapped), the bytes to pick out (or an empty range) and the pattern whose matches
// show.
typedef struct PreviewRow {
  i32 row;
  i32 bottom;
  i32 column;
  i32 width;
  u64 firstRow;
  u64 left;
  PatternMatch hit;
  const Pattern *pattern;
} PreviewRow;

// lays out a line in rows width columns wide: wrapped when soft wrap is on, using the
// whole width since no cursor needs the last column, or else one row.
static void
WrapLine(const char *text, u64 length, i32 width, LayoutRows *rows)
{
  LayoutWrap(text, length, workspace->options->softWrap ? width + 1 : 0, rows);
}

// draws the rows of a line, laid out in rows, from its first row shown, and returns the
// screen rows it took.
static i32
DrawLine(const PreviewRow *place, const char *text, u64 length, const LayoutRows *rows)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  PatternMatch match = {};
  bool more = place->pattern != nullptr;
  u64 index = 0;
  LayoutRow layoutRow = LayoutGetRow(rows, 0, length);
  u64 left = place->left;
  i32 row = place->row - (i32)place->firstRow;

  while (LayoutNext(&iterator, &cluster)) {
    // each wrapped row starts at its own first column.
    while (!layoutRow.last && cluster.start >= layoutRow.end) {
      layoutRow = LayoutGetRow(rows, ++index, length);
      left = cluster.column;
      row++;
    }

    if (row >= place->bottom) break;

    // matches are found from the start of the line, so one that starts out of sight still
    // shows.
    if (more && cluster.start >= match.end) {
      more = PatternFind(place->pattern, text, length, cluster.start, UINT64_MAX, &match);
    }

    if (row < place->row || cluster.column < left) continue;

    i64 x = (i64)(cluster.column - left);
    if (x + cluster.width > place->width && layoutRow.last) break;
    if (x + cluster.width > place->width) continue;

    bool isHit = cluster.start >= place->hit.start && cluster.start < place->hit.end;
    bool matched = more && cluster.start >= match.start && cluster.start < match.end;
    if (!place->pattern) matched = isHit;

    enum ThemeGroup group = isHit ? ThemeMatchSelected : ThemeMatch;
    Style style = matched ? ThemeStyle(group) : styleDefault;
    Style control = ThemeStyle(ThemeControl);
    if (matched) control.background = style.background;

    i32 column = place->column + (i32)x;
    const char *bytes = text + cluster.start;
    u64 size = cluster.end - cluster.start;
    char symbol[2] = {'^', (char)(text[cluster.start] ^ 0x40)};

    switch (cluster.kind) {
      case LayoutKindText:
        ScreenPutCluster(row, column, bytes, size, cluster.width, style);
        break;
      case LayoutKindSpace:
      case LayoutKindTab:
        ScreenFill(row, column, (i32)cluster.width, style);
        break;
      case LayoutKindControl:
        ScreenPutText(row, column, symbol, 2, control);
        break;
      case LayoutKindInvalid:
        ScreenPutCluster(row, column, invalidSymbol, strlen(invalidSymbol), 1, control);
        break;
    }
  }

  i32 last = place->row - (i32)place->firstRow + (i32)rows->count;
  return (last < place->bottom ? last : place->bottom) - place->row;
}

// names the entry and the line on the row above the lines.
static void
DrawHeader(u64 serial, u64 line, i32 column, i32 width)
{
  u64 index = WorkspaceEntryOf(workspace, serial);
  if (index == workspaceNone) return;

  StrBuf place = {};
  WorkspaceName(workspace, index, &place);
  StrBufAppendFormat(&place, ":%lu", line + 1);
  StrBuf shown = {};
  LayoutShorten(place.data, place.length, width - 2, &shown);
  ScreenPutText(0, column + 1, shown.data, shown.length, ThemeStyle(ThemeGutter));
  StrBufFree(&shown);
  StrBufFree(&place);
}

void
PreviewDraw(
  u64 serial,
  u64 line,
  u64 start,
  u64 end,
  const Pattern *pattern,
  i32 column,
  i32 width,
  i32 height
)
{
  // the header takes the first row, and the lines the rows below it.
  DrawHeader(serial, line, column, width);
  if (height < 2) return;

  const char *text;
  u64 length;

  if (!LineOf(serial, line, &text, &length)) {
    ScreenPutString(1, column + 1, "the line is no longer there", ThemeStyle(ThemeGutter));
    return;
  }

  // the numbers are as wide as the last line that could show needs.
  i32 digits = 1;
  for (u64 last = line + (u64)height; last >= 10; last /= 10) digits++;

  PreviewRow place = {
    .bottom = height,
    .column = column + digits + 2,
    .width = width - digits - 2,
    .pattern = pattern
  };
  if (place.width < 1) return;

  // the bytes picked out are kept in view: wrapped, by the row that holds them, and
  // otherwise a quarter of the way in when the line has to scroll.
  u64 hitStart = start <= length ? start : length;
  u64 hitEnd = end <= length ? end : length;
  u64 startColumn = LayoutColumn(text, length, hitStart);
  u64 endColumn = LayoutColumn(text, length, hitEnd);
  u64 quarter = (u64)place.width / 4;
  bool scrolled = !workspace->options->softWrap && endColumn > (u64)place.width;
  if (scrolled) place.left = startColumn > quarter ? startColumn - quarter : 0;

  LayoutRows rows = {};
  WrapLine(text, length, place.width, &rows);

  // that row is put in the middle, going back over the rows of the lines above it.
  u64 first = line;
  u64 firstRow = LayoutRowOf(&rows, hitStart);
  u64 above = (u64)(height - 2) / 2;

  while (above > 0) {
    if (firstRow >= above) {
      firstRow -= above;
      break;
    }

    above -= firstRow;
    firstRow = 0;
    if (!first || !LineOf(serial, first - 1, &text, &length)) break;

    first--;
    above--;
    WrapLine(text, length, place.width, &rows);
    firstRow = rows.count - 1;
  }

  place.row = 1;

  for (u64 shown = first; place.row < height; shown++) {
    if (!LineOf(serial, shown, &text, &length)) break;

    // only a line's first row is numbered.
    if (!firstRow) {
      StrBuf number = {};
      StrBufAppendFormat(&number, "%*lu", digits, shown + 1);
      Style gutter = shown == line ? styleDefault : ThemeStyle(ThemeGutter);
      ScreenPutText(place.row, column + 1, number.data, number.length, gutter);
      StrBufFree(&number);
    }

    WrapLine(text, length, place.width, &rows);
    place.hit = shown == line ? (PatternMatch){hitStart, hitEnd} : (PatternMatch){};
    place.firstRow = firstRow;
    place.row += DrawLine(&place, text, length, &rows);
    firstRow = 0;
  }

  LayoutRowsFree(&rows);
}
