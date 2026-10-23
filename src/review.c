#include "review.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "changes.h"
#include "file.h"
#include "jump.h"
#include "layout.h"
#include "memory.h"
#include "move.h"
#include "picker.h"
#include "render.h"
#include "scan.h"
#include "screen.h"
#include "settings.h"
#include "theme.h"

// unchanged lines the panel shows before and after a change.
static constexpr u64 contextLines = 3;

// marks a changed row, as the gutter does.
static constexpr char markSymbol[] = "▎";

static bool panelOpen;

// a change in a file that differs from the last commit: its entry, the line it is marked
// on, which of the file's changes it is, and the lines it changes.
typedef struct Changed {
  u64 serial;
  u64 line;
  u64 hunk;
  ChangeCounts counts;
} Changed;

// the changes found, grouped by file in scan order, and how many files they are in, of
// those scanned and of those in a repository.
static Changed *changed;
static u64 changedCount;
static u64 changedCapacity;
static u64 changedFiles;
static u64 scannedCount;
static u64 trackedCount;

static void
DescribeCounts(const ChangeCounts *counts, StrBuf *text)
{
  StrBufAppendFormat(text, "+%lu ~%lu -%lu", counts->added, counts->changed, counts->removed);
}

// the current document's hunks, or nullptr after saying why there are none.
static const DiffHunk *
CurrentHunks(Document *document, u64 *count)
{
  *count = 0;

  if (!document->changes) {
    StrBufAppendString(message, "not in a git repository");
    return nullptr;
  }

  ChangesRefresh(document->changes);

  if (!ChangesTracked(document->changes)) {
    StrBufAppendString(message, "not in the last commit");
    return nullptr;
  }

  const DiffHunk *hunks = ChangesHunks(document->changes, count);
  if (!*count) StrBufAppendString(message, "no changes since the last commit");
  return *count ? hunks : nullptr;
}

void
ReviewStep(bool backward)
{
  Document *document = WorkspaceDocument(workspace);
  u64 count;
  const DiffHunk *hunks = CurrentHunks(document, &count);
  if (!hunks) return;

  View *view = document->view;
  u64 line = CursorSetPrimary(&view->cursors)->head.line;
  u64 found = count;

  for (u64 index = 0; index < count; index++) {
    u64 at = ChangesMarkLine(document->changes, &hunks[index]);
    if (backward && at < line) found = index;
    if (!backward && at > line) {
      found = index;
      break;
    }
  }

  if (found == count) {
    found = backward ? count - 1 : 0;
    StrBufAppendString(message, backward ? "wrapped to the end" : "wrapped to the start");
  }

  Position position = {ChangesMarkLine(document->changes, &hunks[found]), 0};
  JumpRecord();
  MoveSelect(view, position, position);
}

void
ReviewRevert(void)
{
  Document *document = WorkspaceDocument(workspace);
  u64 count;
  const DiffHunk *hunks = CurrentHunks(document, &count);
  if (!hunks) return;

  View *view = document->view;
  u64 index = ChangesHunkAt(document->changes, CursorSetPrimary(&view->cursors)->head.line);

  if (index == UINT64_MAX) {
    StrBufAppendString(message, "no change at the cursor");
    return;
  }

  // the hunks are worked out again after the edit, so this one is copied.
  DiffHunk hunk = hunks[index];
  Buffer *buffer = &document->buffer;
  bool atEnd = hunk.newStart + hunk.newCount >= BufferLineCount(buffer);
  Range range = {{hunk.newStart, 0}, {hunk.newStart + hunk.newCount, 0}};
  StrBuf text = {};

  // lines that run to the end of the buffer have no line break after them to replace, so
  // the one before them is replaced instead.
  if (atEnd) {
    range.end = BufferEnd(buffer);

    if (hunk.newStart > 0) {
      u64 above = hunk.newStart - 1;
      range.start = (Position){above, BufferLine(buffer, above)->length};
      StrBufAppendByte(&text, '\n');
    }
  }

  for (u64 line = 0; line < hunk.oldCount; line++) {
    const char *data;
    u64 length;
    ChangesBaseLine(document->changes, hunk.oldStart + line, &data, &length);
    StrBufAppend(&text, data, length);
    if (!atEnd || line + 1 < hunk.oldCount) StrBufAppendByte(&text, '\n');
  }

  // lines added at the end go with the line break before them, leaving nothing in their
  // place.
  if (!hunk.oldCount) StrBufClear(&text);

  JumpRecord();
  UndoBegin(&document->undo, UndoKindOther, &view->cursors);
  BufferReplace(buffer, range, text.data ? text.data : "", text.length);
  Position start = BufferClamp(buffer, (Position){hunk.newStart, 0});
  MoveSelect(view, start, start);
  UndoEnd(&document->undo, &view->cursors);
  StrBufFree(&text);

  if (hunk.oldCount) {
    const char *plural = StrBufPlural(hunk.oldCount);
    StrBufAppendFormat(
      message,
      "restored %lu line%s of the last commit",
      hunk.oldCount,
      plural
    );
  } else {
    const char *plural = StrBufPlural(hunk.newCount);
    StrBufAppendFormat(message, "removed %lu added line%s", hunk.newCount, plural);
  }
}

