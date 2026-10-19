#include "lookup.h"

#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "jump.h"
#include "layout.h"
#include "lsp.h"
#include "memory.h"
#include "move.h"
#include "places.h"
#include "symbols.h"

// what a reference's note keeps of its line.
static constexpr u64 maxNoteLength = 200;

static Workspace *workspace;
static StrBuf *message;

// the question waiting for an answer, and where the cursor was when it was asked, so an
// answer that comes after the cursor moved or the text changed is dropped.
static struct {
  LspRequest request;
  u64 serial;
  u64 revision;
  Position head;
  i32 width;
} asked;

void
LookupInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

static Document *
Current(void)
{
  return WorkspaceDocument(workspace);
}

static Position
Head(void)
{
  return CursorSetPrimary(&Current()->view->cursors)->head;
}

// forgets the question before and remembers where this one was asked.
static void
Asking(LspRequest request)
{
  LspCancel(asked.request);
  asked.request = request;
  asked.serial = workspace->entries[workspace->current].serial;
  asked.revision = Current()->buffer.revision;
  asked.head = Head();
}

// true when the answer to the question still applies, which ends the wait for it.
static bool
Answered(void)
{
  asked.request = 0;
  bool same = workspace->entries[workspace->current].serial == asked.serial;
  return same && Current()->buffer.revision == asked.revision &&
    PositionCompare(Head(), asked.head) == 0;
}

// the serial of the entry of a server's path, loading it when it is not loaded, or
// workspaceNone.
static u64
SerialOf(const char *path)
{
  u64 index = WorkspaceLoadPath(workspace, path, message);
  return index == workspaceNone ? workspaceNone : workspace->entries[index].serial;
}

static void
GoToLocation(const LspLocation *location)
{
  u64 serial = SerialOf(location->path);
  if (serial != workspaceNone) PlacesGo(serial, location->range.start);
}

// appends "file:line" for a place.
static void
AppendPlace(StrBuf *text, u64 serial, Position position)
{
  WorkspaceName(workspace, WorkspaceEntryOf(workspace, serial), text);
  StrBufAppendFormat(text, ":%lu", position.line + 1);
}

static void
DefinitionDone([[maybe_unused]] void *context, const LspLocation *locations, u32 count,
  bool answered)
{
  if (!Answered()) return;

  // the symbol index may know what the server does not.
  if (!answered || !count) {
    SymbolsGoToDefinition();
    return;
  }

  if (count == 1) {
    GoToLocation(&locations[0]);
    return;
  }

  PlacesBegin();
  StrBuf item = {};
  u32 added = 0;

  for (u32 index = 0; index < count; index++) {
    u64 serial = SerialOf(locations[index].path);
    if (serial == workspaceNone) continue;

    StrBufClear(&item);
    AppendPlace(&item, serial, locations[index].range.start);
    PlacesAdd(serial, locations[index].range, item.data, item.length, nullptr, nullptr);
    added++;
  }

  StrBuf title = {};
  StrBufAppendFormat(&title, "%u definitions", added);
  PlacesOpen(title.data, 0);
  StrBufFree(&title);
  StrBufFree(&item);
}

void
LookupDefinition(void)
{
  LspRequest request = LspDefinition(Current()->lsp, Head(), DefinitionDone, nullptr);
  Asking(request);
  if (!request) SymbolsGoToDefinition();
}

// appends the declaration a hover shows, on one line: its first fenced block (clangd's
// and most servers' markdown) without comment lines, or else its first line of text.
static void
AppendDeclaration(StrBuf *line, const char *text, u64 length)
{
  const char *fence = memmem(text, length, "```", 3);
  bool fenced = fence != nullptr;
  const char *at = text;
  const char *end = text + length;

  if (fenced) {
    const char *open = memchr(fence, '\n', (u64)(end - fence));
    at = open ? open + 1 : end;
    const char *close = memmem(at, (u64)(end - at), "```", 3);
    if (close) end = close;
  }

  for (const char *next; at < end; at = next) {
    const char *newline = memchr(at, '\n', (u64)(end - at));
    next = newline ? newline + 1 : end;
    const char *stop = newline ? newline : end;

    // blanks are collapsed, and markdown's emphasis around a heading left out.
    while (at < stop && (*at == ' ' || *at == '\t' || (!fenced && *at == '#'))) at++;
    if (at == stop || (fenced && stop - at >= 2 && at[0] == '/' && at[1] == '/')) continue;

    for (const char *byte = at; byte < stop; byte++) {
      bool blank = *byte == ' ' || *byte == '\t';
      bool after = line->length && line->data[line->length - 1] == ' ';
      if ((blank && after) || (!fenced && *byte == '`')) continue;
      StrBufAppendByte(line, blank ? ' ' : *byte);
    }

    if (!fenced) break;
    if (line->length && line->data[line->length - 1] != ' ') StrBufAppendByte(line, ' ');
  }

  while (line->length && line->data[line->length - 1] == ' ') {
    StrBufTruncate(line, line->length - 1);
  }
}

static void
SignatureDone([[maybe_unused]] void *context, const char *text, u64 length, bool answered)
{
  if (!Answered()) return;

  StrBuf declaration = {};
  if (answered) AppendDeclaration(&declaration, text, length);

  if (declaration.length) {
    LayoutCut(declaration.data, declaration.length, asked.width - 1, message);
  } else {
    SymbolsShowSignature(asked.width);
  }

  StrBufFree(&declaration);
}

void
LookupSignature(i32 width)
{
  LspRequest request = LspHover(Current()->lsp, Head(), SignatureDone, nullptr);
  Asking(request);
  asked.width = width;
  if (!request) SymbolsShowSignature(width);
}

