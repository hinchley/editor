#include "workspace.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "settings.h"

void
WorkspaceInit(Workspace *workspace, const DisplayOptions *options)
{
  *workspace = (Workspace){.options = options, .generation = 1};
}

// frees what an entry owns, returning true when it was open.
static bool
FreeEntry(WorkspaceEntry *entry)
{
  StrBufFree(&entry->path);
  StrBufFree(&entry->name);
  StrBufFree(&entry->real);
  StrBufFree(&entry->namedPath);
  if (!entry->document) return false;

  DocumentFree(entry->document);
  free(entry->document);
  return true;
}

static void
FreeFolders(Workspace *workspace)
{
  for (u64 index = 0; index < workspace->folderCount; index++) {
    StrBufFree(&workspace->folders[index].real);
    StrBufFree(&workspace->folders[index].name);
  }

  free(workspace->folders);
  workspace->folders = nullptr;
  workspace->folderCount = 0;
}

void
WorkspaceFree(Workspace *workspace)
{
  for (u64 index = 0; index < workspace->count; index++) FreeEntry(&workspace->entries[index]);
  free(workspace->entries);
  FreeFolders(workspace);
  u64 serials = workspace->serials;
  WorkspaceInit(workspace, workspace->options);
  workspace->serials = serials;
}

static WorkspaceEntry *
Append(Workspace *workspace)
{
  if (workspace->count == workspace->capacity) {
    workspace->capacity = workspace->capacity ? workspace->capacity * 2 : 64;
    u64 capacity = workspace->capacity;
    workspace->entries = MemoryResize(workspace->entries, capacity, sizeof(WorkspaceEntry));
  }

  WorkspaceEntry *entry = &workspace->entries[workspace->count++];
  *entry = (WorkspaceEntry){.serial = ++workspace->serials};
  return entry;
}

static void
AppendFile(Workspace *workspace, LoaderFile *file)
{
  WorkspaceEntry *entry = Append(workspace);
  entry->path = file->path;
  entry->id = file->id;
  file->path = (StrBuf){};
}

void
WorkspaceAddList(Workspace *workspace, LoaderList *list)
{
  for (u64 index = 0; index < list->count; index++) AppendFile(workspace, &list->files[index]);
  LoaderFree(list);
}

static const StrBuf *
EntryPath(const WorkspaceEntry *entry)
{
  return entry->document ? &entry->document->file.path : &entry->path;
}

// the file an entry is, including one that a save has created since it was loaded.
static FileId
EntryId(const WorkspaceEntry *entry)
{
  if (entry->id.inode || !entry->document) return entry->id;

  const FileStamp *stamp = &entry->document->file.stamp;
  return stamp->exists ? (FileId){stamp->device, stamp->inode} : (FileId){};
}

static bool
SameFile(const WorkspaceEntry *entry, const LoaderFile *file)
{
  FileId id = EntryId(entry);
  if (file->id.inode) return id.device == file->id.device && id.inode == file->id.inode;

  const StrBuf *path = EntryPath(entry);
  if (path->length != file->path.length || !path->length) return false;
  return memcmp(path->data, file->path.data, path->length) == 0;
}

u64
WorkspaceFind(const Workspace *workspace, const LoaderFile *file)
{
  for (u64 index = 0; index < workspace->count; index++) {
    if (SameFile(&workspace->entries[index], file)) return index;
  }

  return workspaceNone;
}

u64
WorkspaceLoad(Workspace *workspace, LoaderFile *file, StrBuf *message)
{
  u64 index = WorkspaceFind(workspace, file);
  if (index != workspaceNone) return index;

  if (workspace->count >= maxFileCount) {
    StrBufAppendFormat(message, "cannot load more than %lu files", maxFileCount);
    return workspaceNone;
  }

  return WorkspaceAdd(workspace, file);
}

u64
WorkspaceLoadPath(Workspace *workspace, const char *path, StrBuf *message)
{
  StrBuf copy = {};
  StrBufAppendString(&copy, path);
  LoaderList list = {};
  enum LoaderStatus status = LoaderLoad(&list, &copy.data, 1, maxFileCount, message);
  u64 index = workspaceNone;

  if (status == LoaderStatusOk && list.folders) {
    StrBufAppendFormat(message, "%s is a folder", path);
  } else if (status == LoaderStatusOk && list.count) {
    index = WorkspaceLoad(workspace, &list.files[0], message);
  }

  LoaderFree(&list);
  StrBufFree(&copy);
  return index;
}

u64
WorkspaceAdd(Workspace *workspace, LoaderFile *file)
{
  AppendFile(workspace, file);
  return workspace->count - 1;
}

u64
WorkspaceAddUnnamed(Workspace *workspace)
{
  WorkspaceEntry *entry = Append(workspace);
  entry->unnamed = ++workspace->unnamedCount;
  return workspace->count - 1;
}