bool
ReviewPanelOpen(void)
{
  return panelOpen;
}

void
ReviewTogglePanel(i32 width)
{
  panelOpen = !panelOpen;

  if (panelOpen && width < minSplitWidth) {
    StrBufAppendFormat(
      message,
      "the change panel needs a window %d columns wide",
      minSplitWidth
    );
  }

  if (panelOpen) ChangesRefresh(WorkspaceDocument(workspace)->changes);
}

// lays out a line of a diff in rows width columns wide: wrapped when soft wrap is on, using
// the whole width since no cursor needs the last column, or else one row cut at the edge.
static void
WrapText(const char *text, u64 length, i32 width, LayoutRows *rows)
{
  LayoutWrap(text, length, workspace->options->softWrap ? width + 1 : 0, rows);
}

// draws the first count rows of text, laid out in rows, from row and column, cut at width,
// in style.
static void
DrawText(
  i32 row,
  i32 count,
  i32 column,
  i32 width,
  const char *text,
  u64 length,
  const LayoutRows *rows,
  Style style
)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  Style control = ThemeStyle(ThemeControl);
  control.background = style.background;
  i32 index = 0;
  LayoutRow layoutRow = LayoutGetRow(rows, 0, length);
  u64 left = 0;

  while (LayoutNext(&iterator, &cluster)) {
    // each wrapped row starts at its own first column.
    while (!layoutRow.last && cluster.start >= layoutRow.end && index < count) {
      layoutRow = LayoutGetRow(rows, (u64)++index, length);
      left = cluster.column;
    }

    if (index == count) break;

    i64 x = (i64)(cluster.column - left);
    if (x + cluster.width > width && layoutRow.last) break;
    if (x + cluster.width > width) continue;

    RenderCluster(row + index, column + (i32)x, text, &cluster, style, control);
  }
}

// one row of a diff: a line of the newer text (unchanged, or new in a change) or of the
// base (old in a change).
enum RowKind {
  RowContext,
  RowOld,
  RowNew
};

typedef struct PanelRow {
  enum RowKind kind;
  u64 line;
} PanelRow;

// the rows showing a hunk: context before, the old lines, the new lines, context after.
static u64
PanelRows(const Changes *changes, const DiffHunk *hunk, PanelRow *rows)
{
  u64 count = 0;
  u64 lineCount = ChangesLineCount(changes);
  u64 before = hunk->newStart > contextLines ? hunk->newStart - contextLines : 0;
  u64 end = hunk->newStart + hunk->newCount;
  u64 after = end + contextLines < lineCount ? end + contextLines : lineCount;

  for (u64 line = before; line < hunk->newStart; line++) {
    rows[count++] = (PanelRow){RowContext, line};
  }

  for (u64 line = 0; line < hunk->oldCount; line++) {
    rows[count++] = (PanelRow){RowOld, hunk->oldStart + line};
  }

  for (u64 line = hunk->newStart; line < end; line++) rows[count++] = (PanelRow){RowNew, line};
  for (u64 line = end; line < after; line++) rows[count++] = (PanelRow){RowContext, line};
  return count;
}

