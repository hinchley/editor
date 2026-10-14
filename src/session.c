#include "session.h"

#include <stdlib.h>
#include <string.h>

#include "complete.h"
#include "fatal.h"
#include "loader.h"
#include "memory.h"
#include "picker.h"
#include "prompt.h"
#include "save.h"
#include "settings.h"

static Workspace *workspace;
static StrBuf *message;

// a folder opened with Ctrl-O, waiting for the answer about unsaved changes.
static LoaderList pending;

// what runs once the unsaved changes are saved or discarded.
static SaveFinished afterUnsaved;

static void
Switch(u64 index)
{
  WorkspaceSwitch(workspace, index, message);
}

static void
ReportUnreadable(u64 count)
{
  if (!count) return;

  if (message->length) StrBufAppendString(message, "; ");
  StrBufAppendFormat(message, "%lu folder%s could not be read", count, count == 1 ? "" : "s");
}

// makes the first entry that opens current, or a new unnamed buffer when none does.
// the reasons entries were refused only matter when explain is set, since with a list to
// pick from the user sees them when picking one.
static void
SwitchToFirst(bool explain)
{
  StrBuf reasons = {};

  for (u64 index = 0; index < workspace->count; index++) {
    StrBuf opened = {};
    bool ok = WorkspaceSwitch(workspace, index, &opened);
    StrBufAppend(ok ? message : &reasons, opened.data ? opened.data : "", opened.length);
    StrBufFree(&opened);

    if (ok) {
      StrBufFree(&reasons);
      return;
    }
  }

  if (explain) StrBufAppend(message, reasons.data ? reasons.data : "", reasons.length);
  else if (workspace->count) StrBufAppendString(message, "none of the files could be opened");
  StrBufFree(&reasons);
  Switch(WorkspaceAddUnnamed(workspace));
}

void
SessionStart(Workspace *newWorkspace, StrBuf *newMessage, char *const *paths, u64 pathCount)
{
  workspace = newWorkspace;
  message = newMessage;

  if (!pathCount) {
    Switch(WorkspaceAddUnnamed(workspace));
    return;
  }

  LoaderList list = {};
  StrBuf error = {};
  enum LoaderStatus status = LoaderLoad(&list, paths, pathCount, maxFileCount, &error);
  if (status == LoaderStatusTooMany) Fatal("more than %lu files to load", maxFileCount);
  if (status == LoaderStatusError) Fatal("%s", error.data);
  StrBufFree(&error);

  bool pick = pathCount > 1 || list.folders;
  bool empty = !list.count;
  u64 unreadable = list.unreadable;
  WorkspaceAddList(workspace, &list);
  SwitchToFirst(!pick);
  if (empty) StrBufAppendString(message, "no files to load");
  ReportUnreadable(unreadable);
  if (pick && !empty) SessionPick(false);
}

// true, with a message, when there is no room for another buffer.
static bool
Full(void)
{
  if (workspace->count < maxFileCount) return false;

  StrBufAppendFormat(message, "cannot load more than %lu files", maxFileCount);
  return true;
}

void
SessionNew(void)
{
  if (!Full()) Switch(WorkspaceAddUnnamed(workspace));
}

static void
Discarded(void)
{
  if (afterUnsaved) afterUnsaved();
}

static void
UnsavedAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
) {
  if (answer && *answer == 's') SaveAll(workspace, message, afterUnsaved);
  else if (answer && *answer == 'd') Discarded();
}

// runs then, after offering to save or discard any unsaved changes.
static void
AskAboutUnsaved(SaveFinished then)
{
  afterUnsaved = then;
  u64 count = WorkspaceModifiedCount(workspace);

  if (!count) {
    then();
    return;
  }

  StrBuf question = {};
  StrBufAppendFormat(&question, "%lu unsaved: (s)ave all, (d)iscard, (c)ancel", count);
  PromptChoose(question.data, "sdc", UnsavedAnswered, nullptr);
  StrBufFree(&question);
}

// replaces every buffer with the pending folder.
static void
ReplaceAll(void)
{
  u64 unreadable = pending.unreadable;
  WorkspaceFree(workspace);
  WorkspaceAddList(workspace, &pending);
  SwitchToFirst(false);
  ReportUnreadable(unreadable);
  SessionPick(false);
}

static void
OpenFile(LoaderFile *file)
{
  u64 index = WorkspaceFind(workspace, file);

  if (index == workspaceNone && Full()) return;
  if (index == workspaceNone) index = WorkspaceAdd(workspace, file);
  Switch(index);
}

