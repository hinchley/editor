#include "project.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "filter.h"
#include "jump.h"
#include "memory.h"
#include "move.h"
#include "multifile.h"
#include "picker.h"
#include "preview.h"
#include "prompt.h"
#include "scan.h"
#include "search.h"
#include "settings.h"

static constexpr u64 noHit = UINT64_MAX;

// the revision of a hit read from a file on disk rather than from an open buffer.
static constexpr u64 noRevision = UINT64_MAX;

// a matching line, in the entry with the serial entry.
// hits are in the order of their entries, then their lines.
typedef struct Hit {
  u64 entry;
  u64 line;

  // the revision of the buffer it was found in, or noRevision and the version of the file
  // it was read from.
  u64 revision;
  FileStamp stamp;

  // its first match, in bytes.
  u64 start;
  u64 end;

  // the line as the list shows it, in shownTexts.
  u64 shown;
  u64 shownLength;
} Hit;

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

// the entry and line of the hit opened last from the list, or noHit.
static u64 openedEntry = noHit;
static u64 openedLine;

// the hits a replace takes, in list order, and the replacement once it is known.
static u64 *chosen;
static u64 chosenCount;
static StrBuf replacement;

// appends a hit whose text is already at the end of shownTexts.
static void
AppendHit(Hit hit)
{
  if (hitCount == hitCapacity) {
    hitCapacity = hitCapacity ? hitCapacity * 2 : 256;
    hits = MemoryResize(hits, hitCapacity, sizeof(Hit));
  }

  if (!hitCount || hits[hitCount - 1].entry != hit.entry) matchedFiles++;
  hits[hitCount++] = hit;
  if (hitCount == maxProjectHits) capped = true;
}

// appends hit, which has its entry, line and source, with its match and shown text.
static void
AddHit(Hit hit, const char *text, u64 length, PatternMatch match)
{
  hit.start = match.start;
  hit.end = match.end;
  hit.shown = shownTexts.length;
  PickerAppendLine(&shownTexts, text, length);
  hit.shownLength = shownTexts.length - hit.shown;
  AppendHit(hit);
}

static void
SearchLine(Hit hit, const char *text, u64 length)
{
  PatternMatch match;
  if (!PatternFind(&pattern, text, length, 0, UINT64_MAX, &match)) return;

  AddHit(hit, text, length, match);
}

static void
SearchBuffer(u64 entry, const Buffer *buffer)
{
  u64 count = BufferLineCount(buffer);

  for (u64 line = 0; line < count && !capped; line++) {
    const StrBuf *text = BufferLine(buffer, line);
    Hit hit = {.entry = entry, .line = line, .revision = buffer->revision};
    SearchLine(hit, StrBufText(text), text->length);
  }
}

// searches text read from disk, where a "\r" before a line break is part of the break.
static void
SearchText(u64 entry, FileStamp stamp, const char *text, u64 length)
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
    Hit hit = {.entry = entry, .line = line, .revision = noRevision, .stamp = stamp};
    SearchLine(hit, text + at, textEnd - at);
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
  else SearchText(serial, item->stamp, item->text, item->length);
  return !capped;
}

static void
Progress([[maybe_unused]] void *context, u64 done, u64 count, StrBuf *text)
{
  StrBufAppendFormat(
    text,
    "searching %lu of %lu file%s: %lu line%s found (escape stops)",
    done,
    count,
    StrBufPlural(count),
    hitCount,
    StrBufPlural(hitCount)
  );
}

static void
DescribeResult(StrBuf *text)
{
  StrBufAppendFormat(
    text,
    "%s: %lu line%s in %lu of %lu file%s",
    StrBufText(&pattern.source),
    hitCount,
    StrBufPlural(hitCount),
    matchedFiles,
    fileCount,
    StrBufPlural(fileCount)
  );

  if (capped) StrBufAppendFormat(text, ", stopped at %lu lines", maxProjectHits);
  const char *plural = StrBufPlural(searched);
  if (stopped) StrBufAppendFormat(text, ", stopped after %lu file%s", searched, plural);
  if (skipped) StrBufAppendFormat(text, ", %lu skipped", skipped);
}

static void
ShowList(u64 position);