// the colour of the mark on a row of a hunk: its first min(old, new) lines on either side
// are changed in place, and the rest are added or removed.
static enum ThemeGroup
RowMark(const DiffHunk *hunk, PanelRow row)
{
  u64 inPlace = hunk->oldCount < hunk->newCount ? hunk->oldCount : hunk->newCount;

  if (row.kind == RowOld) {
    return row.line - hunk->oldStart < inPlace ? ThemeChangeModified : ThemeChangeRemoved;
  }

  return row.line - hunk->newStart < inPlace ? ThemeChangeModified : ThemeChangeAdded;
}

// the text of a row of a diff, from the base for an old line.
static void
RowText(const Changes *changes, PanelRow panelRow, const char **text, u64 *length)
{
  if (panelRow.kind == RowOld) ChangesBaseLine(changes, panelRow.line, text, length);
  else ChangesLine(changes, panelRow.line, text, length);
}

// the columns left for the text of a row width wide, after the mark, the sign, the line
// number in digits columns and a space.
static i32
TextWidth(i32 width, i32 digits)
{
  return width - 1 - 2 - digits - 1;
}

// the screen rows a row of a diff takes when width wide (one, unless soft wrap is on).
static u64
RowHeight(const Changes *changes, PanelRow panelRow, i32 width, i32 digits)
{
  i32 textWidth = TextWidth(width, digits);
  if (!workspace->options->softWrap || textWidth <= 0) return 1;

  const char *text;
  u64 length;
  RowText(changes, panelRow, &text, &length);
  LayoutRows rows = {};
  WrapText(text, length, textWidth, &rows);
  u64 count = rows.count;
  LayoutRowsFree(&rows);
  return count;
}

// draws a row of a diff from screen row (and above bottom) from column, width wide: a
// mark in the first cell (for a row of hunk), a sign, the line number in digits columns, a
// space and the text, wrapped when soft wrap is on.
// returns the screen rows it took.
static i32
DrawRow(
  const Changes *changes,
  const DiffHunk *hunk,
  PanelRow panelRow,
  i32 row,
  i32 bottom,
  i32 column,
  i32 width,
  i32 digits
)
{
  i32 textColumn = column + width - TextWidth(width, digits);
  i32 textWidth = TextWidth(width, digits);
  if (textWidth <= 0 || row >= bottom) return 1;

  Style style = styleDefault;
  const char *sign = " ";
  const char *text;
  u64 length;
  RowText(changes, panelRow, &text, &length);

  if (panelRow.kind == RowOld) {
    style = ThemeStyle(ThemeDiffOld);
    sign = "-";
  } else if (panelRow.kind == RowNew) {
    style = ThemeStyle(ThemeDiffNew);
    sign = "+";
  }

  LayoutRows rows = {};
  WrapText(text, length, textWidth, &rows);
  i32 count = rows.count < (u64)(bottom - row) ? (i32)rows.count : bottom - row;

  // the mark and background run down every row of a wrapped line.
  for (i32 at = row; at < row + count && panelRow.kind != RowContext; at++) {
    ScreenPutString(at, column, markSymbol, ThemeStyle(RowMark(hunk, panelRow)));
    ScreenFill(at, column + 1, width - 1, style);
  }

  StrBuf gutter = {};
  StrBufAppendFormat(&gutter, "%s %*lu", sign, digits, panelRow.line + 1);
  Style number = ThemeStyle(ThemeGutter);
  number.background = style.background;
  if (panelRow.kind != RowContext) number.foreground = colorDefault;
  ScreenPutText(row, column + 1, gutter.data, gutter.length, number);
  StrBufFree(&gutter);

  DrawText(row, count, textColumn, textWidth, text, length, &rows, style);
  LayoutRowsFree(&rows);
  return count;
}

// the columns line numbers up to highest take.
static i32
DigitsOf(u64 highest)
{
  i32 digits = 1;
  for (u64 number = highest; number >= 10; number /= 10) digits++;
  return digits;
}

static void
DrawHeader(const Document *document, const DiffHunk *hunk, i32 column, i32 width)
{
  StrBuf header = {};
  u64 index = workspace->current;
  WorkspaceName(workspace, index, &header);

  if (hunk) {
    StrBufAppendFormat(&header, ":%lu  ", ChangesMarkLine(document->changes, hunk) + 1);
    ChangeCounts counts = {};
    ChangesCountHunk(hunk, &counts);
    DescribeCounts(&counts, &header);
  }

  StrBuf shown = {};
  LayoutShorten(header.data, header.length, width - 2, &shown);
  ScreenPutText(0, column + 1, shown.data, shown.length, ThemeStyle(ThemeGutter));
  StrBufFree(&shown);
  StrBufFree(&header);
}

