#pragma once

// the buffer list: every loaded file and unnamed buffer, which of them are open, which one
// is current, and when each was last used.
// an entry is loaded when it is in the list and open once its document has been read,
// which happens the first time it is used.
// documents are allocated one by one, so they never move while the list grows.
// entries are only ever appended, and removing one keeps the order of the rest, so an
// entry's index can change but its serial never does.
// anything outside the workspace that remembers an entry keeps its serial, which simply
// stops matching once the entry is removed or a folder replaces every entry.
// the window shows one view, or two side by side when split.
// each side uses its own slot of every document's views, and the focused view's entry is
// the current one.
#include "document.h"
#include "file.h"
#include "loader.h"
#include "strbuf.h"
#include "types.h"
#include "view.h"

// why an entry is never opened, found the first time it was tried.
enum WorkspaceRefusal {
  WorkspaceRefusalNone,
  WorkspaceRefusalTooLarge,
  WorkspaceRefusalBinary
};

typedef struct WorkspaceEntry {
  // the display path until the entry is opened; after that its document holds it.
  StrBuf path;

  // the file the path led to when loaded, or zero for a new or unnamed buffer.
  FileId id;

  // nullptr until the entry is opened.
  Document *document;
  enum WorkspaceRefusal refusal;

  // tells unnamed buffers apart (from 1), or 0 for a named one.
  u32 unnamed;

  // when the entry was last made current, or 0 when it never was.
  u64 used;

  // unique for the whole session, and increasing along the list.
  u64 serial;
} WorkspaceEntry;

typedef struct Workspace {
  WorkspaceEntry *entries;
  u64 count;
  u64 capacity;

  // the entry being edited, always open once the workspace has any entry.
  u64 current;
  u64 openCount;

  // the slot of the focused view.
  u32 slot;

  // while split: the entry the other view shows, and the slot of the view on the left.
  bool split;
  u64 other;
  u32 left;

  const DisplayOptions *options;
  u64 clock;
  u32 unnamedCount;

  // the last serial given out, kept when every entry is freed.
  u64 serials;
} Workspace;

static constexpr u64 workspaceNone = UINT64_MAX;

void
WorkspaceInit(Workspace *workspace, const DisplayOptions *options);

// frees every entry and document and leaves an empty workspace.
// serials go on from where they were, so none is ever given out twice.
void
WorkspaceFree(Workspace *workspace);

// adds every file of a list, in order, taking their paths and leaving the list empty.
// the list is assumed to hold nothing already loaded, as when the workspace is empty.
void
WorkspaceAddList(Workspace *workspace, LoaderList *list);

// the entry already loaded for the same file (or, for a file that does not exist yet,
// the same path), or workspaceNone.
u64
WorkspaceFind(const Workspace *workspace, const LoaderFile *file);

// adds one file, taking its path, and returns its index.
u64
WorkspaceAdd(Workspace *workspace, LoaderFile *file);

// the index of file's entry, adding it when it is not loaded and there is room for it;
// workspaceNone, after saying so in message, when there is not.
u64
WorkspaceLoad(Workspace *workspace, LoaderFile *file, StrBuf *message);

// the index of the entry of the file at path, loading it as ctrl+o does; workspaceNone,
// after saying why in message, when it cannot be (a folder, or no room).
u64
WorkspaceLoadPath(Workspace *workspace, const char *path, StrBuf *message);

// adds an unnamed buffer and returns its index.
u64
WorkspaceAddUnnamed(Workspace *workspace);

// the index of the entry with serial, or workspaceNone when it was removed.
u64
WorkspaceEntryOf(const Workspace *workspace, u64 serial);

// removes an entry, freeing its document.
// when the current entry is removed, the most recently used one (or else the first one
// that opens) becomes current; when the other side of a split showed it, that side shows
// its most recently used entry instead.
// removing the last entry leaves a new unnamed buffer.
// problems opening the replacement are appended to message.
void
WorkspaceRemove(Workspace *workspace, u64 index, StrBuf *message);

// removes the entries with serials, given in list order, as WorkspaceRemove does but in
// one pass; serials that match no entry are skipped.
void
WorkspaceRemoveSome(Workspace *workspace, const u64 *serials, u64 count, StrBuf *message);

// records that an open entry's document now names another file, as after a rename.
void
WorkspaceRenamed(Workspace *workspace, u64 index);

// opens an entry if it is not open yet.
// returns false, appending the reason to message, when it cannot be opened.
bool
WorkspaceOpen(Workspace *workspace, u64 index, StrBuf *message);

// opens an entry and makes it current, shown in the focused view.
// switching to an entry that was already open notes in message when its file changed on
// disk since it was loaded or saved.
// returns false, leaving the current entry alone, when it cannot be opened.
bool
WorkspaceSwitch(Workspace *workspace, u64 index, StrBuf *message);

// splits the window, showing an entry in a new view on the right that takes focus.
// returns false, leaving the window alone, when the entry cannot be opened.
bool
WorkspaceSplit(Workspace *workspace, u64 index, StrBuf *message);

// moves focus to the other view of the split.
void
WorkspaceFocusOther(Workspace *workspace);

// closes the focused view of the split, leaving the other one.
void
WorkspaceCloseView(Workspace *workspace);

// the view the unfocused side of the split shows.
View *
WorkspaceOtherView(const Workspace *workspace);

// the current entry's document.
Document *
WorkspaceDocument(const Workspace *workspace);

// an entry's path, empty for an unnamed buffer.
const StrBuf *
WorkspacePath(const Workspace *workspace, u64 index);

// appends an entry's display name: its path, or "[unnamed N]".
void
WorkspaceName(const Workspace *workspace, u64 index, StrBuf *name);

// true when an entry is open and its text differs from its file.
bool
WorkspaceModified(const Workspace *workspace, u64 index);

u64
WorkspaceModifiedCount(const Workspace *workspace);

// the next open entry after the current one (or before it when step is negative),
// wrapping around, or workspaceNone when no other entry is open.
u64
WorkspaceNextOpen(const Workspace *workspace, i32 step);

// the most recently used entry other than the current one, or workspaceNone.
u64
WorkspacePreviousUsed(const Workspace *workspace);