u64
WorkspaceEntryOf(const Workspace *workspace, u64 serial)
{
  u64 low = 0;
  u64 high = workspace->count;

  while (low < high) {
    u64 middle = low + (high - low) / 2;
    u64 found = workspace->entries[middle].serial;
    if (found == serial) return middle;

    if (found < serial) low = middle + 1;
    else high = middle;
  }

  return workspaceNone;
}

void
WorkspaceRenamed(Workspace *workspace, u64 index)
{
  WorkspaceEntry *entry = &workspace->entries[index];
  entry->id = (FileId){};
  entry->id = EntryId(entry);
}

static void
DescribeRefusal(const WorkspaceEntry *entry, StrBuf *message)
{
  const char *reason = entry->refusal == WorkspaceRefusalBinary ? "binary file" : "too large";
  StrBufAppendFormat(message, "%s: %s, not opened", entry->path.data, reason);
}

bool
WorkspaceOpen(Workspace *workspace, u64 index, StrBuf *message)
{
  WorkspaceEntry *entry = &workspace->entries[index];
  if (entry->document) return true;

  if (entry->refusal != WorkspaceRefusalNone) {
    DescribeRefusal(entry, message);
    return false;
  }

  Document *document = MemoryAllocate(sizeof(Document));
  const char *path = entry->unnamed ? nullptr : entry->path.data;
  enum FileStatus status = DocumentOpen(document, path, workspace->options, message);

  if (status != FileStatusOk && status != FileStatusNew) {
    // size and content are settled once; other errors may pass, so they are tried again.
    if (status == FileStatusTooLarge) entry->refusal = WorkspaceRefusalTooLarge;
    if (status == FileStatusBinary) entry->refusal = WorkspaceRefusalBinary;
    DocumentFree(document);
    free(document);
    return false;
  }

  // the document now holds the path.
  StrBufFree(&entry->path);
  entry->document = document;
  workspace->openCount++;
  return true;
}

// makes an open entry current, in the focused view.
static void
MakeCurrent(Workspace *workspace, u64 index)
{
  workspace->current = index;
  workspace->entries[index].used = ++workspace->clock;
}

bool
WorkspaceSwitch(Workspace *workspace, u64 index, StrBuf *message)
{
  bool wasOpen = workspace->entries[index].document != nullptr;
  if (!WorkspaceOpen(workspace, index, message)) return false;

  const Document *document = workspace->entries[index].document;
  MakeCurrent(workspace, index);
  DocumentShow(workspace->entries[index].document, workspace->slot);

  // a commit made since the entry was last shown changes what its text is compared with.
  if (wasOpen) ChangesRefresh(document->changes);

  if (wasOpen && document->file.path.length && FileChangedOnDisk(&document->file)) {
    if (message->length) StrBufAppendString(message, "; ");
    const char *path = document->file.path.data;
    StrBufAppendFormat(message, "%s: changed on disk (alt+r reloads)", path);
  }

  return true;
}

bool
WorkspaceSplit(Workspace *workspace, u64 index, StrBuf *message)
{
  if (!WorkspaceOpen(workspace, index, message)) return false;

  workspace->split = true;
  workspace->other = workspace->current;
  workspace->left = workspace->slot;
  workspace->slot ^= 1;
  return WorkspaceSwitch(workspace, index, message);
}

void
WorkspaceFocusOther(Workspace *workspace)
{
  u64 index = workspace->other;
  workspace->other = workspace->current;
  workspace->slot ^= 1;
  MakeCurrent(workspace, index);
  DocumentFocus(workspace->entries[index].document, workspace->slot);
}

void
WorkspaceCloseView(Workspace *workspace)
{
  WorkspaceFocusOther(workspace);
  workspace->split = false;
}

View *
WorkspaceOtherView(const Workspace *workspace)
{
  return &workspace->entries[workspace->other].document->views[workspace->slot ^ 1];
}

Document *
WorkspaceDocument(const Workspace *workspace)
{
  return workspace->entries[workspace->current].document;
}

const StrBuf *
WorkspacePath(const Workspace *workspace, u64 index)
{
  return EntryPath(&workspace->entries[index]);
}

// the length of a path without its trailing slashes, so "/" is empty.
static u64
TrimmedLength(const char *path)
{
  u64 length = strlen(path);
  while (length && path[length - 1] == '/') length--;
  return length;
}

// true when path is below the folder of length bytes (without its trailing slash).
static bool
Below(const char *path, const char *folder, u64 length)
{
  return strncmp(path, folder, length) == 0 && path[length] == '/';
}

// the folder of the last path resolved, as written and resolved (empty when it did not
// resolve), since the files of one folder are usually named one after another.
static StrBuf lastParent;
static StrBuf lastReal;
static bool lastResolved;

