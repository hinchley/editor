#include "refactor.h"

#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "lsp.h"
#include "memory.h"
#include "move.h"
#include "multifile.h"
#include "picker.h"
#include "prompt.h"

// the question waiting for an answer, and where the cursor was when it was asked.
static struct {
  LspRequest request;
  WorkspaceSpot at;
} asked;

// the fixes offered, kept while the picker of them shows: their titles and changes, with
// the paths and texts of the changes in one string.
typedef struct Fix {
  u64 title;
  u32 first;
  u32 count;
} Fix;

static Fix *fixes;
static u32 fixCount;
static LspEdit *fixEdits;
static u32 fixEditCount;
static StrBuf fixStrings;

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
  asked.at = WorkspaceSpotNow(workspace);
}

// true when the buffer asked about is still current and unchanged (and, when cursor
// matters, the cursor has not moved), which ends the wait for the answer.
static bool
Answered(bool cursor)
{
  asked.request = 0;
  return WorkspaceSpotSame(workspace, asked.at, cursor);
}

// the order changes are made in: by file, and in a file from the last up (so the ranges
// still to come stay where the server said), with insertions at one place in reverse, so
// they end up in the server's order.
static const LspEdit *sorting;

static int
CompareEdits(const void *first, const void *second)
{
  u32 left = *(const u32 *)first;
  u32 right = *(const u32 *)second;
  i32 order = strcmp(sorting[left].path, sorting[right].path);
  if (order) return order;

  order = PositionCompare(sorting[right].range.start, sorting[left].range.start);
  if (order) return order;
  return left < right ? 1 : -1;
}

// makes a server's changes, named by what, each file's as one undo step (see
// multifile.h), and returns how many files changed; files that could not be loaded or
// opened are counted in failed.
static u64
Apply(const char *what, const LspEdit *edits, u32 count, u64 *failed)
{
  u32 *order = MemoryAllocate((count ? count : 1) * sizeof(u32));
  for (u32 index = 0; index < count; index++) order[index] = index;
  sorting = edits;
  qsort(order, count, sizeof(u32), CompareEdits);

  u64 files = 0;
  *failed = 0;
  MultifileBegin(what);

  for (u32 first = 0, end; first < count; first = end) {
    const char *path = edits[order[first]].path;
    end = first + 1;
    while (end < count && strcmp(edits[order[end]].path, path) == 0) end++;

    StrBuf ignored = {};
    u64 index = WorkspaceLoadPath(workspace, path, &ignored);
    Document *document = MultifileEditBegin(index);
    StrBufFree(&ignored);

    if (!document) {
      (*failed)++;
      continue;
    }

    for (u32 at = first; at < end; at++) {
      const LspEdit *edit = &edits[order[at]];
      BufferReplace(&document->buffer, edit->range, edit->text, edit->length);
    }

    MultifileEditEnd(document);
    files++;
  }

  free(order);
  return files;
}

static void
ReportFailed(u64 failed)
{
  if (!failed) return;

  StrBufAppendFormat(
    message,
    ", %lu file%s could not be opened",
    failed,
    StrBufPlural(failed)
  );
}

static void
Renamed([[maybe_unused]] void *context, const LspEdit *edits, u32 count, bool answered)
{
  if (!Answered(true) || !answered) return;

  if (!count) {
    StrBufAppendString(message, "nothing to rename");
    return;
  }

  u64 failed;
  u64 files = Apply("rename", edits, count, &failed);
  StrBufAppendFormat(
    message,
    "renamed %u place%s in %lu file%s",
    count,
    StrBufPlural(count),
    files,
    StrBufPlural(files)
  );
  StrBufAppendString(message, " (alt+shift+z reviews or undoes, ctrl+shift+s saves)");
  ReportFailed(failed);
}

// the name the rename started from, to tell whether the answer changes it.
static StrBuf oldName;

static void
NameGiven([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer || !length) return;
  if (length == oldName.length && memcmp(answer, oldName.data, length) == 0) return;

  Position head = Head();
  LspRequest request = LspRename(Current()->lsp, head, answer, length, Renamed, nullptr);
  Asking(request);
  if (!request) StrBufAppendString(message, "the language server cannot rename now");
}

// asks for the new name, starting from the name in range, on the cursor's line.
static void
AskName(Range range)
{
  const Buffer *buffer = &Current()->buffer;
  StrBufClear(&oldName);

  if (range.start.line == range.end.line && range.start.byte < range.end.byte) {
    BufferGetText(buffer, range, &oldName);
  }

  if (!oldName.length) {
    StrBufAppendString(message, "no name at the cursor");
    return;
  }

  PromptAsk("rename to: ", oldName.data, NameGiven, nullptr);
  PromptSelectText();
}

// the word at the cursor, the name to start from when the server leaves it to the editor.
static Range
WordAtCursor(void)
{
  Position head = Head();
  const StrBuf *line = BufferLine(&Current()->buffer, head.line);
  u64 start;
  u64 end;
  if (!MoveWordAround(line, head.byte, &start, &end)) return (Range){head, head};
  return (Range){{head.line, start}, {head.line, end}};
}

static void
Prepared(
  [[maybe_unused]] void *context,
  const LspLocation *locations,
  u32 count,
  bool answered
)
{
  if (!Answered(true) || !answered) return;

  if (!count) {
    StrBufAppendString(message, "this cannot be renamed");
    return;
  }

  Range range = locations[0].range;
  bool empty = PositionCompare(range.start, range.end) == 0;
  AskName(empty ? WordAtCursor() : range);
}

