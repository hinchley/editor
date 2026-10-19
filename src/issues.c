#include "issues.h"

#include <stdlib.h>
#include <string.h>

#include "diagnostics.h"
#include "jump.h"
#include "layout.h"
#include "lsp.h"
#include "memory.h"
#include "move.h"
#include "picker.h"
#include "preview.h"
#include "screen.h"
#include "settings.h"
#include "theme.h"

static Workspace *workspace;
static StrBuf *message;
static bool panelOpen;

// a diagnostic in the picker: its entry, and where it starts and ends (the end on the same
// line, or UINT64_MAX).
typedef struct Place {
  u64 serial;
  Position start;
  u64 end;
} Place;

static Place *places;
static u64 placeCount;
static u64 placeCapacity;

void
IssuesInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

// the length of the first line of text.
static u64
FirstLine(const char *text)
{
  return strcspn(text, "\n");
}

// the current document's diagnostics, or nullptr after saying why there are none.
static const Diagnostics *
CurrentDiagnostics(void)
{
  Document *document = WorkspaceDocument(workspace);
  const Diagnostics *diagnostics = LspDiagnostics(document->lsp);

  if (!diagnostics) {
    StrBufAppendString(message, "no language server for this buffer");
    return nullptr;
  }

  if (!diagnostics->count) StrBufAppendString(message, "no diagnostics");
  return diagnostics->count ? diagnostics : nullptr;
}

void
IssuesStep(bool backward)
{
  const Diagnostics *diagnostics = CurrentDiagnostics();
  if (!diagnostics) return;

  View *view = WorkspaceDocument(workspace)->view;
  Position head = CursorSetPrimary(&view->cursors)->head;
  u32 index;
  bool wrapped;
  DiagnosticsStep(diagnostics, head, backward, &index, &wrapped);

  const Diagnostic *diagnostic = &diagnostics->items[index];
  Position start = BufferClamp(view->buffer, diagnostic->range.start);
  JumpRecord();
  MoveSelect(view, start, start);

  const char *around = backward ? "wrapped to the end; " : "wrapped to the start; ";
  if (wrapped) StrBufAppendString(message, around);
  const char *text = DiagnosticsText(diagnostics, diagnostic->message);
  const char *severity = DiagnosticSeverityName(diagnostic->severity);
  StrBufAppendFormat(message, "%s %u of %u: ", severity, index + 1, diagnostics->count);
  StrBufAppend(message, text, FirstLine(text));
}

bool
IssuesPanelOpen(void)
{
  return panelOpen;
}

void
IssuesTogglePanel(i32 width)
{
  panelOpen = !panelOpen;

  if (panelOpen && width < minSplitWidth) {
    const char *format = "the diagnostics panel needs a window %d columns wide";
    StrBufAppendFormat(message, format, minSplitWidth);
  }
}

// draws text wrapped to width from row and column, its lines one after another, down to
// bottom, and returns the row after it.
static i32
DrawWrapped(const char *text, i32 row, i32 bottom, i32 column, i32 width, Style style)
{
  LayoutRows rows = {};

  for (const char *line = text; row < bottom;) {
    u64 length = FirstLine(line);
    LayoutWrap(line, length, width + 1, &rows);

    for (u64 index = 0; index < rows.count && row < bottom; index++, row++) {
      LayoutRow layoutRow = LayoutGetRow(&rows, index, length);
      u64 size = layoutRow.end - layoutRow.start;

      // a row that wrapped at a blank leaves it at its end.
      while (size && !layoutRow.last && line[layoutRow.start + size - 1] == ' ') size--;
      ScreenPutText(row, column, line + layoutRow.start, size, style);
    }

    if (!line[length]) break;
    line += length + 1;
  }

  LayoutRowsFree(&rows);
  return row;
}

// draws one diagnostic from row: a heading with its severity, code and source, then its
// message and notes; returns the row after it.
static i32
DrawDiagnostic(const Diagnostics *diagnostics, const Diagnostic *diagnostic, i32 row,
  i32 bottom, i32 column, i32 width)
{
  Style group = ThemeStyle(DiagnosticSeverityGroup(diagnostic->severity));
  Style faint = ThemeStyle(ThemeGutter);
  const char *code = DiagnosticsText(diagnostics, diagnostic->code);
  const char *source = DiagnosticsText(diagnostics, diagnostic->source);

  StrBuf heading = {};
  StrBufAppendFormat(&heading, "%s %s", DiagnosticSeveritySymbol(diagnostic->severity),
    DiagnosticSeverityName(diagnostic->severity));
  if (*code) StrBufAppendFormat(&heading, " [%s]", code);
  if (*source) StrBufAppendFormat(&heading, " %s", source);
  StrBufAppendFormat(&heading, "  line %lu", diagnostic->range.start.line + 1);
  StrBuf shown = {};
  LayoutShorten(heading.data, heading.length, width, &shown);
  ScreenPutText(row++, column, shown.data, shown.length, group);
  StrBufFree(&shown);
  StrBufFree(&heading);

  row = DrawWrapped(DiagnosticsText(diagnostics, diagnostic->message), row, bottom, column,
    width, styleDefault);
  const char *notes = DiagnosticsText(diagnostics, diagnostic->notes);
  if (*notes) row = DrawWrapped(notes, row, bottom, column, width, faint);
  return row;
}

