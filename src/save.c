#include "save.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "complete.h"
#include "edit.h"
#include "memory.h"
#include "prompt.h"
#include "settings.h"

// one run of saves: the serials of the entries still to save and what to do once they
// are.
static SaveFinished afterSave;
static u64 *queue;
static u64 queueCount;
static u64 queueCapacity;
static u64 position;
static bool saveAll;

// the document being saved had no name before the save as prompt gave it one.
static bool namedForSave;

// the file menu: the entry it acts on, what was chosen ('r', 'c' or 'd') and the path
// given for a rename or copy.
static u64 menuSerial;
static char menuChoice;
static StrBuf menuPath;

// the index of the entry being saved, or workspaceNone once it is gone.
static u64
SavingIndex(void)
{
  return WorkspaceEntryOf(workspace, queue[position]);
}

static Document *
Saving(void)
{
  return workspace->entries[SavingIndex()].document;
}

static void
Begin(SaveFinished finished, bool all)
{
  afterSave = finished;
  saveAll = all;
  queueCount = 0;
  position = 0;
  namedForSave = false;
}

static void
Queue(u64 index)
{
  if (queueCount == queueCapacity) {
    queueCapacity = queueCapacity ? queueCapacity * 2 : 16;
    queue = MemoryResize(queue, queueCapacity, sizeof(u64));
  }

  queue[queueCount++] = workspace->entries[index].serial;
}

// a save that fails after the save as prompt leaves the document unnamed again.
static void
ForgetNewName(void)
{
  if (namedForSave) {
    StrBufClear(&Saving()->file.path);
    WorkspaceRenamed(workspace, SavingIndex());
  }

  namedForSave = false;
}

static void
Next(void);
static void
Write(bool overwriteChanged);

static void
OverwriteAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
)
{
  if (answer && *answer == 'y') {
    Write(true);
    return;
  }

  ForgetNewName();
  StrBufAppendString(message, "not saved");
}

static void
AskToOverwrite(const char *what)
{
  StrBuf question = {};
  StrBufAppendFormat(&question, "%s %s; overwrite it? (y/n)", Saving()->file.path.data, what);
  StrBufClear(message);
  PromptChoose(question.data, "yn", OverwriteAnswered, nullptr);
  StrBufFree(&question);
}

static void
Write(bool overwriteChanged)
{
  Document *document = Saving();
  enum FileStatus status = DocumentSave(document, overwriteChanged, message);

  if (status == FileStatusChanged) {
    AskToOverwrite("changed on disk");
    return;
  }

  if (status != FileStatusOk) {
    ForgetNewName();
    return;
  }

  namedForSave = false;
  u64 lines = BufferTextLineCount(&document->buffer);
  if (!saveAll) StrBufAppendFormat(message, "saved %lu line%s", lines, StrBufPlural(lines));
  position++;
  Next();
}

static void
SaveAsAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  if (!answer || !length) {
    StrBufAppendString(message, "not saved");
    return;
  }

  CompleteExpandHome(answer, length, &Saving()->file.path);
  WorkspaceRenamed(workspace, SavingIndex());
  namedForSave = true;

  // an existing file is only replaced once confirmed.
  struct stat status;

  if (lstat(Saving()->file.path.data, &status) == 0) {
    AskToOverwrite("exists");
    return;
  }

  Write(false);
}

static void
CompleteSaveAs([[maybe_unused]] void *context, StrBuf *text)
{
  CompletePath(text);
}

static void
AskForName(void)
{
  // during save all, the buffer is shown so the user can see what is being named.
  u64 index = SavingIndex();
  StrBuf label = {};

  if (saveAll) {
    WorkspaceSwitch(workspace, index, message);
    StrBufAppendString(&label, "save ");
    WorkspaceName(workspace, index, &label);
    StrBufAppendString(&label, " as: ");
  } else {
    StrBufAppendString(&label, "save as: ");
  }

  PromptAsk(label.data, "", SaveAsAnswered, nullptr);
  PromptHandle(nullptr, CompleteSaveAs);
  StrBufFree(&label);
}

// saves the next document of the run, or finishes it.
static void
Next(void)
{
  while (position < queueCount && SavingIndex() == workspaceNone) position++;

  if (position == queueCount) {
    u64 count = queueCount;
    const char *plural = StrBufPlural(count);
    if (saveAll && count) StrBufAppendFormat(message, "saved %lu file%s", count, plural);
    if (saveAll && !count) StrBufAppendString(message, "no unsaved changes");
    if (afterSave) afterSave();
    return;
  }

  Document *document = Saving();

  // trimming is one undo step, so the screen matches what is written.
  if (trimTrailingWhitespace) {
    EditContext edit = {.view = document->view, .undo = &document->undo};
    EditTrimTrailingWhitespace(&edit);
  }

  if (document->file.path.length) Write(false);
  else AskForName();
}

void
SaveCurrent(SaveFinished finished)
{
  Begin(finished, false);
  Queue(workspace->current);
  Next();
}

void
SaveAll(SaveFinished finished)
{
  Begin(finished, true);

  for (u64 index = 0; index < workspace->count; index++) {
    if (WorkspaceModified(workspace, index)) Queue(index);
  }

  Next();
}