// says why there is no change to show, with where the next one is.
static void
DrawNoChange(Document *document, u64 line, i32 column)
{
  Changes *changes = document->changes;
  Style faint = ThemeStyle(ThemeGutter);
  const char *why = "no change at the cursor";
  if (!changes) why = "not in a git repository";
  else if (!ChangesTracked(changes)) why = "not in the last commit";
  ScreenPutString(1, column + 1, why, faint);

  u64 count;
  const DiffHunk *hunks = ChangesHunks(changes, &count);
  if (!count) return;

  u64 next = count;
  for (u64 index = 0; index < count && next == count; index++) {
    if (ChangesMarkLine(changes, &hunks[index]) > line) next = index;
  }

  StrBuf note = {};
  if (next < count) {
    u64 at = ChangesMarkLine(changes, &hunks[next]);
    StrBufAppendFormat(&note, "next change at line %lu", at + 1);
  } else {
    StrBufAppendFormat(&note, "%lu change%s above", count, StrBufPlural(count));
  }
  ScreenPutString(2, column + 1, note.data, faint);
  StrBufFree(&note);
}

void
ReviewDrawPanel(i32 column, i32 width, i32 height)
{
  Document *document = WorkspaceDocument(workspace);
  u64 line = CursorSetPrimary(&document->view->cursors)->head.line;
  u64 index = document->changes ? ChangesHunkAt(document->changes, line) : UINT64_MAX;
  u64 count;
  const DiffHunk *hunks = ChangesHunks(document->changes, &count);
  const DiffHunk *hunk = index < count ? &hunks[index] : nullptr;
  DrawHeader(document, hunk, column, width);

  if (!hunk) {
    DrawNoChange(document, line, column);
    return;
  }

  u64 most = hunk->oldCount + hunk->newCount + 2 * contextLines;
  PanelRow *rows = MemoryAllocate(most * sizeof(PanelRow));
  Changes *changes = document->changes;
  u64 rowCount = PanelRows(changes, hunk, rows);

  u64 highest = hunk->oldStart + hunk->oldCount;
  if (hunk->newStart + hunk->newCount + contextLines > highest) {
    highest = hunk->newStart + hunk->newCount + contextLines;
  }
  i32 digits = DigitsOf(highest);

  // the rows below the header scroll to keep the cursor's line in sight, with the context
  // after it: the first row shown is the earliest from which those rows fit.
  u64 shown = height > 1 ? (u64)height - 1 : 0;
  u64 cursorRow = 0;
  for (u64 at = 0; at < rowCount; at++) {
    if (rows[at].kind != RowOld && rows[at].line == line) cursorRow = at;
  }

  u64 last = cursorRow + contextLines < rowCount ? cursorRow + contextLines : rowCount - 1;
  u64 first = last + 1;
  u64 used = 0;

  while (first > 0) {
    u64 rowHeight = RowHeight(changes, rows[first - 1], width, digits);
    if (used + rowHeight > shown) break;
    used += rowHeight;
    first--;
  }

  if (first > cursorRow) first = cursorRow;

  for (u64 at = first, row = 1; at < rowCount && row < (u64)height; at++) {
    row += (u64)DrawRow(changes, hunk, rows[at], (i32)row, height, column, width, digits);
  }

  free(rows);
}

// the changes picker.

// adds the changes of a file: of its document when it is open, or else of its text on disk.
static void
AddChanged(u64 serial, const Changes *changes, const DiffHunk *hunks, u64 count)
{
  if (!count) return;

  changedFiles++;

  for (u64 index = 0; index < count; index++) {
    if (changedCount == changedCapacity) {
      changedCapacity = changedCapacity ? changedCapacity * 2 : 64;
      changed = MemoryResize(changed, changedCapacity, sizeof(Changed));
    }

    const DiffHunk *hunk = &hunks[index];
    u64 line = ChangesMarkLine(changes, hunk);
    Changed *change = &changed[changedCount++];
    *change = (Changed){.serial = serial, .line = line, .hunk = index};
    ChangesCountHunk(hunk, &change->counts);
  }
}