void
IssuesDrawPanel(i32 column, i32 width, i32 height)
{
  Document *document = WorkspaceDocument(workspace);
  u64 line = CursorSetPrimary(&document->view->cursors)->head.line;
  const Diagnostics *diagnostics = LspDiagnostics(document->lsp);
  Style faint = ThemeStyle(ThemeGutter);
  i32 left = column + 1;
  i32 textWidth = width - 2;

  StrBuf header = {};
  WorkspaceName(workspace, workspace->current, &header);
  StrBufAppendFormat(&header, ":%lu", line + 1);
  StrBuf shown = {};
  LayoutShorten(header.data, header.length, textWidth, &shown);
  ScreenPutText(0, left, shown.data, shown.length, faint);
  StrBufFree(&shown);
  StrBufFree(&header);

  if (!diagnostics) {
    ScreenPutString(1, left, "no language server for this buffer", faint);
    return;
  }

  // every diagnostic reaching the line, as its underline shows.
  u32 first;
  u32 end;
  DiagnosticsAround(diagnostics, line, &first, &end);
  i32 row = 1;
  bool any = false;

  for (u32 index = first; index < end && row < height; index++) {
    const Diagnostic *diagnostic = &diagnostics->items[index];
    if (diagnostic->range.end.line < line) continue;

    if (any) row++;
    any = true;
    row = DrawDiagnostic(diagnostics, diagnostic, row, height, left, textWidth);
  }

  if (any) return;

  ScreenPutString(1, left, "no diagnostics on this line", faint);
  if (!diagnostics->count) return;

  u32 next;
  bool wrapped;
  DiagnosticsStep(diagnostics, (Position){line, UINT64_MAX}, false, &next, &wrapped);
  StrBuf note = {};
  u64 at = diagnostics->items[next].range.start.line + 1;
  StrBufAppendFormat(&note, "%u in this buffer, the next at line %lu", diagnostics->count, at);
  ScreenPutString(2, left, note.data, faint);
  StrBufFree(&note);
}

// the picker of every diagnostic in the open buffers.

static void
AddPlace(u64 serial, const Diagnostic *diagnostic)
{
  if (placeCount == placeCapacity) {
    placeCapacity = placeCapacity ? placeCapacity * 2 : 64;
    places = MemoryResize(places, placeCapacity, sizeof(Place));
  }

  Range range = diagnostic->range;
  u64 end = range.end.line == range.start.line ? range.end.byte : UINT64_MAX;
  places[placeCount++] = (Place){serial, range.start, end};
}

static void
DrawPlace([[maybe_unused]] void *context, u64 id, i32 column, i32 width, i32 height)
{
  const Place *place = &places[id];
  PreviewDraw(place->serial, place->start.line, place->start.byte, place->end, nullptr, column,
    width, height);
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (!picked) return;

  u64 index = WorkspaceEntryOf(workspace, places[id].serial);

  if (index == workspaceNone) {
    StrBufAppendString(message, "that buffer has been closed");
    return;
  }

  JumpRecord();
  if (index != workspace->current && !WorkspaceSwitch(workspace, index, message)) return;

  View *view = WorkspaceDocument(workspace)->view;
  Position at = BufferClamp(view->buffer, places[id].start);
  MoveSelect(view, at, at);
}

void
IssuesPick(void)
{
  placeCount = 0;
  u64 files = 0;
  DiagnosticCounts total = {};
  StrBuf item = {};
  PickerBegin();

  for (u64 index = 0; index < workspace->count; index++) {
    const WorkspaceEntry *entry = &workspace->entries[index];
    const Diagnostics *diagnostics = entry->document ? LspDiagnostics(entry->document->lsp)
      : nullptr;
    if (!diagnostics || !diagnostics->count) continue;

    files++;
    DiagnosticCounts counts = DiagnosticsCount(diagnostics);
    total.errors += counts.errors;
    total.warnings += counts.warnings;

    for (u32 at = 0; at < diagnostics->count; at++) {
      const Diagnostic *diagnostic = &diagnostics->items[at];
      const char *text = DiagnosticsText(diagnostics, diagnostic->message);
      StrBufClear(&item);
      WorkspaceName(workspace, index, &item);
      StrBufAppendFormat(&item, ":%lu: ", diagnostic->range.start.line + 1);
      StrBufAppend(&item, text, FirstLine(text));
      PickerAdd(item.data, item.length, nullptr, placeCount);
      PickerMark(DiagnosticSeveritySymbol(diagnostic->severity));
      AddPlace(entry->serial, diagnostic);
    }
  }

  StrBufFree(&item);

  if (!placeCount) {
    StrBufAppendString(message, "no diagnostics in the open buffers");
    return;
  }

  StrBuf title = {};
  const char *plural = files == 1 ? "" : "s";
  StrBufAppendFormat(&title, "%lu diagnostics in %lu open buffer%s: E%u W%u", placeCount, files,
    plural, total.errors, total.warnings);
  PickerOpen(title.data, "filter: ", Picked, nullptr);
  PickerShowPreview(DrawPlace, nullptr);
  StrBufFree(&title);
}
