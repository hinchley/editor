#include "project.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "filter.h"
#include "layout.h"
#include "memory.h"
#include "move.h"
#include "picker.h"
#include "prompt.h"
#include "screen.h"
#include "search.h"
#include "settings.h"
#include "theme.h"

// how long a search runs before the window is drawn again, in milliseconds.
static constexpr i64 sliceTime = 20;

// how much of a matching line the list keeps, in bytes.
static constexpr u64 maxShownLength = 200;

static constexpr u64 noHit = UINT64_MAX;

static constexpr char invalidSymbol[] = "�";

// a matching line, in the entry with the serial entry.
typedef struct Hit {
  u64 entry;
  u64 line;

  // its first match, in bytes.
  u64 start;
  u64 end;

  // the line as the list shows it, in shownTexts.
  u64 shown;
  u64 shownLength;
} Hit;

// a file read from disk for the preview (of the entry with the serial entry), with where
// each line starts.
typedef struct Lines {
  u64 entry;
  bool readable;
  char *text;
  u64 length;
  u64 *starts;
  u64 count;
  u64 capacity;
} Lines;

static Workspace *workspace;
static StrBuf *message;

// the pattern waiting for its scope, and the pattern and scope of the last search.
static Pattern asked;
static Pattern pattern;
static StrBuf scope;

static Hit *hits;
static u64 hitCount;
static u64 hitCapacity;
static StrBuf shownTexts;

// how many files the last search took in, how many of them matched, and how many it
// skipped because they were binary, too large or unreadable.
static u64 fileCount;
static u64 matchedFiles;
static u64 skipped;

// the search in progress: the entries in scope and the next one to search.
static bool busy;
static u64 *queue;
static u64 queueCount;
static u64 queueNext;

// the search was stopped with escape, or by reaching maxProjectHits.
static bool stopped;
static bool capped;

// the hit opened last from the list, or noHit.
static u64 lastOpened = noHit;

// a file read from disk while searching.
static char *readText;
static u64 readCapacity;

static Lines preview = {.entry = noHit};

void
ProjectInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

static const char *
TextOf(const StrBuf *text)
{
  return text->data ? text->data : "";
}

static i64
Milliseconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1'000'000;
}

static const char *
Plural(u64 count)
{
  return count == 1 ? "" : "s";
}

// reads the regular file at path into readText, returning false for a file that cannot be
// read, is too large or is binary.
static bool
ReadFile(const char *path, u64 *length)
{
  i32 descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) return false;

  struct stat status;
  bool usable = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
    (u64)status.st_size <= maxFileSize;
  u64 size = usable ? (u64)status.st_size : 0;

  // the text is always allocated, even for an empty file.
  if (usable && (size > readCapacity || !readText)) {
    free(readText);
    readCapacity = size ? size : 1;
    readText = MemoryAllocate(readCapacity);
  }

  // the file may have shrunk since it was measured, so a short read is not an error.
  u64 total = 0;

  while (usable && total < size) {
    i64 count = read(descriptor, readText + total, size - total);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) usable = count == 0;
    if (count <= 0) break;

    total += (u64)count;
  }

  close(descriptor);
  u64 probe = total < binaryProbeSize ? total : binaryProbeSize;
  if (!usable || (probe && memchr(readText, '\0', probe))) return false;

  *length = total;
  return true;
}

