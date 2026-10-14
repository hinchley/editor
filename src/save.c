#include "save.h"

#include <sys/stat.h>

#include "complete.h"
#include "edit.h"
#include "memory.h"
#include "prompt.h"
#include "settings.h"

// one run of saves: the entries still to save and what to do once they are.
static Workspace *workspace;
static StrBuf *message;
static SaveFinished afterSave;
static u64 *queue;
static u64 queueCount;
static u64 queueCapacity;
static u64 position;
static bool saveAll;

// the document being saved had no name before the save as prompt gave it one.
static bool namedForSave;

static Document *
Saving(void)
{
  return workspace->entries[queue[position]].document;
}

static void
Begin(Workspace *newWorkspace, StrBuf *newMessage, SaveFinished finished, bool all)
{
  workspace = newWorkspace;
  message = newMessage;
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

  queue[queueCount++] = index;
}

// a save that fails after the save as prompt leaves the document unnamed again.
static void
ForgetNewName(void)
{
  if (namedForSave) StrBufClear(&Saving()->file.path);
  namedForSave = false;
}

static void Next(void);
static void Write(bool overwriteChanged);

static void
OverwriteAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
) {
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
  u64 lines = BufferLineCount(&document->buffer);
  if (!saveAll) StrBufAppendFormat(message, "saved %lu lines", lines);
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
  u64 index = queue[position];
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
  if (position == queueCount) {
    u64 count = queueCount;
    const char *plural = count == 1 ? "" : "s";
    if (saveAll && count) StrBufAppendFormat(message, "saved %lu file%s", count, plural);
    if (saveAll && !count) StrBufAppendString(message, "no unsaved changes");
    if (afterSave) afterSave();
    return;
  }

  Document *document = Saving();

  // trimming is one undo step, so the screen matches what is written.
  if (trimTrailingWhitespace) {
    EditContext edit = {.view = &document->view, .undo = &document->undo};
    EditTrimTrailingWhitespace(&edit);
  }

  if (document->file.path.length) Write(false);
  else AskForName();
}

void
SaveCurrent(Workspace *newWorkspace, StrBuf *newMessage, SaveFinished finished)
{
  Begin(newWorkspace, newMessage, finished, false);
  Queue(newWorkspace->current);
  Next();
}

void
SaveAll(Workspace *newWorkspace, StrBuf *newMessage, SaveFinished finished)
{
  Begin(newWorkspace, newMessage, finished, true);

  for (u64 index = 0; index < newWorkspace->count; index++) {
    if (WorkspaceModified(newWorkspace, index)) Queue(index);
  }

  Next();
}
