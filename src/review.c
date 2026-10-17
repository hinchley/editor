#include "review.h"

#include <stdlib.h>
#include <string.h>

#include "changes.h"
#include "file.h"
#include "jump.h"
#include "layout.h"
#include "memory.h"
#include "move.h"
#include "picker.h"
#include "scan.h"
#include "screen.h"
#include "settings.h"
#include "theme.h"

// unchanged lines the panel shows before and after a change.
static constexpr u64 contextLines = 3;

static constexpr char invalidSymbol[] = "�";

// marks a changed row, as the gutter does.
static constexpr char markSymbol[] = "▎";

static Workspace *workspace;
static StrBuf *message;
static bool panelOpen;

// a file found to differ from the last commit: its entry, the line of its first change
// and the lines it changes.
typedef struct Changed {
  u64 serial;
  u64 firstLine;
  ChangeCounts counts;
} Changed;

static Changed *changed;
static u64 changedCount;
static u64 changedCapacity;
static u64 scannedCount;

void
ReviewInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

static void
DescribeCounts(const ChangeCounts *counts, StrBuf *text)
{
  StrBufAppendFormat(text, "+%lu ~%lu -%lu", counts->added, counts->changed, counts->removed);
}

// the line a hunk is marked and gone to on: its first new line, or for a removal the line
// after it (the last line for a removal at the end).
static u64
HunkLine(const Document *document, const DiffHunk *hunk)
{
  u64 last = BufferLineCount(&document->buffer) - 1;
  return hunk->newStart <= last ? hunk->newStart : last;
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
    u64 at = HunkLine(document, &hunks[index]);
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

  Position position = {HunkLine(document, &hunks[found]), 0};
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
    const char *plural = hunk.oldCount == 1 ? "" : "s";
    StrBufAppendFormat(message, "restored %lu line%s of the last commit", hunk.oldCount, plural);
  } else {
    const char *plural = hunk.newCount == 1 ? "" : "s";
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
    const char *format = "the change panel needs a window %d columns wide";
    StrBufAppendFormat(message, format, minSplitWidth);
  }

  if (panelOpen) ChangesRefresh(WorkspaceDocument(workspace)->changes);
}