static void
AddHit(u64 entry, u64 line, const char *text, u64 length, PatternMatch match)
{
  if (hitCount == hitCapacity) {
    hitCapacity = hitCapacity ? hitCapacity * 2 : 256;
    hits = MemoryResize(hits, hitCapacity, sizeof(Hit));
  }

  if (!hitCount || hits[hitCount - 1].entry != entry) matchedFiles++;

  // the list shows the line without its indent, cut short on a code point boundary.
  u64 start = 0;
  while (start < length && (text[start] == ' ' || text[start] == '\t')) start++;
  u64 end = length - start > maxShownLength ? start + maxShownLength : length;
  while (end < length && end > start && ((u8)text[end] & 0xC0) == 0x80) end--;

  Hit *hit = &hits[hitCount++];
  *hit = (Hit){entry, line, match.start, match.end, shownTexts.length, end - start};

  // a tab would show as a control character.
  for (u64 index = start; index < end; index++) {
    StrBufAppendByte(&shownTexts, text[index] == '\t' ? ' ' : text[index]);
  }

  if (hitCount == maxProjectHits) capped = true;
}

static void
SearchLine(u64 entry, u64 line, const char *text, u64 length)
{
  PatternMatch match;
  if (PatternFind(&pattern, text, length, 0, &match)) AddHit(entry, line, text, length, match);
}

static void
SearchBuffer(u64 entry, const Buffer *buffer)
{
  u64 count = BufferLineCount(buffer);

  for (u64 line = 0; line < count && !capped; line++) {
    const StrBuf *text = BufferLine(buffer, line);
    SearchLine(entry, line, TextOf(text), text->length);
  }
}

// searches text read from disk, where a "\r" before a line break is part of the break.
static void
SearchText(u64 entry, const char *text, u64 length)
{
  // a line that matches case exactly holds the literal text, so a file without it has
  // no match.
  bool exact = pattern.options.caseSensitive && pattern.text.length;
  if (exact && !memmem(text, length, pattern.text.data, pattern.text.length)) return;

  u64 at = 0;
  u64 line = 0;

  while (!capped) {
    const char *newline = memchr(text + at, '\n', length - at);
    u64 end = newline ? (u64)(newline - text) : length;
    u64 textEnd = newline && end > at && text[end - 1] == '\r' ? end - 1 : end;
    SearchLine(entry, line, text + at, textEnd - at);
    if (!newline) break;

    at = end + 1;
    line++;
  }
}

// no entry is removed while a search runs, so the queue holds indexes.
static void
SearchEntry(u64 index)
{
  const WorkspaceEntry *entry = &workspace->entries[index];

  if (entry->document) {
    SearchBuffer(entry->serial, &entry->document->buffer);
    return;
  }

  u64 length;
  if (ReadFile(TextOf(&entry->path), &length)) SearchText(entry->serial, readText, length);
  else skipped++;
}

static void
DescribeResult(StrBuf *text)
{
  StrBufAppendFormat(
    text,
    "%s: %lu line%s in %lu of %lu file%s",
    TextOf(&pattern.source),
    hitCount,
    Plural(hitCount),
    matchedFiles,
    fileCount,
    Plural(fileCount)
  );

  if (capped) StrBufAppendFormat(text, ", stopped at %lu lines", maxProjectHits);
  if (stopped) StrBufAppendFormat(text, ", stopped after %lu files", queueNext);
  if (skipped) StrBufAppendFormat(text, ", %lu skipped", skipped);
}

static void ShowList(u64 position);

static void
Finish(void)
{
  busy = false;
  free(queue);
  queue = nullptr;
  StrBufClear(message);

  if (!hitCount && !stopped) {
    const char *source = TextOf(&pattern.source);
    StrBufAppendFormat(message, "no match for %s in %lu files", source, fileCount);
    if (skipped) StrBufAppendFormat(message, " (%lu skipped)", skipped);
    return;
  }

  if (!hitCount) {
    const char *source = TextOf(&pattern.source);
    const char *format = "stopped: no match for %s in the first %lu of %lu files";
    StrBufAppendFormat(message, format, source, queueNext, fileCount);
    return;
  }

  // a stopped search keeps what it found for alt+f rather than covering the window.
  if (stopped) {
    DescribeResult(message);
    if (hitCount) StrBufAppendString(message, " (alt+f lists them)");
    return;
  }

  ShowList(0);
}

bool
ProjectBusy(void)
{
  return busy;
}

