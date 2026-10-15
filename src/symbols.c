#include "symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "highlight.h"
#include "jump.h"
#include "memory.h"
#include "move.h"
#include "picker.h"
#include "preview.h"
#include "scan.h"

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

// the listed symbol named by the word at a position, preferring a definition to a
// prototype, or listed.count when the word names none.
static u64
Named(View *view, Position at)
{
  u64 start;
  u64 end;
  const StrBuf *line = BufferLine(view->buffer, at.line);
  if (!MoveWordAround(line, at.byte, &start, &end)) return listed.count;

  u64 found = listed.count;

  for (u64 index = 0; index < listed.count; index++) {
    const Symbol *symbol = &listed.symbols[index];
    bool same = symbol->length == end - start &&
      memcmp(listed.names.data + symbol->text, line->data + start, end - start) == 0;
    if (!same) continue;

    if (strcmp(symbol->kind, "prototype") != 0) return index;
    if (found == listed.count) found = index;
  }

  return found;
}

void
SymbolsPickFile(void)
{
  FreeList(&listed);
  if (!Collect(&listed)) return;

  View *view = WorkspaceDocument(workspace)->view;
  Position head = CursorSetPrimary(&view->cursors)->head;
  u64 selected = 0;
  u64 width = 0;

  for (u64 index = 0; index < listed.count; index++) {
    const Symbol *symbol = &listed.symbols[index];
    if (PositionCompare(symbol->start, head) <= 0) selected = index;
    if (symbol->length > width) width = symbol->length;
  }

  u64 named = Named(view, head);
  if (named < listed.count) selected = named;

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

// the project index: the symbols of every loaded file whose language has them, kept
// sorted by name. open documents are taken from their buffer and the others from disk.

enum Kind : u8 {
  KindFunction,
  KindPrototype,
  KindType,
  KindStruct,
  KindEnum,
  KindMacro,
  KindHeading,
  KindCount
};

static const char *const kindNames[KindCount] = {
  "function",
  "prototype",
  "type",
  "struct",
  "enum",
  "macro",
  "heading"
};

// one file of the index, by its entry's serial.
typedef struct IndexFile {
  u64 serial;
  const Lang *lang;

  // the symbols came from the document at revision (live), or else from the file at
  // stamp.
  bool live;
  u64 revision;
  FileStamp stamp;

  // its symbols carry the generation they were found in, so finding them again leaves
  // the old ones behind until the next merge.
  u32 generation;
} IndexFile;

typedef struct IndexSymbol {
  u64 serial;
  u32 generation;
  enum Kind kind;
  Range name;

  // its name, then its signature, in strings.
  u64 text;
  u32 nameLength;
  u32 signatureLength;
} IndexSymbol;

// sorted by serial.
static IndexFile *files;
static u64 fileCount;
static u64 fileCapacity;

// sorted by name up to sortedCount; the rest were added since the last merge.
static IndexSymbol *symbols;
static u64 symbolCount;
static u64 symbolCapacity;
static u64 sortedCount;
static StrBuf strings;

// a file was dropped or found again since the last merge, leaving symbols behind.
static bool stale;

// a scan has gone through every loaded file at least once.
static bool complete;

// what to do once the scan that builds or updates the index finishes.
enum Want {
  WantNone,
  WantDefinition,
  WantSignature,
  WantProject
};

static enum Want pending;
static StrBuf pendingNames[2];
static i32 pendingWidth;

// stands for "no name or extension" when asking a path's language.
static const Lang noExtension;

static const char *
TextOf(const StrBuf *text)
{
  return text->data ? text->data : "";
}

static const char *
Plural(u64 count)
{
  return count == 1 ? "" : "s";
}

static const char *
NameOf(const IndexSymbol *symbol)
{
  return strings.data + symbol->text;
}

static const char *
SignatureOf(const IndexSymbol *symbol)
{
  return strings.data + symbol->text + symbol->nameLength;
}

// the position of the file with serial in files, or where it would go.
static u64
FilePlace(u64 serial)
{
  u64 low = 0;
  u64 high = fileCount;

  while (low < high) {
    u64 middle = low + (high - low) / 2;
    if (files[middle].serial < serial) low = middle + 1;
    else high = middle;
  }

  return low;
}

static IndexFile *
FileOf(u64 serial)
{
  u64 place = FilePlace(serial);
  return place < fileCount && files[place].serial == serial ? &files[place] : nullptr;
}

// the file with serial, added with nothing found yet when it is new.
static IndexFile *
AddFile(u64 serial)
{
  u64 place = FilePlace(serial);
  if (place < fileCount && files[place].serial == serial) return &files[place];

  if (fileCount == fileCapacity) {
    fileCapacity = fileCapacity ? fileCapacity * 2 : 256;
    files = MemoryResize(files, fileCapacity, sizeof(IndexFile));
  }

  memmove(files + place + 1, files + place, (fileCount - place) * sizeof(IndexFile));
  fileCount++;
  files[place] = (IndexFile){.serial = serial};
  return &files[place];
}

static void
DropFile(IndexFile *file)
{
  u64 place = (u64)(file - files);
  memmove(files + place, files + place + 1, (fileCount - place - 1) * sizeof(IndexFile));
  fileCount--;
  stale = true;
}

static bool
Current(const IndexSymbol *symbol)
{
  const IndexFile *file = FileOf(symbol->serial);
  return file && file->generation == symbol->generation;
}

static enum Kind
KindOf(const char *name)
{
  for (u32 kind = 0; kind < KindCount; kind++) {
    if (strcmp(kindNames[kind], name) == 0) return (enum Kind)kind;
  }

  return KindCount;
}

// where found symbols go: the file's serial and generation.
typedef struct Adding {
  u64 serial;
  u32 generation;
} Adding;

static void
AddSymbol(void *context, const HighlightSymbol *found)
{
  const Adding *adding = context;
  enum Kind kind = KindOf(found->kind);
  if (kind == KindCount || !found->nameLength) return;

  if (symbolCount == symbolCapacity) {
    symbolCapacity = symbolCapacity ? symbolCapacity * 2 : 1024;
    symbols = MemoryResize(symbols, symbolCapacity, sizeof(IndexSymbol));
  }

  symbols[symbolCount++] = (IndexSymbol){
    .serial = adding->serial,
    .generation = adding->generation,
    .kind = kind,
    .name = found->name,
    .text = strings.length,
    .nameLength = (u32)found->nameLength,
    .signatureLength = (u32)found->signatureLength
  };

  StrBufAppend(&strings, found->nameText, found->nameLength);
  StrBufAppend(&strings, found->signature, found->signatureLength);
}

// starts the file's symbols again, leaving the ones found before behind.
static Adding
Renew(IndexFile *file)
{
  file->generation++;
  stale = true;
  return (Adding){file->serial, file->generation};
}

static i32
CompareSymbols(const void *a, const void *b)
{
  const IndexSymbol *first = a;
  const IndexSymbol *second = b;
  u32 shorter = first->nameLength < second->nameLength ? first->nameLength : second->nameLength;
  i32 order = memcmp(NameOf(first), NameOf(second), shorter);
  if (order) return order;
  u32 lengths[2] = {first->nameLength, second->nameLength};
  if (lengths[0] != lengths[1]) return lengths[0] < lengths[1] ? -1 : 1;
  if (first->serial != second->serial) return first->serial < second->serial ? -1 : 1;
  return PositionCompare(first->name.start, second->name.start);
}

// sorts the symbols added since the last merge into the rest, leaving out those of files
// that were dropped or found again, and packs their texts when most are left over.
static void
Merge(void)
{
  if (sortedCount == symbolCount && !stale) return;

  u64 added = symbolCount - sortedCount;
  qsort(symbols + sortedCount, added, sizeof(IndexSymbol), CompareSymbols);

  IndexSymbol *merged = MemoryAllocate((symbolCount ? symbolCount : 1) * sizeof(IndexSymbol));
  u64 count = 0;
  u64 bytes = 0;
  u64 left = 0;
  u64 right = sortedCount;

  while (left < sortedCount || right < symbolCount) {
    bool takeLeft = right == symbolCount ||
      (left < sortedCount && CompareSymbols(&symbols[left], &symbols[right]) <= 0);
    const IndexSymbol *symbol = takeLeft ? &symbols[left++] : &symbols[right++];
    if (!Current(symbol)) continue;

    merged[count++] = *symbol;
    bytes += symbol->nameLength + symbol->signatureLength;
  }

  free(symbols);
  symbols = merged;
  symbolCount = count;
  symbolCapacity = count ? count : 1;
  sortedCount = count;
  stale = false;

  if (bytes * 2 >= strings.length) return;

  StrBuf packed = {};
  StrBufReserve(&packed, bytes);

  for (u64 index = 0; index < count; index++) {
    IndexSymbol *symbol = &symbols[index];
    u64 text = packed.length;
    StrBufAppend(&packed, NameOf(symbol), symbol->nameLength + symbol->signatureLength);
    symbol->text = text;
  }

  StrBufFree(&strings);
  strings = packed;
}

void
SymbolsClear(void)
{
  free(files);
  free(symbols);
  StrBufFree(&strings);
  files = nullptr;
  symbols = nullptr;
  fileCount = fileCapacity = 0;
  symbolCount = symbolCapacity = sortedCount = 0;
  stale = false;
  complete = false;
  pending = WantNone;
}

// the language of an entry that is not open, from its path, or nullptr when that leaves
// it to a "#!" line (noExtension is returned) or it has no symbols.
static const Lang *
PathLang(const StrBuf *path)
{
  const Lang *lang = LangOfPath(TextOf(path), &noExtension);
  if (lang == &noExtension) return lang;
  return lang && HighlightHasSymbols(lang->name) ? lang : nullptr;
}

// brings the open documents up to date from their buffers, and drops the files of
// entries that are gone.
static void
Refresh(void)
{
  for (u64 place = 0; place < fileCount;) {
    if (WorkspaceEntryOf(workspace, files[place].serial) == workspaceNone) DropFile(&files[place]);
    else place++;
  }

  for (u64 index = 0; index < workspace->count; index++) {
    const WorkspaceEntry *entry = &workspace->entries[index];
    Document *document = entry->document;
    if (!document) continue;

    const Lang *lang = document->lang;
    bool has = lang && HighlightHasSymbols(lang->name);
    IndexFile *file = FileOf(entry->serial);

    // a rename may have given the document a language without symbols.
    if (!has) {
      if (file) DropFile(file);
      continue;
    }

    bool fresh = file && file->live && file->revision == document->buffer.revision &&
      file->lang == lang;
    if (fresh) continue;

    file = AddFile(entry->serial);
    file->live = true;
    file->revision = document->buffer.revision;
    file->lang = lang;
    Adding adding = Renew(file);
    HighlightSymbols(document->highlight, AddSymbol, &adding);
  }

  Merge();
}

static bool
Wants([[maybe_unused]] void *context, u64 index, FileStamp stamp)
{
  IndexFile *file = FileOf(workspace->entries[index].serial);
  if (file && !stamp.exists) DropFile(file);
  if (!stamp.exists) return false;

  return !file || file->live || !FileStampEqual(file->stamp, stamp);
}

static bool
Visit([[maybe_unused]] void *context, const ScanItem *item)
{
  const WorkspaceEntry *entry = &workspace->entries[item->index];
  const Lang *lang = PathLang(&entry->path);

  // a file without an extension may be a script.
  if (lang == &noExtension) {
    const char *newline = memchr(item->text, '\n', item->length);
    u64 length = newline ? (u64)(newline - item->text) : item->length;
    lang = LangOfScript(item->text, length);
    if (lang && !HighlightHasSymbols(lang->name)) lang = nullptr;
  }

  // a file with nothing to find is kept too, so it is not read again until it changes.
  IndexFile *file = AddFile(entry->serial);
  file->live = false;
  file->stamp = item->stamp;
  file->lang = lang;
  Adding adding = Renew(file);
  if (lang) HighlightSymbolsOfText(lang->name, item->text, item->length, AddSymbol, &adding);
  return true;
}

static void
Progress([[maybe_unused]] void *context, u64 done, u64 count, StrBuf *text)
{
  const char *format = "indexing symbols: %lu of %lu files (escape stops)";
  StrBufAppendFormat(text, format, done, count);
}

static void RunPending(bool mayScan);

static void
Finished([[maybe_unused]] void *context, ScanResult result)
{
  StrBufClear(message);

  if (result.stopped) {
    const char *format = "stopped indexing symbols after %lu files (they are kept)";
    StrBufAppendFormat(message, format, result.done);
    pending = WantNone;
    return;
  }

  complete = true;
  RunPending(false);
}

// indexes the loaded files that are not open, all of them or (once the index is complete)
// only those that changed on disk, then does what is pending.
static void
StartScan(enum Want want)
{
  pending = want;
  u64 *queue = MemoryAllocate((workspace->count ? workspace->count : 1) * sizeof(u64));
  u64 count = 0;

  for (u64 index = 0; index < workspace->count; index++) {
    const WorkspaceEntry *entry = &workspace->entries[index];
    bool usable = !entry->document && entry->refusal == WorkspaceRefusalNone && entry->path.length;
    if (usable && PathLang(&entry->path)) queue[count++] = index;
  }

  static const ScanClient client = {
    .wants = Wants,
    .visit = Visit,
    .progress = Progress,
    .finish = Finished
  };

  ScanStart(queue, count, &client, nullptr);
  free(queue);
  StrBufAppendFormat(message, "indexing symbols in %lu files (escape stops)", count);
}

// the range of symbols named name.
static void
NameRange(const char *name, u64 length, u64 *first, u64 *last)
{
  u64 low = 0;
  u64 high = symbolCount;

  while (low < high) {
    u64 middle = low + (high - low) / 2;
    const IndexSymbol *symbol = &symbols[middle];
    u32 shorter = symbol->nameLength < length ? symbol->nameLength : (u32)length;
    i32 order = memcmp(NameOf(symbol), name, shorter);
    bool before = order < 0 || (order == 0 && symbol->nameLength < length);
    if (before) low = middle + 1;
    else high = middle;
  }

  *first = low;
  *last = low;

  while (*last < symbolCount && symbols[*last].nameLength == length &&
    memcmp(NameOf(&symbols[*last]), name, length) == 0) {
    (*last)++;
  }
}

static const Lang *
LangOfSymbol(const IndexSymbol *symbol)
{
  const IndexFile *file = FileOf(symbol->serial);
  return file ? file->lang : nullptr;
}

// the symbols named name that code can refer to (not headings), as positions in symbols,
// those in the current file first, and only those in the current buffer's language when
// there are any.
// the array, which the caller frees, is left in found.
static u64
Candidates(const char *name, u64 length, u64 **foundArray)
{
  u64 first;
  u64 last;
  NameRange(name, length, &first, &last);
  u64 *found = MemoryAllocate((last > first ? last - first : 1) * sizeof(u64));
  *foundArray = found;
  u64 current = workspace->entries[workspace->current].serial;
  const Lang *lang = WorkspaceDocument(workspace)->lang;
  bool sameLang = false;

  for (u64 at = first; at < last; at++) {
    if (symbols[at].kind != KindHeading && LangOfSymbol(&symbols[at]) == lang) sameLang = true;
  }

  u64 count = 0;

  for (u32 pass = 0; pass < 2; pass++) {
    for (u64 at = first; at < last; at++) {
      const IndexSymbol *symbol = &symbols[at];
      if (symbol->kind == KindHeading) continue;
      if (sameLang && LangOfSymbol(symbol) != lang) continue;
      if ((symbol->serial == current) != (pass == 0)) continue;

      // a typedef of a struct gives two symbols at one name.
      bool repeated = count && symbols[found[count - 1]].serial == symbol->serial &&
        PositionCompare(symbols[found[count - 1]].name.start, symbol->name.start) == 0;
      if (!repeated) found[count++] = at;
    }
  }

  return count;
}

// the word at the primary cursor, or false when there is none.
static bool
WordAtCursor(StrBuf *word)
{
  View *view = WorkspaceDocument(workspace)->view;
  Position head = CursorSetPrimary(&view->cursors)->head;
  const StrBuf *line = BufferLine(view->buffer, head.line);
  u64 start;
  u64 end;
  if (!MoveWordAround(line, head.byte, &start, &end)) return false;

  StrBufAppend(word, line->data + start, end - start);
  return true;
}

// the word before the innermost "(" left open before the cursor on its line, or false.
static bool
WordBeforeCall(StrBuf *word)
{
  View *view = WorkspaceDocument(workspace)->view;
  Position head = CursorSetPrimary(&view->cursors)->head;
  const StrBuf *line = BufferLine(view->buffer, head.line);
  u64 depth = 0;

  for (u64 at = head.byte; at > 0; at--) {
    char byte = line->data[at - 1];
    if (byte == ')') depth++;
    if (byte != '(') continue;

    if (depth) {
      depth--;
      continue;
    }

    u64 end = at - 1;
    while (end > 0 && (line->data[end - 1] == ' ' || line->data[end - 1] == '\t')) end--;

    u64 start;
    if (!end || !MoveWordAround(line, end, &start, &end)) return false;

    StrBufAppend(word, line->data + start, end - start);
    return true;
  }

  return false;
}

// goes to a symbol's name, switching to its entry.
static void
GoToSymbol(const IndexSymbol *symbol)
{
  u64 index = WorkspaceEntryOf(workspace, symbol->serial);

  if (index == workspaceNone) {
    StrBufAppendString(message, "that buffer has been closed");
    return;
  }

  JumpRecord();
  if (index != workspace->current && !WorkspaceSwitch(workspace, index, message)) return;

  View *view = WorkspaceDocument(workspace)->view;
  Position at = BufferClamp(view->buffer, symbol->name.start);
  MoveSelect(view, at, at);
}

// the line of a symbol's name, picked out by the preview.
static void
DrawSymbol([[maybe_unused]] void *context, u64 id, i32 column, i32 width, i32 height)
{
  const IndexSymbol *symbol = &symbols[id];
  Range name = symbol->name;
  u64 end = name.end.line == name.start.line ? name.end.byte : UINT64_MAX;
  Position at = name.start;
  PreviewDraw(symbol->serial, at.line, at.byte, end, nullptr, column, width, height);
}

static void
PickedSymbol([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (picked) GoToSymbol(&symbols[id]);
}

// appends "path:line" for a symbol.
static void
AppendPlace(StrBuf *text, const IndexSymbol *symbol)
{
  WorkspaceName(workspace, WorkspaceEntryOf(workspace, symbol->serial), text);
  StrBufAppendFormat(text, ":%lu", symbol->name.start.line + 1);
}

// lists several definitions or prototypes to pick from, with a preview.
static void
PickTarget(const u64 *targets, u64 count, const char *what, const StrBuf *name)
{
  PickerBegin();
  StrBuf item = {};
  StrBuf note = {};

  for (u64 index = 0; index < count; index++) {
    const IndexSymbol *symbol = &symbols[targets[index]];
    StrBufClear(&item);
    AppendPlace(&item, symbol);
    StrBufAppendFormat(&item, "  %s", kindNames[symbol->kind]);
    StrBufClear(&note);
    StrBufAppend(&note, SignatureOf(symbol), symbol->signatureLength);
    PickerAdd(item.data, item.length, TextOf(&note), targets[index]);
  }

  StrBuf title = {};
  StrBufAppendFormat(&title, "%lu %s of %s", count, what, TextOf(name));
  PickerOpen(title.data, "filter: ", PickedSymbol, nullptr);
  PickerShowPreview(DrawSymbol, nullptr);
  StrBufFree(&title);
  StrBufFree(&item);
  StrBufFree(&note);
}

// true when the primary cursor is on the name of a symbol.
static bool
OnName(const IndexSymbol *symbol)
{
  if (symbol->serial != workspace->entries[workspace->current].serial) return false;

  Position head = CursorSetPrimary(&WorkspaceDocument(workspace)->view->cursors)->head;
  Range name = symbol->name;
  return PositionCompare(name.start, head) <= 0 && PositionCompare(head, name.end) <= 0;
}

// goes to the definition of the name in pendingNames[0]: straight there when there is
// one, or through a picker. returns false when there is none.
static bool
Define(void)
{
  const StrBuf *name = &pendingNames[0];
  u64 *found;
  u64 count = Candidates(TextOf(name), name->length, &found);

  if (!count) {
    free(found);
    return false;
  }

  // the prototypes count only when there is no definition, or the cursor is on one.
  u64 *targets = MemoryAllocate(count * sizeof(u64));
  u64 definitions = 0;
  u64 prototypes = 0;
  bool onDefinition = false;

  for (u64 index = 0; index < count; index++) {
    const IndexSymbol *symbol = &symbols[found[index]];
    bool prototype = symbol->kind == KindPrototype;
    if (!prototype && OnName(symbol)) onDefinition = true;
    if (prototype) prototypes++;
    else definitions++;
  }

  bool wantPrototypes = onDefinition ? prototypes > 0 : definitions == 0;
  u64 targetCount = 0;

  for (u64 index = 0; index < count; index++) {
    const IndexSymbol *symbol = &symbols[found[index]];
    bool prototype = symbol->kind == KindPrototype;
    if (prototype != wantPrototypes || OnName(symbol)) continue;

    targets[targetCount++] = found[index];
  }

  if (!targetCount) {
    StrBufAppendFormat(message, "no other definition of %s", TextOf(name));
  } else if (targetCount == 1) {
    GoToSymbol(&symbols[targets[0]]);
  } else {
    PickTarget(targets, targetCount, wantPrototypes ? "prototypes" : "definitions", name);
  }

  free(targets);
  free(found);
  return true;
}

// appends text, cut on a code point boundary with "…" to at most width code points.
static void
AppendCut(StrBuf *text, const char *data, u64 length, u64 width)
{
  u64 points = 0;
  u64 end = 0;

  for (u64 at = 0; at < length; at++) {
    if (((u8)data[at] & 0xC0) == 0x80) continue;

    if (points + 1 == width) end = at;
    points++;
  }

  if (points <= width) {
    StrBufAppend(text, data, length);
    return;
  }

  StrBufAppend(text, data, end);
  StrBufAppendString(text, "…");
}

// shows the signature of the first name in pendingNames that has symbols: one in the
// current file, else a prototype, else a definition. returns false when none has.
static bool
Sign(void)
{
  for (u32 which = 0; which < 2; which++) {
    const StrBuf *name = &pendingNames[which];
    if (!name->length) continue;

    u64 *found;
    u64 count = Candidates(TextOf(name), name->length, &found);

    if (!count) {
      free(found);
      continue;
    }

    // the current file's symbols come first.
    u64 current = workspace->entries[workspace->current].serial;
    u64 best = 0;

    for (u64 index = 0; index < count && symbols[found[0]].serial != current; index++) {
      if (symbols[found[index]].kind != KindPrototype) continue;

      best = index;
      break;
    }

    const IndexSymbol *symbol = &symbols[found[best]];
    free(found);

    // the place is left out when there is little room.
    StrBuf where = {};
    StrBufAppendString(&where, "  ");
    AppendPlace(&where, symbol);
    u64 width = pendingWidth > 1 ? (u64)pendingWidth - 1 : 1;
    u64 room = width > where.length + 20 ? width - where.length : width;
    AppendCut(message, SignatureOf(symbol), symbol->signatureLength, room);
    if (room < width) StrBufAppend(message, where.data, where.length);
    StrBufFree(&where);
    return true;
  }

  return false;
}

// lists every symbol in the index, starting on the one named by the word at the cursor.
static void
PickProject(void)
{
  if (!symbolCount) {
    StrBufAppendString(message, "no symbols in the loaded files");
    return;
  }

  u64 width = 0;

  for (u64 index = 0; index < symbolCount; index++) {
    if (symbols[index].nameLength > width) width = symbols[index].nameLength;
  }

  // names line up, unless one is very long.
  if (width > 40) width = 40;

  u64 first = symbolCount;
  StrBuf word = {};

  if (WordAtCursor(&word)) {
    u64 last;
    NameRange(TextOf(&word), word.length, &first, &last);
    if (first == last) first = symbolCount;
  }

  StrBufFree(&word);
  PickerBegin();
  StrBuf item = {};
  StrBuf note = {};

  for (u64 index = 0; index < symbolCount; index++) {
    const IndexSymbol *symbol = &symbols[index];
    StrBufClear(&item);
    StrBufAppend(&item, NameOf(symbol), symbol->nameLength);
    while (item.length < width + 2) StrBufAppendByte(&item, ' ');
    if (item.length > width + 2) StrBufAppendString(&item, "  ");
    StrBufAppendString(&item, kindNames[symbol->kind]);
    StrBufClear(&note);
    AppendPlace(&note, symbol);
    PickerAdd(item.data, item.length, note.data, index);
  }

  StrBuf title = {};
  u64 count = fileCount;
  StrBufAppendFormat(&title, "%lu symbols in %lu file%s", symbolCount, count, Plural(count));
  PickerOpen(title.data, "symbol: ", PickedSymbol, nullptr);
  PickerShowPreview(DrawSymbol, nullptr);
  if (first < symbolCount) PickerSelect(first);
  StrBufFree(&title);
  StrBufFree(&item);
  StrBufFree(&note);
}

// does what is pending on an index that is up to date with the open documents. a name
// that is not found first indexes again the files that changed on disk, when mayScan.
static void
RunPending(bool mayScan)
{
  enum Want want = pending;
  pending = WantNone;
  Refresh();

  bool done = true;
  if (want == WantDefinition) done = Define();
  if (want == WantSignature) done = Sign();
  if (want == WantProject) PickProject();
  if (done) return;

  if (mayScan) {
    StartScan(want);
    return;
  }

  const char *name = TextOf(&pendingNames[0]);
  if (want == WantDefinition) StrBufAppendFormat(message, "no definition of %s found", name);
  else StrBufAppendFormat(message, "no signature found for %s", name);

  if (want == WantSignature && pendingNames[1].length) {
    StrBufAppendFormat(message, " or %s", TextOf(&pendingNames[1]));
  }
}

// builds the index first when no scan has gone through every file yet.
static void
Request(enum Want want)
{
  if (complete) {
    pending = want;
    RunPending(true);
    return;
  }

  StartScan(want);
}

void
SymbolsGoToDefinition(void)
{
  StrBufClear(&pendingNames[0]);
  StrBufClear(&pendingNames[1]);

  if (!WordAtCursor(&pendingNames[0])) {
    StrBufAppendString(message, "no name at the cursor");
    return;
  }

  Request(WantDefinition);
}

void
SymbolsShowSignature(i32 width)
{
  StrBufClear(&pendingNames[0]);
  StrBufClear(&pendingNames[1]);
  WordAtCursor(&pendingNames[0]);
  WordBeforeCall(&pendingNames[1]);

  // the name at the cursor is tried first, so a call's name is only kept when it differs.
  bool same = pendingNames[0].length == pendingNames[1].length &&
    memcmp(TextOf(&pendingNames[0]), TextOf(&pendingNames[1]), pendingNames[0].length) == 0;
  if (same) StrBufClear(&pendingNames[1]);

  if (!pendingNames[0].length && pendingNames[1].length) {
    StrBuf swap = pendingNames[0];
    pendingNames[0] = pendingNames[1];
    pendingNames[1] = swap;
  }

  if (!pendingNames[0].length) {
    StrBufAppendString(message, "no name at the cursor or before an open \"(\"");
    return;
  }

  pendingWidth = width;
  Request(WantSignature);
}

void
SymbolsPickProject(void)
{
  Request(WantProject);
}
