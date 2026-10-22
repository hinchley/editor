#include "issues.h"

#include <stdlib.h>
#include <string.h>

#include "diagnostics.h"
#include "jump.h"
#include "layout.h"
#include "lsp.h"
#include "move.h"
#include "places.h"
#include "screen.h"
#include "settings.h"
#include "term.h"
#include "theme.h"

static Workspace *workspace;
static StrBuf *message;
static bool panelOpen;

// what the panel shows of the symbol at the cursor: where the cursor was when it last
// moved (or the text changed), when that was, whether the server was asked, and its
// answer.
static struct {
  u64 serial;
  u64 revision;
  Position head;
  i64 since;
  bool asked;
  LspRequest request;
  StrBuf text;
} hover;

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

// forgets the hover, so it is asked for again once the cursor rests.
static void
ForgetHover(void)
{
  LspCancel(hover.request);
  hover.request = 0;
  hover.asked = false;
  hover.since = TermNow();
  StrBufClear(&hover.text);
}

// keeps the server's answer as plain text: without markdown's fences, rules, heading marks
// and code quotes.
static void
HoverDone([[maybe_unused]] void *context, const char *text, u64 length, bool answered)
{
  hover.request = 0;
  if (!answered) return;

  const char *end = text + length;

  for (const char *at = text, *next; at < end; at = next) {
    const char *newline = memchr(at, '\n', (u64)(end - at));
    next = newline ? newline + 1 : end;
    const char *stop = newline ? newline : end;
    bool fence = stop - at >= 3 && memcmp(at, "```", 3) == 0;
    bool rule = stop - at == 3 && memcmp(at, "---", 3) == 0;
    if (fence) continue;

    const char *start = at;
    while (!rule && at < stop && *at == '#') at++;
    while (at > start && at < stop && *at == ' ') at++;

    for (const char *byte = at; byte < stop && !rule; byte++) {
      if (*byte != '`') StrBufAppendByte(&hover.text, *byte);
    }

    // a blank line is kept once.
    bool blank = hover.text.length >= 2 && hover.text.data[hover.text.length - 1] == '\n' &&
      hover.text.data[hover.text.length - 2] == '\n';
    if (!blank) StrBufAppendByte(&hover.text, '\n');
  }

  while (hover.text.length && hover.text.data[hover.text.length - 1] == '\n') {
    StrBufTruncate(&hover.text, hover.text.length - 1);
  }
}

void
IssuesTick(void)
{
  if (!panelOpen) {
    if (hover.request) ForgetHover();
    return;
  }

  Document *document = WorkspaceDocument(workspace);
  Position head = CursorSetPrimary(&document->view->cursors)->head;
  u64 serial = workspace->entries[workspace->current].serial;
  bool moved = serial != hover.serial || document->buffer.revision != hover.revision ||
    PositionCompare(head, hover.head) != 0;

  if (moved) {
    hover.serial = serial;
    hover.revision = document->buffer.revision;
    hover.head = head;
    ForgetHover();
  }

  if (hover.asked || TermNow() - hover.since < hoverDelay) return;

  hover.asked = true;
  hover.request = LspHover(document->lsp, head, true, HoverDone, nullptr);
}

i32
IssuesTimeout(void)
{
  if (!panelOpen || hover.asked) return -1;

  i64 left = hover.since + hoverDelay - TermNow();
  return left > 0 ? (i32)left : 0;
}

void
IssuesTogglePanel(i32 width)
{
  panelOpen = !panelOpen;
  ForgetHover();

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
DrawDiagnostic(
  const Diagnostics *diagnostics,
  const Diagnostic *diagnostic,
  i32 row,
  i32 bottom,
  i32 column,
  i32 width
)
{
  Style group = ThemeStyle(DiagnosticSeverityGroup(diagnostic->severity));
  Style faint = ThemeStyle(ThemeGutter);
  const char *code = DiagnosticsText(diagnostics, diagnostic->code);
  const char *source = DiagnosticsText(diagnostics, diagnostic->source);

  StrBuf heading = {};
  StrBufAppendFormat(
    &heading,
    "%s %s",
    DiagnosticSeveritySymbol(diagnostic->severity),
    DiagnosticSeverityName(diagnostic->severity)
  );
  if (*code) StrBufAppendFormat(&heading, " [%s]", code);
  if (*source) StrBufAppendFormat(&heading, " %s", source);
  StrBufAppendFormat(&heading, "  line %lu", diagnostic->range.start.line + 1);
  StrBuf shown = {};
  LayoutShorten(heading.data, heading.length, width, &shown);
  ScreenPutText(row++, column, shown.data, shown.length, group);
  StrBufFree(&shown);
  StrBufFree(&heading);

  row = DrawWrapped(
    DiagnosticsText(diagnostics, diagnostic->message),
    row,
    bottom,
    column,
    width,
    styleDefault
  );
  const char *notes = DiagnosticsText(diagnostics, diagnostic->notes);
  if (*notes) row = DrawWrapped(notes, row, bottom, column, width, faint);
  return row;
}

// says there is no diagnostic on the cursor's line, and where the next one is, from row;
// returns the row after it.
static i32
DrawNone(const Diagnostics *diagnostics, u64 line, i32 row, i32 column)
{
  Style faint = ThemeStyle(ThemeGutter);
  ScreenPutString(row++, column, "no diagnostics on this line", faint);
  if (!diagnostics->count) return row;

  u32 next;
  bool wrapped;
  DiagnosticsStep(diagnostics, (Position){line, UINT64_MAX}, false, &next, &wrapped);
  StrBuf note = {};
  u64 at = diagnostics->items[next].range.start.line + 1;
  StrBufAppendFormat(&note, "%u in this buffer, the next at line %lu", diagnostics->count, at);
  ScreenPutString(row++, column, note.data, faint);
  StrBufFree(&note);
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

  // every diagnostic reaching the line, as its underline shows, then what the symbol at
  // the cursor is.
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

  if (!any) row = DrawNone(diagnostics, line, row, left);
  if (hover.text.length && row + 1 < height) {
    DrawWrapped(hover.text.data, row + 1, height, left, textWidth, styleDefault);
  }
}

void
IssuesPick(void)
{
  u64 count = 0;
  u64 files = 0;
  DiagnosticCounts total = {};
  StrBuf item = {};
  PlacesBegin();

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
      const char *mark = DiagnosticSeveritySymbol(diagnostic->severity);
      PlacesAdd(entry->serial, diagnostic->range, item.data, item.length, nullptr, mark);
      count++;
    }
  }

  StrBufFree(&item);

  if (!count) {
    StrBufAppendString(message, "no diagnostics in the open buffers");
    return;
  }

  StrBuf title = {};
  const char *plural = files == 1 ? "" : "s";
  StrBufAppendFormat(
    &title,
    "%lu diagnostics in %lu open buffer%s: E%u W%u",
    count,
    files,
    plural,
    total.errors,
    total.warnings
  );
  PlacesOpen(title.data, 0);
  StrBufFree(&title);
}