// sets real to path with its folder resolved, or empties it when that does not resolve.
static void
Resolve(const char *path, StrBuf *real)
{
  const char *slash = strrchr(path, '/');
  const char *parent = slash ? path : ".";
  u64 parentLength = !slash ? 1 : slash == path ? 1 : (u64)(slash - path);
  StrBufClear(real);

  bool same = lastParent.length == parentLength &&
    memcmp(lastParent.data, parent, parentLength) == 0;

  if (!same) {
    StrBufClear(&lastParent);
    StrBufAppend(&lastParent, parent, parentLength);
    StrBufClear(&lastReal);
    char resolved[PATH_MAX];
    lastResolved = realpath(lastParent.data, resolved) != nullptr;
    if (lastResolved) StrBufAppend(&lastReal, resolved, TrimmedLength(resolved));
  }

  if (!lastResolved) return;

  StrBufAppend(real, lastReal.data, lastReal.length);
  StrBufAppendFormat(real, "/%s", slash ? slash + 1 : path);
}

// the start of the last count components of a folder's resolved path.
static u64
Components(const StrBuf *real, u32 count)
{
  u64 start = real->length;

  while (start && count) {
    start--;
    if (real->data[start] == '/') count--;
  }

  return count ? 0 : start + 1;
}

// names each folder by its last component, adding more to those whose names are still
// the same as another's until they differ or run out.
static void
NameFolders(Workspace *workspace)
{
  u64 count = workspace->folderCount;
  u32 *depths = MemoryAllocate((count + 1) * sizeof(u32));
  for (u64 index = 0; index < count; index++) depths[index] = 1;

  for (bool clashed = true; clashed;) {
    clashed = false;

    for (u64 index = 0; index < count; index++) {
      WorkspaceFolder *folder = &workspace->folders[index];
      u64 start = Components(&folder->real, depths[index]);
      StrBufClear(&folder->name);
      if (start < folder->real.length) {
        StrBufAppend(&folder->name, folder->real.data + start, folder->real.length - start);
      }
    }

    for (u64 index = 0; index < count; index++) {
      const WorkspaceFolder *folder = &workspace->folders[index];
      if (!Components(&folder->real, depths[index])) continue;

      for (u64 other = 0; other < count; other++) {
        const StrBuf *name = &workspace->folders[other].name;
        if (other == index || name->length != folder->name.length) continue;
        if (memcmp(name->data, folder->name.data, name->length) != 0) continue;

        depths[index]++;
        clashed = true;
        break;
      }
    }
  }

  free(depths);
}

void
WorkspaceSetFolders(Workspace *workspace, char *const *paths, u64 count)
{
  FreeFolders(workspace);
  workspace->folders = MemoryAllocate((count + 1) * sizeof(WorkspaceFolder));
  workspace->generation++;

  // a folder may have changed on disk since the last one was resolved.
  StrBufClear(&lastParent);

  for (u64 index = 0; index < count; index++) {
    char real[PATH_MAX];
    if (!realpath(paths[index], real)) continue;

    WorkspaceFolder *folder = &workspace->folders[workspace->folderCount++];
    *folder = (WorkspaceFolder){};
    StrBufAppend(&folder->real, real, TrimmedLength(real));
  }

  NameFolders(workspace);
}

// works out an entry's name and resolved path for its path and the open folders.
static void
NameEntry(const Workspace *workspace, WorkspaceEntry *entry)
{
  const StrBuf *path = EntryPath(entry);
  StrBufClear(&entry->name);
  StrBufClear(&entry->namedPath);
  StrBufAppend(&entry->namedPath, path->data, path->length);
  entry->generation = workspace->generation;

  if (!path->length) {
    StrBufClear(&entry->real);
    StrBufAppendFormat(&entry->name, "[unnamed %u]", entry->unnamed);
    return;
  }

  Resolve(path->data, &entry->real);
  const StrBuf *real = &entry->real;

  if (!real->length) {
    StrBufAppend(&entry->name, path->data, path->length);
    return;
  }

  // the deepest folder holding the file names it, when folders nest.
  const WorkspaceFolder *holder = nullptr;

  for (u64 index = 0; index < workspace->folderCount; index++) {
    const WorkspaceFolder *folder = &workspace->folders[index];
    if (!Below(real->data, folder->real.data, folder->real.length)) continue;
    if (!holder || folder->real.length > holder->real.length) holder = folder;
  }

  if (holder) {
    StrBufAppend(&entry->name, holder->name.data, holder->name.length);
    u64 inside = holder->real.length;
    StrBufAppend(&entry->name, real->data + inside, real->length - inside);
    return;
  }

  const char *home = getenv("HOME");
  u64 homeLength = home ? TrimmedLength(home) : 0;

  if (homeLength && Below(real->data, home, homeLength)) {
    StrBufAppendFormat(&entry->name, "~%s", real->data + homeLength);
    return;
  }

  StrBufAppend(&entry->name, real->data, real->length);
}

