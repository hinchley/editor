#include "changes.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "settings.h"
#include "term.h"

// text in one piece split into lines at "\n", with a "\r" before it left out, as preview
// and project search do, since a buffer holds its lines without either.
typedef struct Lines {
  const char *text;
  u64 length;
  u64 *starts;
  u64 count;
} Lines;

struct Changes {
  // the newer side: a buffer, or text read from disk (with buffer nullptr).
  const Buffer *buffer;
  StrBuf text;
  Lines textLines;

  StrBuf path;
  GitRepo *repo;
  StrBuf relative;

  // the commit the base was read from, and whether it has the file.
  bool read;
  bool headValid;
  GitHash commit;
  bool tracked;
  StrBuf base;
  Lines lines;

  // the hunks, and the buffer revision they were worked out at.
  bool computed;
  u64 revision;
  DiffHunks hunks;

  // the revision the buffer was last seen at, and when it was first seen at it.
  u64 seenRevision;
  i64 seenAt;
};

// when the marks drawn while typing are due to be worked out again, or 0.
static i64 dueAt;

static void
SplitLines(Lines *lines, const char *text, u64 length)
{
  free(lines->starts);
  *lines = (Lines){.text = text ? text : "", .length = length};
  u64 capacity = 64;
  lines->starts = MemoryAllocate(capacity * sizeof(u64));

  for (u64 at = 0;;) {
    if (lines->count == capacity) {
      capacity *= 2;
      lines->starts = MemoryResize(lines->starts, capacity, sizeof(u64));
    }

    lines->starts[lines->count++] = at;
    const char *newline = memchr(lines->text + at, '\n', length - at);
    if (!newline) break;

    at = (u64)(newline - lines->text) + 1;
  }
}

static void
LineOfLines(const void *context, u64 index, const char **text, u64 *length)
{
  const Lines *lines = context;
  u64 start = lines->starts[index];
  u64 end = index + 1 < lines->count ? lines->starts[index + 1] - 1 : lines->length;
  if (end > start && lines->text[end - 1] == '\r' && end < lines->length) end--;
  *text = lines->text + start;
  *length = end - start;
}

static void
LineOfBuffer(const void *context, u64 index, const char **text, u64 *length)
{
  const StrBuf *line = BufferLine(context, index);
  *text = line->data ? line->data : "";
  *length = line->length;
}

static void
ReadBase(Changes *changes)
{
  changes->read = true;
  changes->computed = false;
  changes->tracked = false;
  changes->headValid = GitHead(changes->repo, &changes->commit);

  if (changes->headValid) {
    const char *relative = changes->relative.data;
    enum GitFile status =
      GitReadFile(changes->repo, &changes->commit, relative, &changes->base);
    changes->tracked = status == GitFileOk;
  }

  if (!changes->tracked) StrBufFree(&changes->base);
  SplitLines(&changes->lines, changes->base.data, changes->base.length);
}

Changes *
ChangesNew(const Buffer *buffer, const char *path)
{
  StrBuf relative = {};
  GitRepo *repo = GitRepoOf(path, &relative);

  if (!repo) {
    StrBufFree(&relative);
    return nullptr;
  }

  Changes *changes = MemoryAllocateZeroed(1, sizeof(Changes));
  changes->buffer = buffer;
  StrBufAppendString(&changes->path, path);
  changes->repo = repo;
  changes->relative = relative;
  ReadBase(changes);
  return changes;
}

Changes *
ChangesNewText(const char *path, const char *text, u64 length)
{
  Changes *changes = ChangesNew(nullptr, path);
  if (!changes) return nullptr;

  StrBufAppend(&changes->text, text, length);
  SplitLines(&changes->textLines, changes->text.data, changes->text.length);
  return changes;
}

void
ChangesFree(Changes *changes)
{
  if (!changes) return;

  StrBufFree(&changes->path);
  StrBufFree(&changes->relative);
  StrBufFree(&changes->base);
  free(changes->lines.starts);
  StrBufFree(&changes->text);
  free(changes->textLines.starts);
  DiffFree(&changes->hunks);
  free(changes);
}

const char *
ChangesPath(const Changes *changes)
{
  return changes->path.data;
}

void
ChangesRefresh(Changes *changes)
{
  if (!changes) return;

  GitHash commit;
  bool valid = GitHead(changes->repo, &commit);
  bool same = valid == changes->headValid &&
    (!valid || GitHashEqual(&commit, &changes->commit));
  if (!same) ReadBase(changes);
}

bool
ChangesTracked(const Changes *changes)
{
  return changes && changes->tracked;
}

u64
ChangesLineCount(const Changes *changes)
{
  return changes->buffer ? BufferLineCount(changes->buffer) : changes->textLines.count;
}

u64
ChangesMarkLine(const Changes *changes, const DiffHunk *hunk)
{
  u64 last = ChangesLineCount(changes) - 1;
  return hunk->newStart <= last ? hunk->newStart : last;
}