void
RefactorRename(void)
{
  LspDocument *lsp = Current()->lsp;

  if (!LspRenames(lsp)) {
    StrBufAppendString(message, "renaming needs a language server that renames");
    return;
  }

  // a server that does not check names first gets the word at the cursor.
  LspRequest request = LspPrepareRename(lsp, Head(), Prepared, nullptr);
  Asking(request);
  if (!request) AskName(WordAtCursor());
}

static void
ForgetFixes(void)
{
  free(fixes);
  free(fixEdits);
  StrBufFree(&fixStrings);
  fixes = nullptr;
  fixEdits = nullptr;
  fixCount = 0;
  fixEditCount = 0;
}

static void
ApplyFix(const char *title, const LspEdit *edits, u32 count)
{
  u64 failed;
  u64 files = Apply("fix", edits, count, &failed);
  StrBufAppendFormat(message, "fixed: %s", title);
  if (files > 1) {
    StrBufAppendFormat(message, " (in %lu files: alt+shift+z reviews or undoes)", files);
  }
  ReportFailed(failed);
}

static void
FixPicked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (picked) {
    const Fix *fix = &fixes[id];
    ApplyFix(fixStrings.data + fix->title, fixEdits + fix->first, fix->count);
  }

  ForgetFixes();
}

// keeps the fixes offered for the picker, which outlives the answer.
static void
KeepFixes(const LspAction *actions, u32 count)
{
  ForgetFixes();
  fixes = MemoryAllocate(count * sizeof(Fix));
  for (u32 index = 0; index < count; index++) fixEditCount += actions[index].editCount;
  fixEdits = MemoryAllocate((fixEditCount ? fixEditCount : 1) * sizeof(LspEdit));
  u64 *offsets = MemoryAllocate((fixEditCount ? fixEditCount : 1) * 2 * sizeof(u64));
  u32 at = 0;

  // the string grows while the changes are copied, so they point into it at the end.
  for (u32 index = 0; index < count; index++) {
    const LspAction *action = &actions[index];
    fixes[index] = (Fix){fixStrings.length, at, action->editCount};
    StrBufAppendString(&fixStrings, action->title);
    StrBufAppendByte(&fixStrings, '\0');

    for (u32 edit = 0; edit < action->editCount; edit++, at++) {
      const LspEdit *source = &action->edits[edit];
      fixEdits[at] = *source;
      offsets[at * 2] = fixStrings.length;
      StrBufAppendString(&fixStrings, source->path);
      StrBufAppendByte(&fixStrings, '\0');
      offsets[at * 2 + 1] = fixStrings.length;
      StrBufAppend(&fixStrings, source->text, source->length);
      StrBufAppendByte(&fixStrings, '\0');
    }
  }

  for (u32 index = 0; index < fixEditCount; index++) {
    fixEdits[index].path = fixStrings.data + offsets[index * 2];
    fixEdits[index].text = fixStrings.data + offsets[index * 2 + 1];
  }

  free(offsets);
  fixCount = count;
}

static void
Fixed([[maybe_unused]] void *context, const LspAction *actions, u32 count, bool answered)
{
  if (!Answered(true) || !answered) return;

  if (!count) {
    StrBufAppendString(message, "no fix for this line");
    return;
  }

  if (count == 1) {
    ApplyFix(actions[0].title, actions[0].edits, actions[0].editCount);
    return;
  }

  // the server's preferred fix comes first.
  KeepFixes(actions, count);
  PickerBegin();
  u32 first = count;

  for (u32 index = 0; index < count; index++) {
    const char *title = actions[index].title;
    PickerAdd(title, strlen(title), actions[index].preferred ? "preferred" : nullptr, index);
    if (actions[index].preferred && first == count) first = index;
  }

  if (first == count) first = 0;

  StrBuf title = {};
  StrBufAppendFormat(&title, "%u fixes for line %lu", count, asked.at.head.line + 1);
  PickerOpen(title.data, "fix: ", FixPicked, nullptr);
  PickerSelect(first);
  StrBufFree(&title);
}

void
RefactorFix(void)
{
  LspDocument *lsp = Current()->lsp;
  LspRequest request = LspFixes(lsp, Head().line, Fixed, nullptr);
  Asking(request);

  if (!request && !lsp) StrBufAppendString(message, "fixes need a language server");
  else if (!request) StrBufAppendString(message, "the language server offers no fixes now");
}

static void
Formatted([[maybe_unused]] void *context, const LspEdit *edits, u32 count, bool answered)
{
  // the cursor may move while the server formats, but the text must not change.
  if (!Answered(false)) {
    StrBufAppendString(message, "the buffer changed while it was being formatted");
    return;
  }

  if (!answered) return;

  if (!count) {
    StrBufAppendString(message, "already formatted");
    return;
  }

  u64 failed;
  Apply("formatting", edits, count, &failed);
  StrBufAppendFormat(message, "formatted (%u change%s)", count, StrBufPlural(count));
  ReportFailed(failed);
}

void
RefactorFormat(bool spaces)
{
  LspDocument *lsp = Current()->lsp;
  LspRequest request = LspFormat(lsp, spaces, Formatted, nullptr);
  Asking(request);

  if (!request && !lsp) StrBufAppendString(message, "formatting needs a language server");
  else if (!request) StrBufAppendString(message, "the language server cannot format now");
}