static void
OpenAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer || !length) return;

  // the loader takes the same kind of list as the command line, after the shell would
  // have replaced "~".
  StrBuf path = {};
  CompleteExpandHome(answer, length, &path);
  LoaderList list = {};
  enum LoaderStatus status = LoaderLoad(&list, &path.data, 1, maxFileCount, message);

  if (status == LoaderStatusTooMany) {
    StrBufAppendFormat(message, "%s: more than %lu files to load", path.data, maxFileCount);
  }

  if (status == LoaderStatusOk && list.folders && !list.count) {
    StrBufAppendFormat(message, "%s: no files to load", path.data);
  } else if (status == LoaderStatusOk && list.folders) {
    LoaderFree(&pending);
    pending = list;
    list = (LoaderList){};
    AskAboutUnsaved(ReplaceAll);
  } else if (status == LoaderStatusOk) {
    OpenFile(&list.files[0]);
  }

  LoaderFree(&list);
  StrBufFree(&path);
}

static void
CompleteOpen([[maybe_unused]] void *context, StrBuf *text)
{
  CompletePath(text);
}

void
SessionOpen(void)
{
  PromptAsk("open: ", "", OpenAnswered, nullptr);
  PromptHandle(nullptr, CompleteOpen);
}

void
SessionCycle(i32 step)
{
  u64 index = WorkspaceNextOpen(workspace, step);
  if (index == workspaceNone) StrBufAppendString(message, "no other open buffer");
  else Switch(index);
}

void
SessionLastUsed(void)
{
  u64 index = WorkspacePreviousUsed(workspace);
  if (index == workspaceNone) StrBufAppendString(message, "no other buffer used yet");
  else Switch(index);
}

// most recently used first, then named buffers by path, then unnamed ones in order.
static i32
CompareEntries(const void *a, const void *b)
{
  const WorkspaceEntry *first = &workspace->entries[*(const u64 *)a];
  const WorkspaceEntry *second = &workspace->entries[*(const u64 *)b];

  if (first->used != second->used) return first->used > second->used ? -1 : 1;
  if (!first->unnamed != !second->unnamed) return first->unnamed ? 1 : -1;
  if (first->unnamed) return first->unnamed < second->unnamed ? -1 : 1;

  const StrBuf *firstPath = WorkspacePath(workspace, *(const u64 *)a);
  const StrBuf *secondPath = WorkspacePath(workspace, *(const u64 *)b);
  return strcmp(firstPath->data, secondPath->data);
}

static const char *
EntryNote(u64 index)
{
  const WorkspaceEntry *entry = &workspace->entries[index];
  if (entry->refusal == WorkspaceRefusalBinary) return "(binary)";
  if (entry->refusal == WorkspaceRefusalTooLarge) return "(too large)";
  return WorkspaceModified(workspace, index) ? "[+]" : nullptr;
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (picked) Switch(id);
}

void
SessionPick(bool unsavedOnly)
{
  u64 *order = MemoryAllocate((workspace->count + 1) * sizeof(u64));
  u64 count = 0;

  for (u64 index = 0; index < workspace->count; index++) {
    if (!unsavedOnly || WorkspaceModified(workspace, index)) order[count++] = index;
  }

  if (!count) {
    StrBufAppendString(message, "no buffers with unsaved changes");
    free(order);
    return;
  }

  qsort(order, count, sizeof(u64), CompareEntries);
  PickerBegin();
  StrBuf name = {};

  for (u64 index = 0; index < count; index++) {
    StrBufClear(&name);
    WorkspaceName(workspace, order[index], &name);
    PickerAdd(name.data, name.length, EntryNote(order[index]), order[index]);
  }

  StrBufFree(&name);
  free(order);
  PickerOpen(unsavedOnly ? "unsaved buffers" : "buffers", "buffer: ", Picked, nullptr);
}

void
SessionSplit(bool fresh, i32 width)
{
  if (workspace->split) {
    StrBufAppendString(message, "the window is already split");
    return;
  }

  if (width < minSplitWidth) {
    StrBufAppendFormat(message, "a split needs a window at least %d columns wide", minSplitWidth);
    return;
  }

  if (fresh && Full()) return;

  u64 index = fresh ? WorkspaceAddUnnamed(workspace) : workspace->current;
  WorkspaceSplit(workspace, index, message);
}

// true, with a message, when the window is not split.
static bool
Unsplit(void)
{
  if (workspace->split) return false;

  StrBufAppendString(message, "the window is not split");
  return true;
}

void
SessionFocusOther(void)
{
  if (!Unsplit()) WorkspaceFocusOther(workspace);
}

void
SessionSwapViews(void)
{
  if (!Unsplit()) workspace->left ^= 1;
}

void
SessionCloseView(void)
{
  if (!Unsplit()) WorkspaceCloseView(workspace);
}

void
SessionQuit(SessionQuitter quit)
{
  AskAboutUnsaved(quit);
}