void
ProjectContinue(void)
{
  i64 deadline = Milliseconds() + sliceTime;

  while (queueNext < queueCount && !capped) {
    SearchEntry(queue[queueNext++]);
    if (Milliseconds() > deadline) break;
  }

  if (queueNext == queueCount || capped) {
    Finish();
    return;
  }

  StrBufClear(message);
  StrBufAppendFormat(
    message,
    "searching %lu of %lu files: %lu line%s found (escape stops)",
    queueNext,
    queueCount,
    hitCount,
    Plural(hitCount)
  );
}

void
ProjectKey(Key key)
{
  if (key.code != KeyEscape || key.modifiers) return;

  stopped = true;
  Finish();
}

// hits remember entries by serial, so they never lead to the wrong file; this only frees
// them.
void
ProjectClear(void)
{
  busy = false;
  free(queue);
  queue = nullptr;
  hitCount = 0;
  StrBufClear(&shownTexts);
  lastOpened = noHit;
  preview.entry = noHit;
}

// splits a scope into the files it picks and the files it leaves out.
static void
ParseScope(const char *text, u64 length, Filter *include, Filter *exclude)
{
  u64 at = 0;

  while (at < length) {
    while (at < length && (text[at] == ' ' || text[at] == '\t')) at++;
    u64 start = at;
    while (at < length && text[at] != ' ' && text[at] != '\t') at++;
    if (at == start) break;

    if (text[start] == '!') FilterAdd(exclude, text + start + 1, at - start - 1);
    else FilterAdd(include, text + start, at - start);
  }
}

// true when the entry's path is in the scope.
// an unnamed buffer has no path for a pattern to pick, so only a scope that picks
// everything takes it.
static bool
InScope(const Filter *include, const Filter *exclude, const StrBuf *path)
{
  const char *text = TextOf(path);
  u64 length = path->length;
  if (!length) return !include->count;

  // "./src/a.c" is matched as "src/a.c".
  while (length > 2 && text[0] == '.' && text[1] == '/') {
    text += 2;
    length -= 2;
  }

  if (include->count && !FilterMatches(include, text, length, false)) return false;
  return !FilterMatches(exclude, text, length, false);
}

static void
ScopeAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer) return;

  StrBufClear(&scope);
  StrBufAppend(&scope, answer, length);
  ProjectClear();
  PatternSet(&pattern, TextOf(&asked.source), asked.source.length, asked.options);

  Filter include = {};
  Filter exclude = {};
  ParseScope(answer, length, &include, &exclude);
  queue = MemoryAllocate((workspace->count ? workspace->count : 1) * sizeof(u64));
  queueCount = 0;
  queueNext = 0;
  skipped = 0;

  // entries already refused as binary or too large are skipped without being read.
  for (u64 index = 0; index < workspace->count; index++) {
    if (!InScope(&include, &exclude, WorkspacePath(workspace, index))) continue;

    bool refused = workspace->entries[index].refusal != WorkspaceRefusalNone;
    if (refused) skipped++;
    else queue[queueCount++] = index;
  }

  FilterFree(&include);
  FilterFree(&exclude);
  fileCount = queueCount + skipped;
  matchedFiles = 0;
  stopped = false;
  capped = false;
  busy = true;
  StrBufAppendFormat(message, "searching %lu files (escape stops)", queueCount);
}

void
ProjectAsk(const Pattern *newPattern)
{
  if (newPattern->impossible) {
    StrBufAppendString(message, "a match cannot cross a line break");
    return;
  }

  const StrBuf *source = &newPattern->source;
  PatternSet(&asked, TextOf(source), source->length, newPattern->options);
  PromptAsk("in files (blank for all): ", TextOf(&scope), ScopeAnswered, nullptr);
}