void
ChangesLine(const Changes *changes, u64 line, const char **text, u64 *length)
{
  if (changes->buffer) {
    LineOfBuffer(changes->buffer, line, text, length);
  } else {
    LineOfLines(&changes->textLines, line, text, length);
  }
}

static bool
FitsDiff(const Changes *changes)
{
  if (changes->base.length > maxDiffSize) return false;

  const Buffer *buffer = changes->buffer;
  if (!buffer) return changes->text.length <= maxDiffSize;

  u64 size = 0;
  u64 count = BufferLineCount(buffer);

  for (u64 line = 0; line < count; line++) {
    size += BufferLine(buffer, line)->length + 1;
    if (size > maxDiffSize) return false;
  }

  return true;
}

// the hunks; with settled, those of an earlier revision are kept until the buffer has
// rested changesDelay, so the frame drawn after each key does not diff the whole file.
static const DiffHunk *
Hunks(Changes *changes, u64 *count, bool settled)
{
  *count = 0;
  if (!changes || !changes->tracked) return nullptr;

  const Buffer *buffer = changes->buffer;
  u64 revision = buffer ? buffer->revision : 0;
  i64 now = TermNow();

  if (changes->seenRevision != revision) {
    changes->seenRevision = revision;
    changes->seenAt = now;
  }

  i64 due = changes->seenAt + changesDelay;
  bool stale = !changes->computed || changes->revision != revision;

  if (stale && settled && changes->computed && now < due) {
    if (!dueAt || due < dueAt) dueAt = due;
  } else if (stale) {
    changes->computed = true;
    changes->revision = revision;
    changes->hunks.count = 0;

    if (FitsDiff(changes)) {
      DiffSource old = {changes->lines.count, LineOfLines, &changes->lines};
      DiffSource new = {changes->textLines.count, LineOfLines, &changes->textLines};
      if (buffer) new = (DiffSource){BufferLineCount(buffer), LineOfBuffer, buffer};
      DiffRun(&old, &new, maxDiffCost, &changes->hunks);
    }
  }

  *count = changes->hunks.count;
  return changes->hunks.hunks;
}

const DiffHunk *
ChangesHunks(Changes *changes, u64 *count)
{
  return Hunks(changes, count, false);
}

i32
ChangesTimeout(void)
{
  if (!dueAt) return -1;

  i64 left = dueAt - TermNow();
  if (left > 0) return (i32)left;

  dueAt = 0;
  return 0;
}

static u64
HunkAt(Changes *changes, u64 line, bool settled)
{
  u64 count;
  const DiffHunk *hunks = Hunks(changes, &count, settled);

  // the last hunk that starts at or before the line.
  u64 low = 0;
  u64 high = count;

  while (low < high) {
    u64 middle = low + (high - low) / 2;

    if (hunks[middle].newStart <= line) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }

  if (low) {
    const DiffHunk *hunk = &hunks[low - 1];
    bool inside = line < hunk->newStart + hunk->newCount;
    bool removal = hunk->newCount == 0 && hunk->newStart == line;
    if (inside || removal) return low - 1;
  }

  // lines removed from the end of the file are marked on the last line.
  if (count) {
    const DiffHunk *last = &hunks[count - 1];
    bool end = last->newCount == 0 && last->newStart > line;
    if (end && ChangesMarkLine(changes, last) == line) return count - 1;
  }

  return UINT64_MAX;
}

u64
ChangesHunkAt(Changes *changes, u64 line)
{
  return HunkAt(changes, line, false);
}

enum ChangeMark
ChangesMarkOf(Changes *changes, u64 line)
{
  if (!changes || !changes->tracked) return ChangeMarkNone;

  u64 index = HunkAt(changes, line, true);
  if (index == UINT64_MAX) return ChangeMarkNone;

  const DiffHunk *hunk = &changes->hunks.hunks[index];

  if (hunk->newCount == 0) {
    bool below = hunk->newStart > line;
    return below ? ChangeMarkRemovedBelow : ChangeMarkRemovedAbove;
  }

  return hunk->oldCount ? ChangeMarkModified : ChangeMarkAdded;
}

void
ChangesCountHunk(const DiffHunk *hunk, ChangeCounts *counts)
{
  u64 changed = hunk->oldCount < hunk->newCount ? hunk->oldCount : hunk->newCount;
  counts->changed += changed;
  counts->added += hunk->newCount - changed;
  counts->removed += hunk->oldCount - changed;
}

ChangeCounts
ChangesCount(Changes *changes)
{
  ChangeCounts counts = {};
  u64 count;
  const DiffHunk *hunks = Hunks(changes, &count, true);
  for (u64 index = 0; index < count; index++) ChangesCountHunk(&hunks[index], &counts);
  return counts;
}

void
ChangesBaseLine(const Changes *changes, u64 line, const char **text, u64 *length)
{
  LineOfLines(&changes->lines, line, text, length);
}
