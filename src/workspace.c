#include "workspace.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "settings.h"

void
WorkspaceInit(Workspace *workspace, const DisplayOptions *options)
{
  *workspace = (Workspace){.options = options};
}

void
WorkspaceFree(Workspace *workspace)
{
  for (u64 index = 0; index < workspace->count; index++) {
    WorkspaceEntry *entry = &workspace->entries[index];
    StrBufFree(&entry->path);
    if (!entry->document) continue;

    DocumentFree(entry->document);
    free(entry->document);
  }

  free(workspace->entries);
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

Document *
WorkspaceEditBegin(Workspace *workspace, u64 index)
{
  StrBuf ignored = {};
  bool opened = index != workspaceNone && WorkspaceOpen(workspace, index, &ignored);
  StrBufFree(&ignored);
  if (!opened) return nullptr;

  Document *document = workspace->entries[index].document;
  UndoBreak(&document->undo);
  UndoBegin(&document->undo, UndoKindOther, &document->view->cursors);
  return document;
}

void
WorkspaceEditEnd(Document *document)
{
  UndoEnd(&document->undo, &document->view->cursors);
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

void
WorkspaceName(const Workspace *workspace, u64 index, StrBuf *name)
{
  const WorkspaceEntry *entry = &workspace->entries[index];
  const StrBuf *path = EntryPath(entry);

  if (path->length) StrBufAppend(name, path->data, path->length);
  else StrBufAppendFormat(name, "[unnamed %u]", entry->unnamed);
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

// makes another entry current in place of a removed one: the most recently used, or else
// the first that opens, or else a new unnamed buffer.
static void
ReplaceCurrent(Workspace *workspace, StrBuf *message)
{
  u64 index = MostRecent(workspace, workspaceNone);
  if (index != workspaceNone && WorkspaceSwitch(workspace, index, message)) return;

  for (index = 0; index < workspace->count; index++) {
    StrBuf reason = {};
    bool opened = WorkspaceSwitch(workspace, index, &reason);
    StrBufFree(&reason);
    if (opened) return;
  }

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
WorkspaceRemove(Workspace *workspace, u64 index, StrBuf *message)
{
  WorkspaceEntry *entry = &workspace->entries[index];
  bool wasCurrent = index == workspace->current;
  bool wasOther = workspace->split && index == workspace->other;

  StrBufFree(&entry->path);

  if (entry->document) {
    DocumentFree(entry->document);
    free(entry->document);
    workspace->openCount--;
  }

  u64 after = workspace->count - index - 1;
  memmove(entry, entry + 1, after * sizeof(WorkspaceEntry));
  workspace->count--;
  if (workspace->current > index) workspace->current--;
  if (workspace->other > index) workspace->other--;

  if (wasCurrent) ReplaceCurrent(workspace, message);
  if (wasOther) ReplaceOther(workspace);
}