// reads the entry's file for the preview, unless it is the one already read.
static void
ReadPreview(u64 entry)
{
  if (preview.entry == entry) return;

  preview.entry = entry;
  preview.count = 0;
  u64 length = 0;
  u64 index = WorkspaceEntryOf(workspace, entry);
  const char *path = index == workspaceNone ? "" : TextOf(&workspace->entries[index].path);
  preview.readable = index != workspaceNone && ReadFile(path, &length);
  if (!preview.readable) return;

  free(preview.text);
  preview.text = MemoryDuplicate(readText, length ? length : 1);
  preview.length = length;

  for (u64 at = 0;;) {
    if (preview.count == preview.capacity) {
      preview.capacity = preview.capacity ? preview.capacity * 2 : 1024;
      preview.starts = MemoryResize(preview.starts, preview.capacity, sizeof(u64));
    }

    preview.starts[preview.count++] = at;
    const char *newline = memchr(preview.text + at, '\n', length - at);
    if (!newline) break;

    at = (u64)(newline - preview.text) + 1;
  }
}

// the text of a line of the entry's file, from its buffer when open, or false when there
// is no such line.
static bool
PreviewLine(u64 entry, u64 line, const char **text, u64 *length)
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

  ReadPreview(entry);
  if (!preview.readable || line >= preview.count) return false;

  u64 start = preview.starts[line];
  u64 end = line + 1 < preview.count ? preview.starts[line + 1] - 1 : preview.length;
  if (end > start && preview.text[end - 1] == '\r' && end < preview.length) end--;
  *text = preview.text + start;
  *length = end - start;
  return true;
}

// where a preview line is drawn: its text area, the first column shown and the hit to
// pick out (or a hit with an empty range).
typedef struct PreviewRow {
  i32 row;
  i32 column;
  i32 width;
  u64 left;
  PatternMatch hit;
} PreviewRow;