// draws text from column, cut at width, in style.
static void
DrawText(i32 row, i32 column, i32 width, const char *text, u64 length, Style style)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  Style control = ThemeStyle(ThemeControl);
  control.background = style.background;

  while (LayoutNext(&iterator, &cluster)) {
    i64 x = (i64)cluster.column;
    if (x + cluster.width > width) break;

    i32 at = column + (i32)x;
    char symbol[2] = {'^', (char)(text[cluster.start] ^ 0x40)};

    switch (cluster.kind) {
    case LayoutKindText:
      u64 size = cluster.end - cluster.start;
      ScreenPutCluster(row, at, text + cluster.start, size, cluster.width, style);
      break;
    case LayoutKindSpace:
    case LayoutKindTab:
      break;
    case LayoutKindControl:
      ScreenPutText(row, at, symbol, 2, control);
      break;
    case LayoutKindInvalid:
      ScreenPutCluster(row, at, invalidSymbol, strlen(invalidSymbol), 1, control);
      break;
    }
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

// draws a row of a diff on screen row from column, width wide: a mark in the first cell
// (for a row of hunk), a sign, the line number in digits columns, a space and the text.
static void
DrawRow(
  const Changes *changes,
  const DiffHunk *hunk,
  PanelRow panelRow,
  i32 row,
  i32 column,
  i32 width,
  i32 digits
)
{
  i32 textColumn = column + 1 + 2 + digits + 1;
  i32 textWidth = column + width - textColumn;
  if (textWidth <= 0) return;

  Style style = styleDefault;
  const char *sign = " ";
  const char *text;
  u64 length;

  if (panelRow.kind == RowOld) {
    style = ThemeStyle(ThemeDiffOld);
    sign = "-";
    ChangesBaseLine(changes, panelRow.line, &text, &length);
  } else {
    if (panelRow.kind == RowNew) style = ThemeStyle(ThemeDiffNew);
    if (panelRow.kind == RowNew) sign = "+";
    ChangesLine(changes, panelRow.line, &text, &length);
  }

  if (panelRow.kind != RowContext) {
    ScreenPutString(row, column, markSymbol, ThemeStyle(RowMark(hunk, panelRow)));
    ScreenFill(row, column + 1, width - 1, style);
  }

  StrBuf gutter = {};
  StrBufAppendFormat(&gutter, "%s %*lu", sign, digits, panelRow.line + 1);
  Style number = ThemeStyle(ThemeGutter);
  number.background = style.background;
  if (panelRow.kind != RowContext) number.foreground = colorDefault;
  ScreenPutText(row, column + 1, gutter.data, gutter.length, number);
  StrBufFree(&gutter);

  DrawText(row, textColumn, textWidth, text, length, style);
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
    StrBufAppendFormat(&header, ":%lu  ", HunkLine(document, hunk) + 1);
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
  Style faint = ThemeStyle(ThemeGutter);
  const char *why = "no change at the cursor";
  if (!document->changes) why = "not in a git repository";
  else if (!ChangesTracked(document->changes)) why = "not in the last commit";
  ScreenPutString(1, column + 1, why, faint);

  u64 count;
  const DiffHunk *hunks = ChangesHunks(document->changes, &count);
  if (!count) return;

  u64 next = count;
  for (u64 index = 0; index < count && next == count; index++) {
    if (HunkLine(document, &hunks[index]) > line) next = index;
  }

  StrBuf note = {};
  if (next < count) {
    StrBufAppendFormat(&note, "next change at line %lu", HunkLine(document, &hunks[next]) + 1);
  } else {
    StrBufAppendFormat(&note, "%lu change%s above", count, count == 1 ? "" : "s");
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

  // the rows below the header scroll to keep the cursor's line in sight.
  u64 shown = height > 1 ? (u64)height - 1 : 0;
  u64 first = 0;
  u64 cursorRow = 0;
  for (u64 at = 0; at < rowCount; at++) {
    if (rows[at].kind != RowOld && rows[at].line == line) cursorRow = at;
  }
  if (rowCount > shown && cursorRow + contextLines >= shown) {
    first = cursorRow + contextLines + 1 - shown;
    if (first > rowCount - shown) first = rowCount - shown;
  }

  u64 highest = hunk->oldStart + hunk->oldCount;
  if (hunk->newStart + hunk->newCount + contextLines > highest) {
    highest = hunk->newStart + hunk->newCount + contextLines;
  }
  i32 digits = DigitsOf(highest);

  for (u64 at = first; at < rowCount && at - first < shown; at++) {
    DrawRow(changes, hunk, rows[at], 1 + (i32)(at - first), column, width, digits);
  }

  free(rows);
}

// the changed files picker.

static void
AddChanged(u64 serial, u64 firstLine, const DiffHunk *hunks, u64 count)
{
  if (!count) return;

  if (changedCount == changedCapacity) {
    changedCapacity = changedCapacity ? changedCapacity * 2 : 64;
    changed = MemoryResize(changed, changedCapacity, sizeof(Changed));
  }

  Changed *file = &changed[changedCount++];
  *file = (Changed){.serial = serial, .firstLine = firstLine};
  for (u64 index = 0; index < count; index++) ChangesCountHunk(&hunks[index], &file->counts);
}

static bool
VisitEntry([[maybe_unused]] void *context, const ScanItem *item)
{
  u64 serial = workspace->entries[item->index].serial;
  scannedCount++;

  if (item->document) {
    Changes *changes = item->document->changes;
    ChangesRefresh(changes);
    u64 count;
    const DiffHunk *hunks = ChangesHunks(changes, &count);
    if (count) AddChanged(serial, HunkLine(item->document, &hunks[0]), hunks, count);
    return true;
  }

  DiffHunks hunks = {};
  const char *path = WorkspacePath(workspace, item->index)->data;
  ChangesOfText(path, item->text, item->length, &hunks);
  if (hunks.count) AddChanged(serial, hunks.hunks[0].newStart, hunks.hunks, hunks.count);
  DiffFree(&hunks);
  return true;
}

static void
Progress([[maybe_unused]] void *context, u64 done, u64 count, StrBuf *text)
{
  StrBufAppendFormat(text, "comparing %lu of %lu files with the last commit", done, count);
  StrBufAppendFormat(text, ": %lu changed (escape stops)", changedCount);
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

  const char *plural = changedCount == 1 ? "" : "s";
  StrBufAppendFormat(text, "%lu file%s changed since the last commit: ", changedCount, plural);
  DescribeCounts(&total, text);
  if (stopped) StrBufAppendFormat(text, " (stopped after %lu files)", scannedCount);
}

// the picker's preview: the whole of the selected file, with the base's lines of each change
// before its new ones, scrolled on its own.
static constexpr u64 noItem = UINT64_MAX;

// the item previewed, the changes read for it when it is not open, and whether they were.
static u64 previewId = noItem;
static Changes *previewOwned;
static bool previewRead;

// the first row shown (noItem until placed at the first change), the rows shown at the
// last draw, the change a step went to (or noItem) and the change at the top.
static u64 previewTop;
static u64 previewHeight;
static u64 previewStep;
static u64 previewCurrent;

static void
ForgetPreview(void)
{
  ChangesFree(previewOwned);
  previewOwned = nullptr;
  previewRead = false;
  previewId = noItem;
}

// the changes of the item with id: its document's, or read from its file.
static Changes *
PreviewChanges(u64 id)
{
  if (id != previewId) {
    ForgetPreview();
    previewId = id;
    previewTop = noItem;
    previewStep = noItem;
    previewCurrent = 0;
  }

  u64 index = WorkspaceEntryOf(workspace, changed[id].serial);
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

// the row a step to a hunk scrolls to, showing a few lines before it.
static u64
HunkTop(const u64 *hunkRows, u64 index)
{
  return hunkRows[index] > contextLines ? hunkRows[index] - contextLines : 0;
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

static void
DrawPreview([[maybe_unused]] void *context, u64 id, i32 column, i32 width, i32 height)
{
  Changes *changes = PreviewChanges(id);
  u64 count;
  const DiffHunk *hunks = ChangesHunks(changes, &count);
  u64 shown = height > 1 ? (u64)height - 1 : 0;
  previewHeight = shown;

  if (!count) {
    StrBuf header = {};
    WorkspaceName(workspace, WorkspaceEntryOf(workspace, changed[id].serial), &header);
    ScreenPutString(0, column + 1, header.data, ThemeStyle(ThemeGutter));
    ScreenPutString(1, column + 1, "no changes to show", ThemeStyle(ThemeGutter));
    StrBufFree(&header);
    return;
  }

  u64 *hunkRows = HunkRows(hunks, count);
  u64 oldTotal = 0;
  u64 newTotal = 0;
  for (u64 index = 0; index < count; index++) oldTotal += hunks[index].oldCount;
  for (u64 index = 0; index < count; index++) newTotal += hunks[index].newCount;
  u64 lineCount = ChangesLineCount(changes);
  u64 rowCount = lineCount + oldTotal;

  if (previewStep < count) previewTop = HunkTop(hunkRows, previewStep);
  if (previewTop == noItem) previewTop = HunkTop(hunkRows, 0);
  u64 lastTop = rowCount > shown ? rowCount - shown : 0;
  if (previewTop > lastTop) previewTop = lastTop;

  // the change at the top is the first that ends below it, unless a step chose one.
  previewCurrent = count - 1;
  for (u64 index = 0; index < count; index++) {
    const DiffHunk *hunk = &hunks[index];

    if (hunkRows[index] + hunk->oldCount + hunk->newCount > previewTop) {
      previewCurrent = index;
      break;
    }
  }
  if (previewStep < count) previewCurrent = previewStep;

  StrBuf header = {};
  WorkspaceName(workspace, WorkspaceEntryOf(workspace, changed[id].serial), &header);
  StrBufAppendString(&header, "  ");
  DescribeCounts(&changed[id].counts, &header);
  StrBufAppendFormat(&header, "  change %lu of %lu", previewCurrent + 1, count);
  StrBuf shortened = {};
  LayoutShorten(header.data, header.length, width - 2, &shortened);
  ScreenPutText(0, column + 1, shortened.data, shortened.length, ThemeStyle(ThemeGutter));
  StrBufFree(&shortened);
  StrBufFree(&header);

  u64 baseCount = lineCount - newTotal + oldTotal;
  i32 digits = DigitsOf(lineCount > baseCount ? lineCount : baseCount);

  for (u64 row = previewTop; row < rowCount && row - previewTop < shown; row++) {
    const DiffHunk *hunk;
    PanelRow panelRow = RowAt(hunks, hunkRows, count, row, &hunk);
    DrawRow(changes, hunk, panelRow, 1 + (i32)(row - previewTop), column, width, digits);
  }

  free(hunkRows);
}

// scrolls the preview by rows, leaving the change it shows to follow the top.
static void
ScrollPreview(i64 rows)
{
  if (previewTop == noItem) return;

  previewStep = noItem;
  i64 top = (i64)previewTop + rows;
  previewTop = top > 0 ? (u64)top : 0;
}

// scrolls the preview to the next change after the one it shows, or (backward) the one
// before it.
static void
StepPreview(bool backward)
{
  if (previewId == noItem) return;

  u64 count;
  ChangesHunks(PreviewChanges(previewId), &count);

  if (!count) return;

  if (backward && previewCurrent > 0) {
    previewStep = previewCurrent - 1;
  } else if (!backward && previewCurrent + 1 < count) {
    previewStep = previewCurrent + 1;
  } else {
    StrBufAppendString(message, backward ? "this is the first change" : "this is the last change");
  }
}

// shift with up, down, page up and page down scrolls the preview, and alt+shift+] and
// alt+shift+[ step through its changes.
static bool
PreviewKey([[maybe_unused]] void *context, Key key)
{
  i64 page = previewHeight > 1 ? (i64)previewHeight - 1 : 1;

  if (key.modifiers == KeyModifierShift) {
    switch (key.code) {
    case KeyUp: ScrollPreview(-1); return true;
    case KeyDown: ScrollPreview(1); return true;
    case KeyPageUp: ScrollPreview(-page); return true;
    case KeyPageDown: ScrollPreview(page); return true;
    default: return false;
    }
  }

  if (key.modifiers == (KeyModifierAlt | KeyModifierShift)) {
    if (key.code == ']') StepPreview(false);
    if (key.code == '[') StepPreview(true);
    return key.code == ']' || key.code == '[';
  }

  return false;
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  // the change the preview showed, when it showed this item.
  u64 shownChange = id == previewId ? previewCurrent : 0;
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
  u64 count;
  const DiffHunk *hunks = ChangesHunks(document->changes, &count);
  if (shownChange >= count) shownChange = 0;
  u64 line = count ? HunkLine(document, &hunks[shownChange]) : changed[id].firstLine;
  u64 last = BufferLineCount(&document->buffer) - 1;
  Position position = {line <= last ? line : last, 0};
  MoveSelect(document->view, position, position);
}

static void
Finish([[maybe_unused]] void *context, ScanResult result)
{
  StrBufClear(message);

  if (!changedCount) {
    StrBufAppendFormat(message, "no changes since the last commit in %lu files", scannedCount);
    if (result.stopped) StrBufAppendString(message, " (stopped)");
    return;
  }

  PickerBegin();
  StrBuf name = {};
  StrBuf note = {};

  for (u64 at = 0; at < changedCount; at++) {
    u64 index = WorkspaceEntryOf(workspace, changed[at].serial);
    StrBufClear(&name);
    StrBufClear(&note);
    WorkspaceName(workspace, index, &name);
    DescribeCounts(&changed[at].counts, &note);
    PickerAdd(name.data, name.length, note.data, at);

    if (WorkspaceModified(workspace, index)) PickerMark("+");
    else if (workspace->entries[index].document) PickerMark("·");
  }

  StrBuf title = {};
  DescribeChanged(result.stopped, &title);
  StrBufAppend(message, title.data, title.length);
  PickerOpen(title.data, "file: ", Picked, nullptr);
  PickerShowPreview(DrawPreview, nullptr);
  PickerHandleKeys(PreviewKey, nullptr);
  StrBufFree(&title);
  StrBufFree(&name);
  StrBufFree(&note);
}

void
ReviewPickChanged(void)
{
  changedCount = 0;
  scannedCount = 0;
  u64 *queue = MemoryAllocate((workspace->count ? workspace->count : 1) * sizeof(u64));
  u64 queueCount = 0;

  // only named files can be in a repository, and refused ones are never read.
  for (u64 index = 0; index < workspace->count; index++) {
    const WorkspaceEntry *entry = &workspace->entries[index];
    bool named = WorkspacePath(workspace, index)->length != 0;
    if (named && entry->refusal == WorkspaceRefusalNone) queue[queueCount++] = index;
  }

  static const ScanClient client = {.visit = VisitEntry, .progress = Progress, .finish = Finish};
  ScanStart(queue, queueCount, &client, nullptr);
  free(queue);
  StrBufAppendFormat(message, "comparing %lu files with the last commit", queueCount);
}
