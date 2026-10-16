#include "session.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "complete.h"
#include "fatal.h"
#include "loader.h"
#include "memory.h"
#include "picker.h"
#include "project.h"
#include "prompt.h"
#include "save.h"
#include "settings.h"
#include "symbols.h"

static Workspace *workspace;
static StrBuf *message;

// a folder opened with Ctrl-O, waiting for the answer about unsaved changes, and its path.
static LoaderList pending;
static StrBuf pendingFolder;

// a list of paths, each allocated on its own.
typedef struct Paths {
  char **paths;
  u64 count;
  u64 capacity;
} Paths;

// the folders the buffers were loaded from, as given, which Alt-Shift-R walks again.
static Paths folders;

// every path the last walk of those folders found, sorted, so Alt-Shift-R only adds files
// that are new to the folders, not ones closed since.
static Paths known;

// what runs once the unsaved changes are saved or discarded.
static SaveFinished afterUnsaved;

// the buffer being closed, waiting for the answer about its unsaved changes.
static u64 closing;

static void
PathsFree(Paths *paths)
{
  for (u64 index = 0; index < paths->count; index++) free(paths->paths[index]);
  free(paths->paths);
  *paths = (Paths){};
}

static void
PathsAdd(Paths *paths, const char *path, u64 length)
{
  if (paths->count == paths->capacity) {
    paths->capacity = paths->capacity ? paths->capacity * 2 : 64;
    paths->paths = MemoryResize(paths->paths, paths->capacity, sizeof(char *));
  }

  char *copy = MemoryAllocate(length + 1);
  memcpy(copy, path, length);
  copy[length] = 0;
  paths->paths[paths->count++] = copy;
}

static i32
ComparePaths(const void *a, const void *b)
{
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void
PathsSort(Paths *paths)
{
  if (paths->count) qsort(paths->paths, paths->count, sizeof(char *), ComparePaths);
}

static bool
PathsHas(const Paths *paths, const char *path)
{
  if (!paths->count) return false;
  return bsearch(&path, paths->paths, paths->count, sizeof(char *), ComparePaths);
}

// remembers which of paths are folders, and every file of list when there are any.
static void
RememberFolders(char *const *paths, u64 pathCount, const LoaderList *list)
{
  PathsFree(&folders);
  PathsFree(&known);

  for (u64 index = 0; index < pathCount; index++) {
    struct stat status;
    bool folder = stat(paths[index], &status) == 0 && S_ISDIR(status.st_mode);
    if (folder) PathsAdd(&folders, paths[index], strlen(paths[index]));
  }

  if (!folders.count) return;

  for (u64 index = 0; index < list->count; index++) {
    const StrBuf *path = &list->files[index].path;
    PathsAdd(&known, path->data, path->length);
  }

  PathsSort(&known);
}

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

    // the message may already say something (a bad language package).
    if (ok && message->length && opened.length) StrBufAppendString(message, "; ");
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
  RememberFolders(paths, pathCount, &list);
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
  ProjectClear();
  SymbolsClear();
  RememberFolders(&pendingFolder.data, 1, &pending);
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
    StrBufClear(&pendingFolder);
    StrBufAppend(&pendingFolder, path.data, path.length);
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

// true when the file is one of the loaded entries, by file or by path (a save replaces
// the file, so its file id changes).
static bool
Loaded(const LoaderFile *file)
{
  LoaderFile byPath = {.path = file->path};
  if (WorkspaceFind(workspace, file) != workspaceNone) return true;
  return WorkspaceFind(workspace, &byPath) != workspaceNone;
}

void
SessionLoadNew(void)
{
  if (!folders.count) {
    StrBufAppendString(message, "no folder is open");
    return;
  }

  // a folder that is gone would be listed as a new file, so it is left out.
  char **present = MemoryAllocate(folders.count * sizeof(char *));
  u64 count = 0;

  for (u64 index = 0; index < folders.count; index++) {
    struct stat status;
    char *folder = folders.paths[index];
    if (stat(folder, &status) == 0 && S_ISDIR(status.st_mode)) {
      present[count++] = folder;
      continue;
    }

    if (message->length) StrBufAppendString(message, "; ");
    StrBufAppendFormat(message, "%s: folder not found", folder);
  }

  if (!count) {
    free(present);
    return;
  }

  LoaderList list = {};
  StrBuf error = {};
  enum LoaderStatus status = LoaderLoad(&list, present, count, maxFileCount, &error);
  free(present);

  if (status != LoaderStatusOk) {
    if (status == LoaderStatusTooMany) {
      StrBufAppendFormat(message, "more than %lu files to load", maxFileCount);
    } else {
      StrBufAppend(message, error.data, error.length);
    }

    StrBufFree(&error);
    LoaderFree(&list);
    return;
  }

  // the walk becomes the known list, apart from new files there was no room for, so a
  // later Alt-Shift-R offers them again.
  Paths found = {};
  u64 added = 0;
  u64 first = workspaceNone;
  bool full = false;

  for (u64 index = 0; index < list.count; index++) {
    LoaderFile *file = &list.files[index];
    bool fresh = !PathsHas(&known, file->path.data) && !Loaded(file);

    if (fresh && !full && Full()) full = true;
    if (fresh && full) continue;

    PathsAdd(&found, file->path.data, file->path.length);
    if (!fresh) continue;

    u64 entry = WorkspaceAdd(workspace, file);
    if (first == workspaceNone) first = entry;
    added++;
  }

  PathsFree(&known);
  known = found;
  PathsSort(&known);

  if (message->length && (added || !full)) StrBufAppendString(message, "; ");
  if (!added) {
    if (!full) StrBufAppendString(message, "no new files");
  } else if (added == 1) {
    const StrBuf *path = WorkspacePath(workspace, first);
    StrBufAppendFormat(message, "added %.*s", (i32)path->length, path->data);
  } else {
    StrBufAppendFormat(message, "added %lu files (ctrl+p lists them)", added);
  }

  ReportUnreadable(list.unreadable);
  StrBufFree(&error);
  LoaderFree(&list);
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

static void
Close(void)
{
  u64 index = WorkspaceEntryOf(workspace, closing);
  if (index == workspaceNone) return;

  StrBuf name = {};
  WorkspaceName(workspace, index, &name);
  WorkspaceRemove(workspace, index, message);
  if (message->length) StrBufAppendString(message, "; ");
  StrBufAppendFormat(message, "closed %s", name.data);
  StrBufFree(&name);
}

static void
CloseAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
) {
  if (!answer || *answer == 'c') return;

  if (*answer == 'y') SaveCurrent(workspace, message, Close);
  else Close();
}

void
SessionClose(void)
{
  closing = workspace->entries[workspace->current].serial;

  if (!WorkspaceModified(workspace, workspace->current)) {
    Close();
    return;
  }

  StrBuf question = {};
  StrBufAppendString(&question, "save changes to ");
  WorkspaceName(workspace, workspace->current, &question);
  StrBufAppendString(&question, "? (y/n/c)");
  PromptChoose(question.data, "ync", CloseAnswered, nullptr);
  StrBufFree(&question);
}

void
SessionSplit(bool fresh, i32 width)
{
  if (workspace->split) {
    StrBufAppendString(message, "the window is already split");
    return;
  }

  if (width < minSplitWidth) {
    const char *text = "a split needs a window at least %d columns wide";
    StrBufAppendFormat(message, text, minSplitWidth);
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