void
SaveSome(const u64 *entries, u64 count, SaveFinished finished)
{
  Begin(finished, true);

  for (u64 index = 0; index < count; index++) {
    if (WorkspaceModified(workspace, entries[index])) Queue(entries[index]);
  }

  Next();
}

// the document the file menu acts on, or nullptr once its entry is gone.
static Document *
MenuDocument(void)
{
  u64 index = WorkspaceEntryOf(workspace, menuSerial);
  return index == workspaceNone ? nullptr : workspace->entries[index].document;
}

static void
Rename(void)
{
  Document *document = MenuDocument();
  if (!document || !DocumentRename(document, menuPath.data, message)) return;

  WorkspaceRenamed(workspace, WorkspaceEntryOf(workspace, menuSerial));
  StrBufAppendFormat(message, "renamed to %s", menuPath.data);
}

static void
SaveCopy(void)
{
  Document *document = MenuDocument();
  if (!document) return;

  // an image is copied as it is on disk.
  if (document->image) {
    if (!FileCopy(document->file.path.data, menuPath.data, message)) return;
    StrBufAppendFormat(message, "copied the image to %s", menuPath.data);
    return;
  }

  // a separate file, so the document's own path, stamp and save point stay as they are.
  // the target has already been confirmed when it exists.
  File copy = {.lineEnding = document->file.lineEnding};
  StrBufAppend(&copy.path, menuPath.data, menuPath.length);
  enum FileStatus status = FileSave(&copy, &document->buffer, true, message);
  FileFree(&copy);
  if (status != FileStatusOk) return;

  u64 lines = BufferTextLineCount(&document->buffer);
  const char *plural = StrBufPlural(lines);
  StrBufAppendFormat(
    message,
    "saved a copy of %lu line%s as %s",
    lines,
    plural,
    menuPath.data
  );
}

static void
Delete(void)
{
  u64 index = WorkspaceEntryOf(workspace, menuSerial);
  if (index == workspaceNone) return;

  // a file that a save never created leaves only the buffer to remove.
  const StrBuf *path = WorkspacePath(workspace, index);

  if (unlink(path->data) != 0 && errno != ENOENT) {
    StrBufAppendFormat(message, "%s: cannot delete: %s", path->data, strerror(errno));
    return;
  }

  StrBuf name = {};
  StrBufAppend(&name, path->data, path->length);
  WorkspaceRemove(workspace, index, message);
  if (message->length) StrBufAppendString(message, "; ");
  StrBufAppendFormat(message, "deleted %s", name.data);
  StrBufFree(&name);
}

static void
RunMenuChoice(void)
{
  if (menuChoice == 'r') Rename();
  else if (menuChoice == 'c') SaveCopy();
  else Delete();
}

static void
ConfirmAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
)
{
  if (answer && *answer == 'y') RunMenuChoice();
  else StrBufAppendString(message, menuChoice == 'd' ? "not deleted" : "nothing written");
}

static void
MenuPathAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  Document *document = MenuDocument();
  if (!answer || !length || !document) return;

  StrBufClear(&menuPath);
  CompleteExpandHome(answer, length, &menuPath);
  const File *file = &document->file;
  struct stat status;
  bool exists = lstat(menuPath.data, &status) == 0;

  if (file->path.length && strcmp(menuPath.data, file->path.data) == 0) {
    StrBufAppendString(message, "that is already the file's name");
    return;
  }

  // writing a copy over the document's own file would leave it out of step with the disk.
  bool same = exists && file->stamp.exists && stat(menuPath.data, &status) == 0 &&
    status.st_dev == file->stamp.device && status.st_ino == file->stamp.inode;

  if (same) {
    StrBufAppendString(message, "that is the buffer's own file");
    return;
  }

  if (!exists) {
    RunMenuChoice();
    return;
  }

  StrBuf question = {};
  StrBufAppendFormat(&question, "%s exists; overwrite it? (y/n)", menuPath.data);
  PromptChoose(question.data, "yn", ConfirmAnswered, nullptr);
  StrBufFree(&question);
}

static void
MenuAnswered([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  Document *document = MenuDocument();
  if (!answer || !document) return;

  menuChoice = *answer;
  const StrBuf *path = &document->file.path;

  if (menuChoice != 'c' && !path->length) {
    StrBufAppendString(message, "the buffer has no file name");
    return;
  }

  if (menuChoice == 'd') {
    StrBuf question = {};
    StrBufAppendFormat(&question, "delete %s", path->data);
    if (DocumentModified(document)) StrBufAppendString(&question, " and its unsaved changes");
    StrBufAppendString(&question, "? (y/n)");
    PromptChoose(question.data, "yn", ConfirmAnswered, nullptr);
    StrBufFree(&question);
    return;
  }

  const char *label = menuChoice == 'r' ? "rename to: " : "save a copy as: ";
  PromptAsk(label, path->length ? path->data : "", MenuPathAnswered, nullptr);
  PromptHandle(nullptr, CompleteSaveAs);
}

void
SaveFileMenu(void)
{
  menuSerial = workspace->entries[workspace->current].serial;
  PromptChoose("file: (r)ename (c)opy (d)elete", "rcd", MenuAnswered, nullptr);
}