static bool
VisitEntry([[maybe_unused]] void *context, const ScanItem *item)
{
  u64 serial = workspace->entries[item->index].serial;
  const Document *document = item->document;
  const char *path = WorkspacePath(workspace, item->index)->data;
  scannedCount++;

  // an open document's changes follow its buffer; a file on disk is compared once.
  Changes *changes = document ? document->changes
                              : ChangesNewText(path, item->text, item->length);
  trackedCount += changes != nullptr;
  if (document) ChangesRefresh(changes);
  u64 count;
  const DiffHunk *hunks = ChangesHunks(changes, &count);
  AddChanged(serial, changes, hunks, count);
  if (!document) ChangesFree(changes);
  return true;
}

static void
Progress([[maybe_unused]] void *context, u64 done, u64 count, StrBuf *text)
{
  StrBufAppendFormat(
    text,
    "comparing %lu of %lu file%s with the last commit",
    done,
    count,
    StrBufPlural(count)
  );
  StrBufAppendFormat(text, ": %lu changed (escape stops)", changedFiles);
}

static void
DescribeChanged(bool stopped, StrBuf *text)
{
  ChangeCounts total = {};

  for (u64 index = 0; index < changedCount; index++) {
    total.added += changed[index].counts.added;
    total.changed += changed[index].counts.changed;
    total.removed += changed[index].counts.removed;
  }

  StrBufAppendFormat(
    text,
    "%lu change%s in %lu file%s since the last commit: ",
    changedCount,
    StrBufPlural(changedCount),
    changedFiles,
    StrBufPlural(changedFiles)
  );
  DescribeCounts(&total, text);
  const char *plural = StrBufPlural(scannedCount);
  if (stopped) StrBufAppendFormat(text, " (stopped after %lu file%s)", scannedCount, plural);
}

// the picker's preview: the whole of the selected change's file, with the base's lines of
// each change before its new ones, placed at that change and scrolled on its own.
static constexpr u64 noItem = UINT64_MAX;

// the entry whose changes are read, the changes read for it when it is not open, and
// whether they were.
static u64 previewSerial = noItem;
static Changes *previewOwned;
static bool previewRead;

// the item previewed, the first row shown (noItem until placed at its change) and the
// rows shown at the last draw.
static u64 previewId = noItem;
static u64 previewTop;
static u64 previewHeight;

static void
ForgetPreview(void)
{
  ChangesFree(previewOwned);
  previewOwned = nullptr;
  previewRead = false;
  previewSerial = noItem;
  previewId = noItem;
}

// the changes of the file of the item with id: its document's, or read from its file,
// once for all the items of that file.
static Changes *
PreviewChanges(u64 id)
{
  u64 serial = changed[id].serial;

  if (serial != previewSerial) {
    ForgetPreview();
    previewSerial = serial;
  }

  if (id != previewId) {
    previewId = id;
    previewTop = noItem;
  }

  u64 index = WorkspaceEntryOf(workspace, serial);
  if (index == workspaceNone) return nullptr;

  Document *document = workspace->entries[index].document;
  if (document) return document->changes;

  if (!previewRead) {
    previewRead = true;
    const char *path = WorkspacePath(workspace, index)->data;
    StrBuf text = {};
    FileStamp stamp;

    if (FileRead(path, &text, &stamp, nullptr) == FileStatusOk) {
      previewOwned = ChangesNewText(path, text.data, text.length);
    }

    StrBufFree(&text);
  }

  return previewOwned;
}

// the row each hunk's base lines start on, with the base lines of every hunk before it.
static u64 *
HunkRows(const DiffHunk *hunks, u64 count)
{
  u64 *rows = MemoryAllocate((count ? count : 1) * sizeof(u64));
  u64 old = 0;

  for (u64 index = 0; index < count; index++) {
    rows[index] = hunks[index].newStart + old;
    old += hunks[index].oldCount;
  }

  return rows;
}