static void
DrawPreviewLine(const PreviewRow *place, const char *text, u64 length)
{
  LayoutIterator iterator = LayoutBegin(text, length);
  LayoutCluster cluster;
  PatternMatch match = {};
  bool more = true;

  while (LayoutNext(&iterator, &cluster)) {
    if (cluster.column < place->left) continue;

    i64 x = (i64)(cluster.column - place->left);
    if (x + cluster.width > place->width) break;

    if (more && cluster.start >= match.end) {
      more = PatternFind(&pattern, text, length, cluster.start, &match);
    }

    bool matched = more && cluster.start >= match.start && cluster.start < match.end;
    bool isHit = cluster.start >= place->hit.start && cluster.start < place->hit.end;
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

// draws the lines around a hit, with line numbers, scrolled sideways to show the match.
static void
DrawPreview([[maybe_unused]] void *context, u64 id, i32 column, i32 width, i32 height)
{
  const Hit *hit = &hits[id];
  u64 first = hit->line > (u64)(height - 1) / 2 ? hit->line - (u64)(height - 1) / 2 : 0;
  const char *text;
  u64 length;

  if (!PreviewLine(hit->entry, hit->line, &text, &length)) {
    ScreenPutString(0, column + 1, "the line is no longer there", ThemeStyle(ThemeGutter));
    return;
  }

  i32 digits = 1;
  for (u64 last = first + (u64)height; last >= 10; last /= 10) digits++;

  PreviewRow place = {.column = column + digits + 2, .width = width - digits - 2};
  if (place.width < 1) return;

  // the hit is kept in view, a quarter of the way in when the line has to scroll.
  u64 hitStart = hit->start <= length ? hit->start : length;
  u64 hitEnd = hit->end <= length ? hit->end : length;
  u64 startColumn = LayoutColumn(text, length, hitStart);
  u64 endColumn = LayoutColumn(text, length, hitEnd);
  u64 quarter = (u64)place.width / 4;
  bool scrolled = endColumn > (u64)place.width;
  if (scrolled) place.left = startColumn > quarter ? startColumn - quarter : 0;

  for (i32 row = 0; row < height; row++) {
    u64 line = first + (u64)row;
    if (!PreviewLine(hit->entry, line, &text, &length)) break;

    StrBuf number = {};
    StrBufAppendFormat(&number, "%*lu", digits, line + 1);
    Style gutter = line == hit->line ? styleDefault : ThemeStyle(ThemeGutter);
    ScreenPutText(row, column + 1, number.data, number.length, gutter);
    StrBufFree(&number);

    place.row = row;
    place.hit = line == hit->line ? (PatternMatch){hitStart, hitEnd} : (PatternMatch){};
    DrawPreviewLine(&place, text, length);
  }
}

// selects the hit's match in the current buffer, or the start of its line when the text
// has changed since the search.
static void
SelectHit(const Hit *hit)
{
  Document *document = WorkspaceDocument(workspace);
  SearchShow(document, &pattern);

  const Buffer *buffer = &document->buffer;
  u64 count = BufferLineCount(buffer);
  u64 line = hit->line < count ? hit->line : count - 1;
  const StrBuf *text = BufferLine(buffer, line);
  u64 from = hit->start <= text->length ? hit->start : 0;
  PatternMatch match = {};
  bool found = PatternFind(&pattern, TextOf(text), text->length, from, &match) ||
    PatternFind(&pattern, TextOf(text), text->length, 0, &match);

  Position start = {line, match.start};
  Position end = {line, match.end};
  if (found) MoveSelect(document->view, start, end);
  else MoveGoTo(document->view, line, 0);
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (!picked) {
    ProjectClear();
    StrBufAppendString(message, "project search matches cleared");
    return;
  }

  lastOpened = id;
  u64 index = WorkspaceEntryOf(workspace, hits[id].entry);

  if (index == workspaceNone) StrBufAppendString(message, "that buffer has been closed");
  else if (WorkspaceSwitch(workspace, index, message)) SelectHit(&hits[id]);
}

// lists the hits, leaving out those in closed buffers, with the first hit from position on
// selected.
static void
ShowList(u64 position)
{
  StrBuf item = {};
  StrBuf note = {};
  PickerBegin();
  u64 shown = 0;
  u64 selected = 0;
  u64 entry = noHit;
  u64 index = workspaceNone;

  for (u64 at = 0; at < hitCount; at++) {
    const Hit *hit = &hits[at];

    // hits come grouped by entry.
    if (hit->entry != entry) {
      entry = hit->entry;
      index = WorkspaceEntryOf(workspace, entry);
    }

    if (index == workspaceNone) continue;

    if (at < position) selected = shown + 1;
    StrBufClear(&item);
    WorkspaceName(workspace, index, &item);
    StrBufAppendFormat(&item, ":%lu", hit->line + 1);
    StrBufClear(&note);
    StrBufAppend(&note, shownTexts.data + hit->shown, hit->shownLength);
    PickerAdd(item.data, item.length, TextOf(&note), at);
    shown++;
  }

  if (!shown) {
    StrBufAppendString(message, "every buffer with project search matches has been closed");
    StrBufFree(&item);
    StrBufFree(&note);
    return;
  }

  StrBuf title = {};
  DescribeResult(&title);
  u64 left = hitCount - shown;
  if (left) StrBufAppendFormat(&title, ", %lu in closed buffers left out", left);
  PickerOpen(title.data, "filter: ", Picked, nullptr);
  PickerShowPreview(DrawPreview, nullptr);
  PickerSelect(selected < shown ? selected : shown - 1);
  StrBufFree(&title);
  StrBufFree(&item);
  StrBufFree(&note);
}

void
ProjectShow(void)
{
  if (!hitCount) {
    StrBufAppendString(message, "no project search matches to list");
    return;
  }

  u64 next = lastOpened == noHit ? 0 : lastOpened + 1;
  ShowList(next < hitCount ? next : hitCount - 1);
}