// a reference found: its entry, where it is, and the path the server gave.
typedef struct Reference {
  u64 serial;
  Range range;
  const char *path;
} Reference;

static u64 currentSerial;

// the current file's references first, then by path and place.
static int
CompareReferences(const void *first, const void *second)
{
  const Reference *a = first;
  const Reference *b = second;
  bool aCurrent = a->serial == currentSerial;
  bool bCurrent = b->serial == currentSerial;
  if (aCurrent != bCurrent) return aCurrent ? -1 : 1;

  i32 order = strcmp(a->path, b->path);
  return order ? order : PositionCompare(a->range.start, b->range.start);
}

// appends a reference's line without the blanks before it, from its buffer when it is
// open, or else from its file, read once for every reference in it.
static void
AppendLine(StrBuf *note, const Reference *reference, StrBuf *read, u64 *readSerial)
{
  u64 index = WorkspaceEntryOf(workspace, reference->serial);
  const Document *document = workspace->entries[index].document;
  u64 line = reference->range.start.line;
  const char *text = nullptr;
  u64 length = 0;

  if (document && line < BufferLineCount(&document->buffer)) {
    text = BufferLine(&document->buffer, line)->data;
    length = BufferLine(&document->buffer, line)->length;
  } else if (!document) {
    if (*readSerial != reference->serial) {
      *readSerial = reference->serial;
      StrBufClear(read);
      FileStamp stamp;
      FileRead(WorkspacePath(workspace, index)->data, read, &stamp, nullptr);
    }

    // the line's start, counting line breaks.
    const char *at = read->data;
    const char *end = read->data + read->length;

    for (u64 count = 0; at && at < end && count < line; count++) {
      at = memchr(at, '\n', (u64)(end - at));
      if (at) at++;
    }

    if (at && at < end) {
      const char *newline = memchr(at, '\n', (u64)(end - at));
      text = at;
      length = (u64)((newline ? newline : end) - at);
    }
  }

  while (length && (*text == ' ' || *text == '\t')) {
    text++;
    length--;
  }

  StrBufAppend(note, text, length < maxNoteLength ? length : maxNoteLength);
}

static void
ReferencesDone([[maybe_unused]] void *context, const LspLocation *locations, u32 count,
  bool answered)
{
  if (!Answered() || !answered) return;

  if (!count) {
    StrBufAppendString(message, "no references found");
    return;
  }

  // references are loaded as buffers, so the preview and the picker can show them.
  Reference *references = MemoryAllocate(count * sizeof(Reference));
  u32 found = 0;
  u64 files = 0;
  const char *lastPath = nullptr;
  u64 lastSerial = workspaceNone;

  for (u32 index = 0; index < count; index++) {
    const char *path = locations[index].path;
    bool same = lastPath && strcmp(path, lastPath) == 0;
    u64 serial = same ? lastSerial : SerialOf(path);
    lastPath = path;
    lastSerial = serial;
    if (serial == workspaceNone) continue;

    references[found++] = (Reference){serial, locations[index].range, path};
  }

  currentSerial = asked.serial;
  qsort(references, found, sizeof(Reference), CompareReferences);

  // the picker starts on the reference at the cursor.
  u64 selected = 0;
  StrBuf item = {};
  StrBuf note = {};
  StrBuf read = {};
  u64 readSerial = workspaceNone;
  PlacesBegin();

  for (u32 index = 0; index < found; index++) {
    const Reference *reference = &references[index];
    if (!index || reference->serial != references[index - 1].serial) files++;

    bool atCursor = reference->serial == asked.serial &&
      PositionCompare(reference->range.start, asked.head) <= 0 &&
      PositionCompare(asked.head, reference->range.end) <= 0;
    if (atCursor) selected = index;

    StrBufClear(&item);
    StrBufClear(&note);
    AppendPlace(&item, reference->serial, reference->range.start);
    AppendLine(&note, reference, &read, &readSerial);
    PlacesAdd(reference->serial, reference->range, item.data, item.length, note.data, nullptr);
  }

  // the name the references are to, as the cursor's line has it.
  StrBuf title = {};
  const StrBuf *line = BufferLine(&Current()->buffer, asked.head.line);
  u64 start;
  u64 end;
  StrBufAppendFormat(&title, "%u reference%s", found, found == 1 ? "" : "s");

  if (MoveWordAround(line, asked.head.byte, &start, &end)) {
    StrBufAppendFormat(&title, " to %.*s", (int)(end - start), line->data + start);
  }

  StrBufAppendFormat(&title, " in %lu file%s", files, files == 1 ? "" : "s");
  PlacesOpen(title.data, selected);

  StrBufFree(&title);
  StrBufFree(&item);
  StrBufFree(&note);
  StrBufFree(&read);
  free(references);
}

void
LookupReferences(void)
{
  LspRequest request = LspReferences(Current()->lsp, Head(), ReferencesDone, nullptr);
  Asking(request);
  if (!request) StrBufAppendString(message, "finding references needs a language server");
}

static void
OtherFileDone([[maybe_unused]] void *context, const LspLocation *locations, u32 count,
  bool answered)
{
  if (!Answered() || !answered) return;

  if (!count) {
    StrBufAppendString(message, "no other file found");
    return;
  }

  // the other file shows where it was last seen, as switching buffers does.
  u64 serial = SerialOf(locations[0].path);
  u64 index = serial == workspaceNone ? workspaceNone : WorkspaceEntryOf(workspace, serial);
  if (index == workspaceNone) return;

  JumpRecord();
  WorkspaceSwitch(workspace, index, message);
}

void
LookupOtherFile(void)
{
  LspRequest request = LspOtherFile(Current()->lsp, OtherFileDone, nullptr);
  Asking(request);
  if (!request) StrBufAppendString(message, "switching between header and source needs clangd");
}