// what row of the whole diff shows, setting hunk to the hunk it belongs to (or nullptr).
static PanelRow
RowAt(const DiffHunk *hunks, const u64 *hunkRows, u64 count, u64 row, const DiffHunk **hunk)
{
  *hunk = nullptr;

  // the last hunk starting at or before the row.
  u64 low = 0;
  u64 high = count;

  while (low < high) {
    u64 middle = low + (high - low) / 2;

    if (hunkRows[middle] <= row) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }

  if (!low) return (PanelRow){RowContext, row};

  const DiffHunk *found = &hunks[low - 1];
  u64 start = hunkRows[low - 1];

  if (row < start + found->oldCount) {
    *hunk = found;
    return (PanelRow){RowOld, found->oldStart + row - start};
  }

  // rows past the base lines are lines of the newer text, offset by every base line so far.
  u64 line = found->newStart + (row - start - found->oldCount);
  if (line < found->newStart + found->newCount) *hunk = found;
  return (PanelRow){*hunk ? RowNew : RowContext, line};
}

// the last row the preview may scroll to: the earliest from which the rows to the end
// fit in it.
static u64
LastTop(
  const Changes *changes,
  const DiffHunk *hunks,
  const u64 *hunkRows,
  u64 count,
  u64 rowCount,
  i32 width,
  i32 digits
)
{
  u64 top = rowCount;
  u64 used = 0;

  while (top > 0) {
    const DiffHunk *hunk;
    PanelRow panelRow = RowAt(hunks, hunkRows, count, top - 1, &hunk);
    u64 rowHeight = RowHeight(changes, panelRow, width, digits);
    if (used + rowHeight > previewHeight) break;
    used += rowHeight;
    top--;
  }

  return top < rowCount ? top : rowCount - 1;
}

static void
DrawPreview([[maybe_unused]] void *context, u64 id, i32 column, i32 width, i32 height)
{
  Changes *changes = PreviewChanges(id);
  u64 count;
  const DiffHunk *hunks = ChangesHunks(changes, &count);
  previewHeight = height > 1 ? (u64)height - 1 : 0;

  StrBuf header = {};
  WorkspaceName(workspace, WorkspaceEntryOf(workspace, changed[id].serial), &header);
  StrBufAppendFormat(&header, ":%lu  ", changed[id].line + 1);
  DescribeCounts(&changed[id].counts, &header);

  if (!count) {
    ScreenPutString(0, column + 1, header.data, ThemeStyle(ThemeGutter));
    ScreenPutString(1, column + 1, "no changes to show", ThemeStyle(ThemeGutter));
    StrBufFree(&header);
    return;
  }

  // the file may have changed on disk since it was compared, leaving fewer changes.
  u64 hunk = changed[id].hunk < count ? changed[id].hunk : count - 1;
  StrBufAppendFormat(&header, "  change %lu of %lu", hunk + 1, count);
  StrBuf shortened = {};
  LayoutShorten(header.data, header.length, width - 2, &shortened);
  ScreenPutText(0, column + 1, shortened.data, shortened.length, ThemeStyle(ThemeGutter));
  StrBufFree(&shortened);
  StrBufFree(&header);

  u64 *hunkRows = HunkRows(hunks, count);
  u64 oldTotal = 0;
  u64 newTotal = 0;
  for (u64 index = 0; index < count; index++) oldTotal += hunks[index].oldCount;
  for (u64 index = 0; index < count; index++) newTotal += hunks[index].newCount;
  u64 lineCount = ChangesLineCount(changes);
  u64 rowCount = lineCount + oldTotal;
  u64 baseCount = lineCount - newTotal + oldTotal;
  i32 digits = DigitsOf(lineCount > baseCount ? lineCount : baseCount);

  // the change starts a few lines down, with the lines before it for context.
  if (previewTop == noItem) {
    previewTop = hunkRows[hunk] > contextLines ? hunkRows[hunk] - contextLines : 0;
  }

  u64 lastTop = LastTop(changes, hunks, hunkRows, count, rowCount, width, digits);
  if (previewTop > lastTop) previewTop = lastTop;

  for (u64 at = previewTop, row = 1; at < rowCount && row < (u64)height; at++) {
    const DiffHunk *rowHunk;
    PanelRow panelRow = RowAt(hunks, hunkRows, count, at, &rowHunk);
    row += (u64)DrawRow(changes, rowHunk, panelRow, (i32)row, height, column, width, digits);
  }

  free(hunkRows);
}

// scrolls the preview by rows of the diff.
static void
ScrollPreview(i64 rows)
{
  if (previewTop == noItem) return;

  i64 top = (i64)previewTop + rows;
  previewTop = top > 0 ? (u64)top : 0;
}

