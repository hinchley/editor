#pragma once

// changes: how a buffer differs from its file in the last commit of the git repository
// holding it.
// the committed text (the base) is read once, and again only when HEAD names another
// commit.
// the hunks are worked out again whenever they are asked for after the buffer changed, as
// the frame does, rather than by following each edit.
#include "buffer.h"
#include "diff.h"
#include "git.h"
#include "strbuf.h"
#include "types.h"

typedef struct Changes Changes;

// how the gutter marks a line.
enum ChangeMark {
  ChangeMarkNone,
  ChangeMarkAdded,
  ChangeMarkModified,

  // lines were removed just before this line, or after it when it is the last.
  ChangeMarkRemovedAbove,
  ChangeMarkRemovedBelow
};

// lines changed in place, added and removed: a hunk of o old lines and n new ones changes
// min(o, n), and adds or removes the rest.
typedef struct ChangeCounts {
  u64 changed;
  u64 added;
  u64 removed;
} ChangeCounts;

// the changes of buffer, the text of the file at path, or nullptr when the file is not in
// a repository the editor can read.
// a file the last commit does not have (untracked, or a branch with no commits) gets
// changes with no hunks, which a later commit can bring.
Changes *
ChangesNew(const Buffer *buffer, const char *path);

// the changes of text (a file as read from disk, which is copied) against the file at path
// in the last commit, for a file that is not open; nullptr as for ChangesNew.
Changes *
ChangesNewText(const char *path, const char *text, u64 length);

void
ChangesFree(Changes *changes);

// the path the changes were made for, as given.
const char *
ChangesPath(const Changes *changes);

// the lines of the newer side (the buffer, or the text), without their line breaks.
u64
ChangesLineCount(const Changes *changes);

void
ChangesLine(const Changes *changes, u64 line, const char **text, u64 *length);

// reads the base again when HEAD names another commit than the one it came from.
void
ChangesRefresh(Changes *changes);

// true when the last commit has the file, so there is something to compare with.
bool
ChangesTracked(const Changes *changes);

// the hunks between the base and the buffer now, in order (old lines are the base's).
// none when changes is nullptr or the file is not tracked, or the buffer or base is larger
// than maxDiffSize.
const DiffHunk *
ChangesHunks(Changes *changes, u64 *count);

// the index of the hunk holding a line of the buffer, or of the removal marked on it, or
// UINT64_MAX when there is none.
u64
ChangesHunkAt(Changes *changes, u64 line);

// the mark and the counts drawn in a frame. while the buffer keeps changing they come
// from the hunks of an earlier revision, worked out again once it has rested
// changesDelay, so they can lag the text that long; commands use ChangesHunks.
enum ChangeMark
ChangesMarkOf(Changes *changes, u64 line);

ChangeCounts
ChangesCount(Changes *changes);

// milliseconds until the marks are due to be worked out again, or -1 when none are.
i32
ChangesTimeout(void);

// adds a hunk's counts to counts.
void
ChangesCountHunk(const DiffHunk *hunk, ChangeCounts *counts);

// a line of the base, valid until the base is read again.
void
ChangesBaseLine(const Changes *changes, u64 line, const char **text, u64 *length);

// the hunks between text (a file as read from disk) and the base, for files that are not
// open; returns false when the file at path has no changes to compare with.
bool
ChangesOfText(const char *path, const char *text, u64 length, DiffHunks *hunks);
