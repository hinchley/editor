#include "project.h"

#include <stdlib.h>
#include <string.h>

#include "filter.h"
#include "jump.h"
#include "memory.h"
#include "move.h"
#include "picker.h"
#include "preview.h"
#include "prompt.h"
#include "scan.h"
#include "search.h"
#include "settings.h"

// how much of a matching line the list keeps, in bytes.
static constexpr u64 maxShownLength = 200;

static constexpr u64 noHit = UINT64_MAX;

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

// how many files a stopped search went through.
static u64 searched;

// the search was stopped with escape, or by reaching maxProjectHits.
static bool stopped;
static bool capped;

// the hit opened last from the list, or noHit.
static u64 lastOpened = noHit;

// the hits a replace takes, in list order, and the replacement once it is known.
static u64 *chosen;
static u64 chosenCount;
static StrBuf replacement;

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

static const char *
Plural(u64 count)
{
  return count == 1 ? "" : "s";
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

static bool
SearchEntry([[maybe_unused]] void *context, const ScanItem *item)
{
  u64 serial = workspace->entries[item->index].serial;
  if (item->document) SearchBuffer(serial, &item->document->buffer);
  else SearchText(serial, item->text, item->length);
  return !capped;
}

static void
Progress([[maybe_unused]] void *context, u64 done, u64 count, StrBuf *text)
{
  StrBufAppendFormat(
    text,
    "searching %lu of %lu files: %lu line%s found (escape stops)",
    done,
    count,
    hitCount,
    Plural(hitCount)
  );
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
  if (stopped) StrBufAppendFormat(text, ", stopped after %lu files", searched);
  if (skipped) StrBufAppendFormat(text, ", %lu skipped", skipped);
}

static void ShowList(u64 position);

static void
Finish([[maybe_unused]] void *context, ScanResult result)
{
  stopped = result.stopped;
  searched = result.done;
  skipped += result.skipped;
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
    StrBufAppendFormat(message, format, source, searched, fileCount);
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

// hits remember entries by serial, so they never lead to the wrong file; this only frees
// them.
void
ProjectClear(void)
{
  hitCount = 0;
  StrBufClear(&shownTexts);
  lastOpened = noHit;
  PreviewForget();
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
  u64 *queue = MemoryAllocate((workspace->count ? workspace->count : 1) * sizeof(u64));
  u64 queueCount = 0;
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
  static const ScanClient client = {.visit = SearchEntry, .progress = Progress, .finish = Finish};
  ScanStart(queue, queueCount, &client, nullptr);
  free(queue);
  StrBufAppendFormat(message, "searching %lu files (escape stops)", queueCount);
}

void
ProjectAsk(const Pattern *newPattern)
{
  if (newPattern->error.length) {
    StrBufAppendFormat(message, "bad pattern: %s", newPattern->error.data);
    return;
  }

  if (newPattern->impossible) {
    StrBufAppendString(message, "a match cannot cross a line break");
    return;
  }

  const StrBuf *source = &newPattern->source;
  PatternSet(&asked, TextOf(source), source->length, newPattern->options);
  PromptAsk("in files (blank for all): ", TextOf(&scope), ScopeAnswered, nullptr);
}

// draws the lines around a hit, with every match highlighted.
static void
DrawPreview([[maybe_unused]] void *context, u64 id, i32 column, i32 width, i32 height)
{
  const Hit *hit = &hits[id];
  PreviewDraw(hit->entry, hit->line, hit->start, hit->end, &pattern, column, width, height);
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

  if (index == workspaceNone) {
    StrBufAppendString(message, "that buffer has been closed");
    return;
  }

  JumpRecord();
  if (WorkspaceSwitch(workspace, index, message)) SelectHit(&hits[id]);
}

// the number of entries the chosen hits are in.
static u64
ChosenFiles(void)
{
  u64 files = 0;

  for (u64 at = 0; at < chosenCount; at++) {
    if (!at || hits[chosen[at]].entry != hits[chosen[at - 1]].entry) files++;
  }

  return files;
}

// replaces every match on the lines of the chosen hits from first to end, which are in one
// entry, matching each line again as its document has it now.
// the lines change from the last up, so a replacement holding a line break leaves the
// lines above where they were, and the whole entry undoes in one step.
// returns false when the entry has been closed or cannot be opened.
static bool
ReplaceInEntry(u64 first, u64 end, u64 *lines, u64 *matches)
{
  u64 index = WorkspaceEntryOf(workspace, hits[chosen[first]].entry);
  StrBuf ignored = {};
  bool opened = index != workspaceNone && WorkspaceOpen(workspace, index, &ignored);
  StrBufFree(&ignored);
  if (!opened) return false;

  Document *document = workspace->entries[index].document;
  Buffer *buffer = &document->buffer;
  UndoBreak(&document->undo);
  UndoBegin(&document->undo, UndoKindOther, &document->view->cursors);
  StrBuf rebuilt = {};

  for (u64 at = end; at-- > first;) {
    u64 line = hits[chosen[at]].line;
    if (line >= BufferLineCount(buffer)) continue;

    const StrBuf *text = BufferLine(buffer, line);
    PatternMatch match;
    if (!PatternFind(&pattern, text->data, text->length, 0, &match)) continue;

    u64 stop = PatternReplaceLine(
      &pattern, text->data, text->length, match, UINT64_MAX, &replacement, &rebuilt, matches
    );
    Range range = {{line, match.start}, {line, stop}};
    BufferReplace(buffer, range, TextOf(&rebuilt), rebuilt.length);
    (*lines)++;
  }

  StrBufFree(&rebuilt);
  UndoEnd(&document->undo, &document->view->cursors);
  return true;
}

static void
ReplaceChosen(void)
{
  u64 lines = 0;
  u64 matches = 0;
  u64 changedFiles = 0;
  u64 failed = 0;
  u64 first = 0;

  while (first < chosenCount) {
    u64 end = first + 1;
    while (end < chosenCount && hits[chosen[end]].entry == hits[chosen[first]].entry) end++;

    u64 before = matches;
    if (!ReplaceInEntry(first, end, &lines, &matches)) failed++;
    else if (matches > before) changedFiles++;
    first = end;
  }

  // the hits no longer describe the text.
  ProjectClear();

  if (matches) {
    const char *format = "replaced %lu match%s on %lu line%s in %lu file%s";
    const char *plural = matches == 1 ? "" : "es";
    StrBufAppendFormat(
      message, format, matches, plural, lines, Plural(lines), changedFiles, Plural(changedFiles)
    );
    StrBufAppendString(message, " (ctrl+u reviews, ctrl+shift+s saves)");
  } else {
    StrBufAppendString(message, "no match left on those lines");
  }

  if (failed) {
    StrBufAppendFormat(message, ", %lu file%s could not be opened", failed, Plural(failed));
  }
}

static void
Confirmed([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  if (answer && *answer == 'y') ReplaceChosen();
  else PickerResume();
}

static void
ReplacementGiven(const StrBuf *text)
{
  if (!text) {
    PickerResume();
    return;
  }

  StrBufClear(&replacement);
  StrBufAppend(&replacement, TextOf(text), text->length);

  StrBuf question = {};
  u64 files = ChosenFiles();
  const char *format = "replace in %lu line%s of %lu file%s? (y/n)";
  StrBufAppendFormat(&question, format, chosenCount, Plural(chosenCount), files, Plural(files));
  PromptChoose(question.data, "yn", Confirmed, nullptr);
  StrBufFree(&question);
}

// ctrl+r replaces the matches on the lines the list shows.
static bool
ListKey([[maybe_unused]] void *context, Key key)
{
  if (key.modifiers != KeyModifierControl || key.code != 'r') return false;

  chosenCount = PickerShownCount();

  if (!chosenCount) {
    StrBufAppendString(message, "no lines to replace in");
    return true;
  }

  chosen = MemoryResize(chosen, chosenCount, sizeof(u64));
  for (u64 at = 0; at < chosenCount; at++) chosen[at] = PickerShownId(at);

  PickerSuspend();
  SearchReplacement(&pattern, ReplacementGiven);
  return true;
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
  PickerHandleKeys(ListKey, nullptr);
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