static void
Finish([[maybe_unused]] void *context, ScanResult result)
{
  stopped = result.stopped;
  searched = result.done;
  skipped += result.skipped;
  StrBufClear(message);

  if (!hitCount && !stopped) {
    const char *source = StrBufText(&pattern.source);
    StrBufAppendFormat(
      message,
      "no match for %s in %lu file%s",
      source,
      fileCount,
      StrBufPlural(fileCount)
    );
    if (skipped) StrBufAppendFormat(message, " (%lu skipped)", skipped);
    return;
  }

  if (!hitCount) {
    const char *source = StrBufText(&pattern.source);
    StrBufAppendFormat(
      message,
      "stopped: no match for %s in the first %lu of %lu files",
      source,
      searched,
      fileCount
    );
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
  openedEntry = noHit;
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
  const char *text = StrBufText(path);
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
  PatternSet(&pattern, StrBufText(&asked.source), asked.source.length, asked.options);

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
  static const ScanClient client =
    {.visit = SearchEntry, .progress = Progress, .finish = Finish};
  ScanStart(queue, queueCount, &client, nullptr);
  free(queue);
  const char *plural = StrBufPlural(queueCount);
  StrBufAppendFormat(message, "searching %lu file%s (escape stops)", queueCount, plural);
}

void
ProjectAsk(const Pattern *newPattern)
{
  if (PatternReportUnusable(newPattern, message)) return;

  const StrBuf *source = &newPattern->source;
  PatternSet(&asked, StrBufText(source), source->length, newPattern->options);
  PromptAsk("in files (blank for all): ", StrBufText(&scope), ScopeAnswered, nullptr);
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
  bool found =
    PatternFind(&pattern, StrBufText(text), text->length, from, UINT64_MAX, &match) ||
    PatternFind(&pattern, StrBufText(text), text->length, 0, UINT64_MAX, &match);

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

  openedEntry = hits[id].entry;
  openedLine = hits[id].line;
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

// true when hits read from disk no longer number the lines of the entry at index: its
// file changed since, or it was opened and then edited.
// the lines of a buffer edited since its search are matched again, so those stay.
static bool
Moved(const Hit *hit, u64 index)
{
  if (hit->revision != noRevision) return false;

  const Document *document = workspace->entries[index].document;
  if (document && DocumentModified(document)) return true;

  const StrBuf *path = WorkspacePath(workspace, index);
  FileStamp now = document ? document->file.stamp : FileStampOf(path->data);
  return !FileStampEqual(now, hit->stamp);
}

// replaces every match on the lines of the chosen hits from first to end, which are in one
// entry, matching each line again as its document has it now.
// the lines change from the last up, so a replacement holding a line break leaves the
// lines above where they were, and the whole entry undoes in one step.
// returns false, changing nothing, when the entry has been closed or cannot be opened, or
// its file changed since the search (counted in moved).
static bool
ReplaceInEntry(u64 first, u64 end, u64 *lines, u64 *matches, u64 *moved)
{
  u64 index = WorkspaceEntryOf(workspace, hits[chosen[first]].entry);

  if (index != workspaceNone && Moved(&hits[chosen[first]], index)) {
    (*moved)++;
    return false;
  }

  Document *document = MultifileEditBegin(index);
  if (!document) return false;

  Buffer *buffer = &document->buffer;
  StrBuf rebuilt = {};

  for (u64 at = end; at-- > first;) {
    u64 line = hits[chosen[at]].line;
    if (line >= BufferLineCount(buffer)) continue;

    const StrBuf *text = BufferLine(buffer, line);
    PatternMatch match;
    if (!PatternFind(&pattern, text->data, text->length, 0, UINT64_MAX, &match)) continue;

    u64 stop = PatternReplaceLine(
      &pattern,
      text->data,
      text->length,
      match,
      UINT64_MAX,
      &replacement,
      &rebuilt,
      matches
    );
    Range range = {{line, match.start}, {line, stop}};
    BufferReplace(buffer, range, StrBufText(&rebuilt), rebuilt.length);
    (*lines)++;
  }

  StrBufFree(&rebuilt);
  MultifileEditEnd(document);
  return true;
}

static void
ReplaceChosen(void)
{
  u64 lines = 0;
  u64 matches = 0;
  u64 changedFiles = 0;
  u64 failed = 0;
  u64 moved = 0;
  u64 first = 0;
  MultifileBegin("replace");

  while (first < chosenCount) {
    u64 end = first + 1;
    while (end < chosenCount && hits[chosen[end]].entry == hits[chosen[first]].entry) end++;

    u64 before = matches;
    u64 movedBefore = moved;
    bool done = ReplaceInEntry(first, end, &lines, &matches, &moved);
    if (!done && moved == movedBefore) failed++;
    else if (matches > before) changedFiles++;
    first = end;
  }

  // the hits no longer describe the text.
  ProjectClear();

  if (matches) {
    const char *plural = matches == 1 ? "" : "es";
    StrBufAppendFormat(
      message,
      "replaced %lu match%s on %lu line%s in %lu file%s",
      matches,
      plural,
      lines,
      StrBufPlural(lines),
      changedFiles,
      StrBufPlural(changedFiles)
    );
    StrBufAppendString(message, " (alt+shift+z reviews or undoes, ctrl+shift+s saves)");
  } else {
    StrBufAppendString(message, "no match left on those lines");
  }

  if (failed) {
    StrBufAppendFormat(
      message,
      ", %lu file%s could not be opened",
      failed,
      StrBufPlural(failed)
    );
  }

  if (moved) {
    StrBufAppendFormat(
      message,
      ", %lu file%s changed since the search (search again)",
      moved,
      StrBufPlural(moved)
    );
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
  StrBufAppend(&replacement, StrBufText(text), text->length);

  StrBuf question = {};
  u64 files = ChosenFiles();
  StrBufAppendFormat(
    &question,
    "replace in %lu line%s of %lu file%s? (y/n)",
    chosenCount,
    StrBufPlural(chosenCount),
    files,
    StrBufPlural(files)
  );
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
    PickerAdd(item.data, item.length, StrBufText(&note), at);
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

// the buffer to search the hits of an entry again in, starting with hit, or nullptr when
// they still describe the text: the entry is not open, or its hits came from its buffer
// as it still is.
static const Buffer *
ChangedBuffer(const Hit *hit)
{
  u64 index = WorkspaceEntryOf(workspace, hit->entry);
  if (index == workspaceNone || !workspace->entries[index].document) return nullptr;

  const Buffer *buffer = &workspace->entries[index].document->buffer;
  return hit->revision == buffer->revision ? nullptr : buffer;
}

// finds the hits again in every open buffer that changed since they were found (or that
// they were read from disk for), so the list follows edits.
// files without hits are not searched again.
static void
RefreshHits(void)
{
  bool changed = false;

  for (u64 at = 0; at < hitCount && !changed; at++) {
    bool first = !at || hits[at].entry != hits[at - 1].entry;
    changed = first && ChangedBuffer(&hits[at]);
  }

  if (!changed) return;

  Hit *old = hits;
  u64 oldCount = hitCount;
  StrBuf oldTexts = shownTexts;
  hits = nullptr;
  hitCount = 0;
  hitCapacity = 0;
  shownTexts = (StrBuf){};
  matchedFiles = 0;

  // a search that stopped at maxProjectHits still did.
  bool wasCapped = capped;
  capped = false;
  u64 at = 0;

  while (at < oldCount && !capped) {
    u64 end = at + 1;
    while (end < oldCount && old[end].entry == old[at].entry) end++;
    const Buffer *buffer = ChangedBuffer(&old[at]);

    if (buffer) {
      SearchBuffer(old[at].entry, buffer);
    } else {
      for (u64 index = at; index < end && !capped; index++) {
        Hit hit = old[index];
        hit.shown = shownTexts.length;
        StrBufAppend(&shownTexts, oldTexts.data + old[index].shown, hit.shownLength);
        AppendHit(hit);
      }
    }

    at = end;
  }

  capped = capped || wasCapped;
  free(old);
  StrBufFree(&oldTexts);
}

// the first hit after the one opened last, or the last hit when none is after it.
static u64
NextHit(void)
{
  if (openedEntry == noHit) return 0;

  for (u64 at = 0; at < hitCount; at++) {
    const Hit *hit = &hits[at];
    if (hit->entry > openedEntry) return at;
    if (hit->entry == openedEntry && hit->line > openedLine) return at;
  }

  return hitCount - 1;
}

void
ProjectShow(void)
{
  if (!hitCount) {
    StrBufAppendString(message, "no project search matches to list");
    return;
  }

  RefreshHits();

  if (!hitCount) {
    StrBufAppendString(message, "the project search matches no longer match");
    return;
  }

  ShowList(NextHit());
}
