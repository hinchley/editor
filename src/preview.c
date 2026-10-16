#include "preview.h"

#include <stdlib.h>
#include <string.h>

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

static Workspace *workspace;
static Lines lines = {.entry = noEntry};

void
PreviewInit(Workspace *newWorkspace)
{
  workspace = newWorkspace;
}

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

// where a preview line is drawn: its text area, the first column shown, the bytes to pick
// out (or an empty range) and the pattern whose matches show.
typedef struct PreviewRow {
  i32 row;
  i32 column;
  i32 width;
  u64 left;
  PatternMatch hit;
  const Pattern *pattern;
} PreviewRow;

static void
DrawLine(const PreviewRow *place, const char *text, u64 length)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  PatternMatch match = {};
  bool more = place->pattern != nullptr;

  while (LayoutNext(&iterator, &cluster)) {
    if (cluster.column < place->left) continue;

    i64 x = (i64)(cluster.column - place->left);
    if (x + cluster.width > place->width) break;

    if (more && cluster.start >= match.end) {
      more = PatternFind(place->pattern, text, length, cluster.start, &match);
    }

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
      ScreenPutCluster(place->row, column, bytes, size, cluster.width, style);
      break;
    case LayoutKindSpace:
    case LayoutKindTab:
      ScreenFill(place->row, column, (i32)cluster.width, style);
      break;
    case LayoutKindControl:
      ScreenPutText(place->row, column, symbol, 2, control);
      break;
    case LayoutKindInvalid:
      ScreenPutCluster(place->row, column, invalidSymbol, strlen(invalidSymbol), 1, control);
      break;
    }
  }
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
) {
  // the header takes the first row, and the lines the rows below it.
  DrawHeader(serial, line, column, width);
  height--;
  if (height < 1) return;

  u64 first = line > (u64)(height - 1) / 2 ? line - (u64)(height - 1) / 2 : 0;
  const char *text;
  u64 length;

  if (!LineOf(serial, line, &text, &length)) {
    ScreenPutString(1, column + 1, "the line is no longer there", ThemeStyle(ThemeGutter));
    return;
  }

  i32 digits = 1;
  for (u64 last = first + (u64)height; last >= 10; last /= 10) digits++;

  PreviewRow place = {
    .column = column + digits + 2,
    .width = width - digits - 2,
    .pattern = pattern
  };
  if (place.width < 1) return;

  // the bytes picked out are kept in view, a quarter of the way in when the line has to
  // scroll.
  u64 hitStart = start <= length ? start : length;
  u64 hitEnd = end <= length ? end : length;
  u64 startColumn = LayoutColumn(text, length, hitStart);
  u64 endColumn = LayoutColumn(text, length, hitEnd);
  u64 quarter = (u64)place.width / 4;
  bool scrolled = endColumn > (u64)place.width;
  if (scrolled) place.left = startColumn > quarter ? startColumn - quarter : 0;

  for (i32 row = 0; row < height; row++) {
    u64 shown = first + (u64)row;
    if (!LineOf(serial, shown, &text, &length)) break;

    StrBuf number = {};
    StrBufAppendFormat(&number, "%*lu", digits, shown + 1);
    Style gutter = shown == line ? styleDefault : ThemeStyle(ThemeGutter);
    ScreenPutText(row + 1, column + 1, number.data, number.length, gutter);
    StrBufFree(&number);

    place.row = row + 1;
    place.hit = shown == line ? (PatternMatch){hitStart, hitEnd} : (PatternMatch){};
    DrawLine(&place, text, length);
  }
}
