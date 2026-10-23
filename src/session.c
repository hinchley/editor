#include "session.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "complete.h"
#include "fatal.h"
#include "loader.h"
#include "lsp.h"
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

// the serial of the new unnamed buffer standing behind the buffer picker, which stays
// when the picker is cancelled and goes once another buffer is picked, or 0.
static u64 placeholder;

// the folder being closed, as given and by the name its files are shown under, and the
// serials of the entries inside it in list order, waiting for the answer about their
// unsaved changes.
static StrBuf closingFolder;
static StrBuf closingName;
static u64 *closingSerials;
static u64 closingCount;

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

// tells the workspace which folders are open, since they name the files inside them.
static void
ShareFolders(void)
{
  WorkspaceSetFolders(workspace, folders.paths, folders.count);
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

// most recently used first, then named buffers by name, then unnamed ones in order.
static i32
CompareEntries(const void *a, const void *b)
{
  const WorkspaceEntry *first = &workspace->entries[*(const u64 *)a];
  const WorkspaceEntry *second = &workspace->entries[*(const u64 *)b];

  if (first->used != second->used) return first->used > second->used ? -1 : 1;
  if (!first->unnamed != !second->unnamed) return first->unnamed ? 1 : -1;
  if (first->unnamed) return first->unnamed < second->unnamed ? -1 : 1;

  // by the names shown (brought up to date before sorting), so the files of each folder
  // come together.
  return strcmp(first->name.data, second->name.data);
}

static const char *
EntryNote(u64 index)
{
  const WorkspaceEntry *entry = &workspace->entries[index];
  if (entry->refusal == WorkspaceRefusalBinary) return "(binary)";
  if (entry->refusal == WorkspaceRefusalTooLarge) return "(too large)";
  return nullptr;
}

// "+" for an entry with unsaved changes (which is always open), "·" for any other open one,
// or nullptr.
static const char *
EntryMark(u64 index)
{
  if (WorkspaceModified(workspace, index)) return "+";
  return workspace->entries[index].document ? "\u00B7" : nullptr;
}

static void
Picked([[maybe_unused]] void *context, bool picked, u64 id)
{
  u64 standing = placeholder;
  placeholder = 0;
  if (!picked) return;

  Switch(id);
  if (!standing) return;

  // the empty buffer behind the picker goes, unless it is still shown or was edited.
  u64 index = WorkspaceEntryOf(workspace, standing);
  if (index == workspaceNone || index == workspace->current) return;
  if (workspace->split && index == workspace->other) return;
  if (!WorkspaceModified(workspace, index)) WorkspaceRemove(workspace, index, message);
}

// opens the buffer picker over every buffer, or only those with unsaved changes, with the
// entry at selected (or else the first) selected.
static void
Pick(bool unsavedOnly, u64 selected)
{
  u64 *order = MemoryAllocate((workspace->count + 1) * sizeof(u64));
  u64 count = 0;

  // the empty buffer standing behind the picker is what cancelling it leaves, so it is
  // not one of the choices.
  for (u64 index = 0; index < workspace->count; index++) {
    if (workspace->entries[index].serial == placeholder) continue;
    if (!unsavedOnly || WorkspaceModified(workspace, index)) order[count++] = index;
  }

  if (!count) {
    if (unsavedOnly) StrBufAppendString(message, "no buffers with unsaved changes");
    placeholder = 0;
    free(order);
    return;
  }

  for (u64 index = 0; index < count; index++) WorkspaceShownName(workspace, order[index]);
  qsort(order, count, sizeof(u64), CompareEntries);
  PickerBegin();
  StrBuf name = {};

  for (u64 index = 0; index < count; index++) {
    StrBufClear(&name);
    WorkspaceName(workspace, order[index], &name);
    PickerAdd(name.data, name.length, EntryNote(order[index]), order[index]);
    const char *mark = EntryMark(order[index]);
    if (mark) PickerMark(mark);
  }

  StrBufFree(&name);
  PickerOpen(unsavedOnly ? "unsaved buffers" : "buffers", "buffer: ", Picked, nullptr);

  for (u64 position = 0; position < count; position++) {
    if (order[position] == selected) PickerSelect(position);
  }

  free(order);
}

void
SessionPick(bool unsavedOnly)
{
  Pick(unsavedOnly, workspaceNone);
}

// after entries were removed: when the current one went, opens the buffer picker over
// the buffer that replaced it, which stays when the picker is cancelled (a new unnamed one
// made for it goes once another is picked).
// with nothing else to pick, the replacement is simply shown.
static void
PickAfterRemoving(u64 current, u64 serials)
{
  u64 replacement = workspace->entries[workspace->current].serial;
  if (replacement == current || workspace->count < 2) return;

  placeholder = replacement > serials ? replacement : 0;
  Pick(false, workspaceNone);
}

// makes a new unnamed buffer current and opens the buffer picker over it, so cancelling
// the picker leaves an empty buffer rather than a file nobody asked for.
static void
PickOverPlaceholder(void)
{
  u64 index = WorkspaceAddUnnamed(workspace);
  Switch(index);
  placeholder = workspace->entries[index].serial;
  Pick(false, workspaceNone);
}

// makes the first file named among paths current, as given on the command line, and
// returns true; false when every path is a folder or no named file opens.
static bool
SwitchToNamed(char *const *paths, u64 pathCount)
{
  for (u64 path = 0; path < pathCount; path++) {
    struct stat status;
    if (stat(paths[path], &status) == 0 && S_ISDIR(status.st_mode)) continue;

    for (u64 index = 0; index < workspace->count; index++) {
      if (strcmp(WorkspacePath(workspace, index)->data, paths[path]) != 0) continue;
      if (WorkspaceSwitch(workspace, index, message)) return true;
    }
  }

  return false;
}

static void
ReportUnreadable(u64 count)
{
  if (!count) return;

  if (message->length) StrBufAppendString(message, "; ");
  StrBufAppendFormat(message, "%lu folder%s could not be read", count, StrBufPlural(count));
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
  bool folder = list.folders;
  bool empty = !list.count;
  u64 unreadable = list.unreadable;
  RememberFolders(paths, pathCount, &list);
  ShareFolders();
  WorkspaceAddList(workspace, &list);
  ReportUnreadable(unreadable);

  // with a folder, the picker stands over a file named with it, or else an empty buffer.
  if (folder && !empty) {
    if (!SwitchToNamed(paths, pathCount)) {
      PickOverPlaceholder();
      return;
    }
  } else {
    SwitchToFirst(!pick);
  }

  if (empty) StrBufAppendString(message, "no files to load");
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
)
{
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
  ShareFolders();

  // the folder may have other roots, so servers start again as its files open.
  LspStopAll();
  WorkspaceAddList(workspace, &pending);
  ReportUnreadable(unreadable);
  PickOverPlaceholder();
}

static void
OpenFile(LoaderFile *file)
{
  u64 index = WorkspaceLoad(workspace, file, message);
  if (index != workspaceNone) Switch(index);
}

// true when path is one of the remembered folders, as given or once resolved.
static bool
FolderOpen(const char *path)
{
  char real[PATH_MAX];
  char other[PATH_MAX];
  bool resolved = realpath(path, real) != nullptr;

  for (u64 index = 0; index < folders.count; index++) {
    if (strcmp(folders.paths[index], path) == 0) return true;
    if (resolved && realpath(folders.paths[index], other) && !strcmp(real, other)) return true;
  }

  return false;
}

// adds the files of the pending folder that are not loaded yet, remembers the folder and
// opens the buffer picker with the first file added selected, leaving the current buffer
// as it is when the picker is cancelled.
// files there is no room for are left out of the known list, so Alt-Shift-R offers them
// again.
static void
AddPending(void)
{
  u64 added = 0;
  u64 first = workspaceNone;
  u64 left = 0;

  for (u64 index = 0; index < pending.count; index++) {
    LoaderFile *file = &pending.files[index];
    if (WorkspaceFind(workspace, file) != workspaceNone) continue;

    if (workspace->count >= maxFileCount) {
      left++;
      continue;
    }

    PathsAdd(&known, file->path.data, file->path.length);
    u64 entry = WorkspaceAdd(workspace, file);
    if (first == workspaceNone) first = entry;
    added++;
  }

  PathsSort(&known);
  const char *folder = pendingFolder.data;
  if (!FolderOpen(folder)) PathsAdd(&folders, folder, pendingFolder.length);
  ShareFolders();
  if (message->length) StrBufAppendString(message, "; ");

  if (!added && !left) {
    StrBufAppendFormat(message, "%s: no new files", folder);
  } else if (added == 1) {
    const StrBuf *path = WorkspacePath(workspace, first);
    StrBufAppendFormat(message, "added %.*s", (i32)path->length, path->data);
  } else if (added) {
    StrBufAppendFormat(message, "added %lu files from %s", added, folder);
  }

  if (left) {
    if (added) StrBufAppendString(message, "; ");
    const char *text = "cannot load more than %lu files (%lu left out)";
    StrBufAppendFormat(message, text, maxFileCount, left);
  }

  ReportUnreadable(pending.unreadable);
  LoaderFree(&pending);
  if (first != workspaceNone) Pick(false, first);
}

static void
FolderAnswered([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  if (answer && *answer == 'a') AddPending();
  else if (answer && *answer == 'r') AskAboutUnsaved(ReplaceAll);
}

// true when every entry is an unnamed buffer without changes, so replacing them loses
// nothing and there is nothing to add to.
static bool
Untouched(void)
{
  for (u64 index = 0; index < workspace->count; index++) {
    if (!workspace->entries[index].unnamed || WorkspaceModified(workspace, index)) {
      return false;
    }
  }

  return true;
}

// asks whether the pending folder is added to the buffers or replaces them, unless there
// is nothing to replace.
static void
AskAboutFolder(void)
{
  if (Untouched()) {
    ReplaceAll();
    return;
  }

  StrBuf question = {};
  const char *text = "%s: (a)dd, (r)eplace, (c)ancel";
  StrBufAppendFormat(&question, text, pendingFolder.data);
  PromptChoose(question.data, "arc", FolderAnswered, nullptr);
  StrBufFree(&question);
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
    AskAboutFolder();
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

const char *
SessionFolder(void)
{
  return folders.count == 1 ? folders.paths[0] : nullptr;
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
    bool fresh = !PathsHas(&known, file->path.data) &&
      WorkspaceFind(workspace, file) == workspaceNone;

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

static void
Close(void)
{
  u64 index = WorkspaceEntryOf(workspace, closing);
  if (index == workspaceNone) return;

  StrBuf name = {};
  WorkspaceName(workspace, index, &name);
  u64 serials = workspace->serials;
  WorkspaceRemove(workspace, index, message);
  if (message->length) StrBufAppendString(message, "; ");
  StrBufAppendFormat(message, "closed %s", name.data);
  StrBufFree(&name);
  PickAfterRemoving(closing, serials);
}

static void
CloseAnswered([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
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

// the length of a path without its trailing slashes, so "src/" and "src" name the same
// folder and "/" is empty.
static u64
FolderLength(const char *path)
{
  u64 length = strlen(path);
  while (length && path[length - 1] == '/') length--;
  return length;
}

// true when path is below the folder of length bytes (from FolderLength).
static bool
Below(const char *path, const char *folder, u64 length)
{
  return strncmp(path, folder, length) == 0 && path[length] == '/';
}

// a folder to find the entries inside: as given, and resolved when it resolves, both
// without trailing slashes.
typedef struct Folder {
  const char *given;
  u64 givenLength;
  bool resolved;
  StrBuf real;
} Folder;

static Folder
FolderOf(const char *path)
{
  Folder folder = {.given = path, .givenLength = FolderLength(path)};
  char real[PATH_MAX];
  folder.resolved = realpath(path, real) != nullptr;
  if (folder.resolved) StrBufAppend(&folder.real, real, FolderLength(real));
  return folder;
}

// appends the name the files of the folder at path are shown under, or the path itself
// when the folder does not resolve.
static void
FolderName(const char *path, StrBuf *name)
{
  Folder folder = FolderOf(path);

  for (u64 index = 0; folder.resolved && index < workspace->folderCount; index++) {
    const WorkspaceFolder *open = &workspace->folders[index];
    if (open->real.length != folder.real.length) continue;
    if (memcmp(open->real.data, folder.real.data, folder.real.length) != 0) continue;

    // the root has no name, since its files are shown by their whole paths.
    if (open->name.length) StrBufAppend(name, open->name.data, open->name.length);
    else StrBufAppendString(name, "/");
    StrBufFree(&folder.real);
    return;
  }

  StrBufAppendString(name, path);
  StrBufFree(&folder.real);
}

// true when the entry at index is a file inside the folder: by its path as loaded, which
// is how the folder's walk names its files, or else by where its folder resolves to, for
// a file opened by another path.
static bool
Inside(const Folder *folder, u64 index)
{
  const StrBuf *path = WorkspacePath(workspace, index);
  if (!path->length) return false;
  if (Below(path->data, folder->given, folder->givenLength)) return true;
  if (!folder->resolved) return false;

  const StrBuf *real = WorkspaceRealPath(workspace, index);
  return real->length && Below(real->data, folder->real.data, folder->real.length);
}

// closes the folder being closed: removes its entries and forgets it.
static void
CloseFolderNow(void)
{
  u64 index = 0;
  while (index < folders.count && strcmp(folders.paths[index], closingFolder.data)) index++;
  if (index == folders.count) return;

  u64 current = workspace->entries[workspace->current].serial;
  u64 serials = workspace->serials;
  WorkspaceRemoveSome(workspace, closingSerials, closingCount, message);
  free(folders.paths[index]);
  u64 after = folders.count - index - 1;
  memmove(&folders.paths[index], &folders.paths[index + 1], after * sizeof(char *));
  folders.count--;
  if (!folders.count) PathsFree(&known);
  ShareFolders();

  // the servers of the folder's files have nothing left to do.
  LspStopIdle();

  if (message->length) StrBufAppendString(message, "; ");
  StrBufAppendFormat(message, "closed %s", closingName.data);
  PickAfterRemoving(current, serials);
}

// the entries of the folder being closed, by index, in list order, returning how many.
static u64
ClosingEntries(u64 *entries)
{
  u64 count = 0;

  for (u64 index = 0; index < closingCount; index++) {
    u64 entry = WorkspaceEntryOf(workspace, closingSerials[index]);
    if (entry != workspaceNone) entries[count++] = entry;
  }

  return count;
}

static void
FolderUnsavedAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
)
{
  if (!answer || *answer == 'c') return;

  if (*answer == 'd') {
    CloseFolderNow();
    return;
  }

  u64 *entries = MemoryAllocate((closingCount + 1) * sizeof(u64));
  u64 count = ClosingEntries(entries);
  SaveSome(workspace, message, entries, count, CloseFolderNow);
  free(entries);
}

// closes the folder at index of folders, after offering to save the unsaved changes in it.
static void
CloseFolder(u64 index)
{
  StrBufClear(&closingFolder);
  StrBufAppendString(&closingFolder, folders.paths[index]);
  StrBufClear(&closingName);
  FolderName(folders.paths[index], &closingName);
  closingSerials = MemoryResize(closingSerials, workspace->count + 1, sizeof(u64));
  closingCount = 0;

  Folder folder = FolderOf(folders.paths[index]);
  u64 unsaved = 0;

  for (u64 entry = 0; entry < workspace->count; entry++) {
    if (!Inside(&folder, entry)) continue;

    closingSerials[closingCount++] = workspace->entries[entry].serial;
    unsaved += WorkspaceModified(workspace, entry);
  }

  StrBufFree(&folder.real);

  if (!unsaved) {
    CloseFolderNow();
    return;
  }

  StrBuf question = {};
  const char *text = "%lu unsaved in %s: (s)ave all, (d)iscard, (c)ancel";
  StrBufAppendFormat(&question, text, unsaved, closingName.data);
  PromptChoose(question.data, "sdc", FolderUnsavedAnswered, nullptr);
  StrBufFree(&question);
}

static void
FolderPicked([[maybe_unused]] void *context, bool picked, u64 id)
{
  if (picked) CloseFolder(id);
}

void
SessionCloseFolder(void)
{
  if (!folders.count) {
    StrBufAppendString(message, "no folder is open");
    return;
  }

  if (folders.count == 1) {
    CloseFolder(0);
    return;
  }

  // the folder holding the current buffer is selected, so enter closes it.
  PickerBegin();
  u64 selected = workspaceNone;
  StrBuf name = {};

  for (u64 index = 0; index < folders.count; index++) {
    const char *path = folders.paths[index];
    StrBufClear(&name);
    FolderName(path, &name);
    PickerAdd(name.data, name.length, nullptr, index);
    if (selected != workspaceNone) continue;

    Folder folder = FolderOf(path);
    if (Inside(&folder, workspace->current)) selected = index;
    StrBufFree(&folder.real);
  }

  StrBufFree(&name);
  PickerOpen("folders", "close folder: ", FolderPicked, nullptr);
  if (selected != workspaceNone) PickerSelect(selected);
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