// the entry at index with its name up to date.
// the name is a cache, so it is worked out again even through a const workspace.
static const WorkspaceEntry *
Named(const Workspace *workspace, u64 index)
{
  WorkspaceEntry *entry = &workspace->entries[index];
  const StrBuf *path = EntryPath(entry);

  bool stale = entry->generation != workspace->generation ||
    entry->namedPath.length != path->length ||
    (path->length && memcmp(entry->namedPath.data, path->data, path->length) != 0);

  if (stale) NameEntry(workspace, entry);
  return entry;
}

void
WorkspaceName(const Workspace *workspace, u64 index, StrBuf *name)
{
  const StrBuf *shown = WorkspaceShownName(workspace, index);
  StrBufAppend(name, shown->data, shown->length);
}

const StrBuf *
WorkspaceShownName(const Workspace *workspace, u64 index)
{
  return &Named(workspace, index)->name;
}

const StrBuf *
WorkspaceRealPath(const Workspace *workspace, u64 index)
{
  return &Named(workspace, index)->real;
}

bool
WorkspaceModified(const Workspace *workspace, u64 index)
{
  const Document *document = workspace->entries[index].document;
  return document && DocumentModified(document);
}

u64
WorkspaceModifiedCount(const Workspace *workspace)
{
  u64 count = 0;

  for (u64 index = 0; index < workspace->count; index++) {
    count += WorkspaceModified(workspace, index);
  }

  return count;
}

u64
WorkspaceNextOpen(const Workspace *workspace, i32 step)
{
  u64 count = workspace->count;
  u64 index = workspace->current;

  for (u64 tried = 1; tried < count; tried++) {
    index = step > 0 ? (index + 1) % count : (index + count - 1) % count;
    if (workspace->entries[index].document) return index;
  }

  return workspaceNone;
}

// the most recently used entry other than skip, or workspaceNone.
static u64
MostRecent(const Workspace *workspace, u64 skip)
{
  u64 found = workspaceNone;
  u64 latest = 0;

  for (u64 index = 0; index < workspace->count; index++) {
    u64 used = workspace->entries[index].used;
    if (index == skip || used <= latest) continue;

    latest = used;
    found = index;
  }

  return found;
}

u64
WorkspacePreviousUsed(const Workspace *workspace)
{
  return MostRecent(workspace, workspace->current);
}

// makes another entry current in place of a removed one: the most recently used (which
// is open, since only an open entry is ever used), or else a new unnamed buffer, so no
// file is read that the user did not ask for.
static void
ReplaceCurrent(Workspace *workspace, StrBuf *message)
{
  u64 index = MostRecent(workspace, workspaceNone);
  if (index != workspaceNone && WorkspaceSwitch(workspace, index, message)) return;

  WorkspaceSwitch(workspace, WorkspaceAddUnnamed(workspace), message);
}

// shows another entry on the unfocused side of the split in place of a removed one,
// starting where it was last seen.
static void
ReplaceOther(Workspace *workspace)
{
  u64 index = MostRecent(workspace, workspace->current);
  if (index == workspaceNone) index = workspace->current;

  Document *document = workspace->entries[index].document;
  View *view = &document->views[workspace->slot ^ 1];
  if (view != document->view) ViewCopy(view, document->view);
  workspace->other = index;
}

void
WorkspaceRemoveSome(Workspace *workspace, const u64 *serials, u64 count, StrBuf *message)
{
  bool wasCurrent = false;
  bool wasOther = false;
  u64 kept = 0;
  u64 next = 0;

  // one pass keeps the rest in order, however many go.
  for (u64 index = 0; index < workspace->count; index++) {
    WorkspaceEntry *entry = &workspace->entries[index];
    while (next < count && serials[next] < entry->serial) next++;

    if (next == count || entry->serial != serials[next]) {
      if (index == workspace->current) workspace->current = kept;
      if (index == workspace->other) workspace->other = kept;
      workspace->entries[kept++] = *entry;
      continue;
    }

    next++;
    if (index == workspace->current) wasCurrent = true;
    if (workspace->split && index == workspace->other) wasOther = true;
    if (FreeEntry(entry)) workspace->openCount--;
  }

  workspace->count = kept;
  if (wasCurrent) ReplaceCurrent(workspace, message);
  if (wasOther) ReplaceOther(workspace);
}

void
WorkspaceRemove(Workspace *workspace, u64 index, StrBuf *message)
{
  u64 serial = workspace->entries[index].serial;
  WorkspaceRemoveSome(workspace, &serial, 1, message);
}