// shift with up, down, page up and page down scrolls the preview.
static bool
PreviewKey([[maybe_unused]] void *context, Key key)
{
  i64 page = previewHeight > 1 ? (i64)previewHeight - 1 : 1;
  if (key.modifiers != KeyModifierShift) return false;

  switch (key.code) {
    case KeyUp:
      ScrollPreview(-1);
      return true;
    case KeyDown:
      ScrollPreview(1);
      return true;
    case KeyPageUp:
      ScrollPreview(-page);
      return true;
    case KeyPageDown:
      ScrollPreview(page);
      return true;
    default:
      return false;
  }
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  ForgetPreview();

  if (!picked) return;

  u64 index = WorkspaceEntryOf(workspace, changed[id].serial);

  if (index == workspaceNone) {
    StrBufAppendString(message, "that buffer has been closed");
    return;
  }

  JumpRecord();
  if (!WorkspaceSwitch(workspace, index, message)) return;

  // the file may have changed since it was compared, so its changes are found again.
  Document *document = WorkspaceDocument(workspace);
  Changes *changes = document->changes;
  u64 count;
  const DiffHunk *hunks = ChangesHunks(changes, &count);
  u64 hunk = changed[id].hunk;
  u64 line = hunk < count ? ChangesMarkLine(changes, &hunks[hunk]) : changed[id].line;
  u64 last = BufferLineCount(&document->buffer) - 1;
  Position position = {line <= last ? line : last, 0};
  MoveSelect(document->view, position, position);
}

static void
Finish([[maybe_unused]] void *context, ScanResult result)
{
  StrBufClear(message);

  // the same as the commands about the current buffer say outside a repository.
  if (!changedCount && !trackedCount) {
    StrBufAppendString(message, "not in a git repository");
    if (result.stopped) StrBufAppendString(message, " (stopped)");
    return;
  }

  if (!changedCount) {
    StrBufAppendFormat(
      message,
      "no changes since the last commit in %lu file%s",
      scannedCount,
      StrBufPlural(scannedCount)
    );
    if (result.stopped) StrBufAppendString(message, " (stopped)");
    return;
  }

  PickerBegin();
  StrBuf item = {};
  StrBuf note = {};
  u64 serial = noItem;
  u64 index = workspaceNone;

  for (u64 at = 0; at < changedCount; at++) {
    // changes come grouped by file.
    if (changed[at].serial != serial) {
      serial = changed[at].serial;
      index = WorkspaceEntryOf(workspace, serial);
    }

    StrBufClear(&item);
    StrBufClear(&note);
    WorkspaceName(workspace, index, &item);
    StrBufAppendFormat(&item, ":%lu", changed[at].line + 1);
    DescribeCounts(&changed[at].counts, &note);
    PickerAdd(item.data, item.length, note.data, at);

    if (WorkspaceModified(workspace, index)) PickerMark("+");
    else if (workspace->entries[index].document) PickerMark("·");
  }

  StrBuf title = {};
  DescribeChanged(result.stopped, &title);
  StrBufAppend(message, title.data, title.length);
  PickerOpen(title.data, "filter: ", Picked, nullptr);
  PickerShowPreview(DrawPreview, nullptr);
  PickerHandleKeys(PreviewKey, nullptr);
  StrBufFree(&title);
  StrBufFree(&item);
  StrBufFree(&note);
}

void
ReviewPickChanged(void)
{
  changedCount = 0;
  changedFiles = 0;
  scannedCount = 0;
  trackedCount = 0;
  u64 *queue = MemoryAllocate((workspace->count ? workspace->count : 1) * sizeof(u64));
  u64 queueCount = 0;

  // only named files can be in a repository, and refused ones are never read.
  for (u64 index = 0; index < workspace->count; index++) {
    const WorkspaceEntry *entry = &workspace->entries[index];
    bool named = WorkspacePath(workspace, index)->length != 0;
    if (named && entry->refusal == WorkspaceRefusalNone) queue[queueCount++] = index;
  }

  static const ScanClient client =
    {.visit = VisitEntry, .progress = Progress, .finish = Finish};
  ScanStart(queue, queueCount, &client, nullptr);
  free(queue);
  StrBufAppendFormat(
    message,
    "comparing %lu file%s with the last commit",
    queueCount,
    StrBufPlural(queueCount)
  );
}
