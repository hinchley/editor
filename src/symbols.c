#include "symbols.h"

#include <stdio.h>
#include <stdlib.h>

#include "highlight.h"
#include "jump.h"
#include "memory.h"
#include "move.h"
#include "picker.h"

// a symbol of the current buffer, with its name and kind in names.
typedef struct Symbol {
  Position start;
  Position name;
  u64 text;
  u64 length;
  const char *kind;
} Symbol;

typedef struct SymbolList {
  Symbol *symbols;
  u64 count;
  u64 capacity;
  StrBuf names;
} SymbolList;

static Workspace *workspace;
static StrBuf *message;

// the symbols the open picker lists.
static SymbolList listed;

void
SymbolsInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

static void
FreeList(SymbolList *list)
{
  free(list->symbols);
  StrBufFree(&list->names);
  *list = (SymbolList){};
}

static void
Add(void *context, const HighlightSymbol *symbol)
{
  SymbolList *list = context;

  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 64;
    list->symbols = MemoryResize(list->symbols, list->capacity, sizeof(Symbol));
  }

  list->symbols[list->count++] = (Symbol){
    .start = symbol->start,
    .name = symbol->name.start,
    .text = list->names.length,
    .length = symbol->nameLength,
    .kind = symbol->kind
  };

  StrBufAppend(&list->names, symbol->nameText, symbol->nameLength);
}

// fills list with the current buffer's symbols, or says why there are none.
static bool
Collect(SymbolList *list)
{
  Document *document = WorkspaceDocument(workspace);
  const Lang *lang = document->lang;

  if (!lang || !HighlightHasSymbols(lang->name)) {
    if (lang) StrBufAppendFormat(message, "%s has no symbols", lang->name);
    else StrBufAppendString(message, "no symbols for this file type");
    return false;
  }

  bool parsed = HighlightSymbols(document->highlight, Add, list);

  if (!parsed) {
    StrBufAppendString(message, "no symbols: highlighting is off for this buffer");
    return false;
  }

  if (!list->count) {
    StrBufAppendString(message, "no symbols in this buffer");
    return false;
  }

  return true;
}

static void
Go(Position position)
{
  JumpRecord();
  MoveSelect(WorkspaceDocument(workspace)->view, position, position);
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (picked) Go(listed.symbols[id].name);
  FreeList(&listed);
}

void
SymbolsPickFile(void)
{
  FreeList(&listed);
  if (!Collect(&listed)) return;

  Position head = CursorSetPrimary(&WorkspaceDocument(workspace)->view->cursors)->head;
  u64 selected = 0;
  u64 width = 0;

  for (u64 index = 0; index < listed.count; index++) {
    const Symbol *symbol = &listed.symbols[index];
    if (PositionCompare(symbol->start, head) <= 0) selected = index;
    if (symbol->length > width) width = symbol->length;
  }

  // names line up, unless one is very long.
  if (width > 40) width = 40;
  PickerBegin();
  StrBuf item = {};
  char note[24];

  for (u64 index = 0; index < listed.count; index++) {
    const Symbol *symbol = &listed.symbols[index];
    StrBufClear(&item);
    StrBufAppend(&item, listed.names.data + symbol->text, symbol->length);
    while (item.length < width + 2) StrBufAppendByte(&item, ' ');
    if (item.length > width + 2) StrBufAppendString(&item, "  ");
    StrBufAppendString(&item, symbol->kind);
    snprintf(note, sizeof note, "%lu", symbol->name.line + 1);
    PickerAdd(item.data, item.length, note, index);
  }

  StrBufFree(&item);
  StrBuf title = {};
  StrBuf name = {};
  WorkspaceName(workspace, workspace->current, &name);
  StrBufAppendFormat(&title, "%lu symbols in %s", listed.count, name.data);
  PickerOpen(title.data, "symbol: ", Picked, nullptr);
  PickerSelect(selected);
  StrBufFree(&title);
  StrBufFree(&name);
}

void
SymbolsStep(bool backward)
{
  SymbolList list = {};

  if (!Collect(&list)) {
    FreeList(&list);
    return;
  }

  Position head = CursorSetPrimary(&WorkspaceDocument(workspace)->view->cursors)->head;
  u64 found = list.count;

  for (u64 index = 0; index < list.count; index++) {
    Position start = list.symbols[index].start;

    // several definitions starting on one line (a typedef and its struct) count as one,
    // the first of them.
    if (backward && PositionCompare(start, head) < 0) {
      bool sameLine = found < list.count && list.symbols[found].start.line == start.line;
      if (!sameLine) found = index;
    }

    if (!backward && start.line > head.line) {
      found = index;
      break;
    }
  }

  if (found < list.count) Go(list.symbols[found].start);
  else StrBufAppendString(message, backward ? "no symbol above" : "no symbol below");
  FreeList(&list);
}
