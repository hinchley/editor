#include "recover.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "document.h"
#include "file.h"
#include "settings.h"
#include "term.h"

static i64 lastInput;

// the folder of the copies, made private when missing; false when there is none.
static bool
Folder(StrBuf *path)
{
  return FileStateFolder("recover", 0700, path);
}

// the copy's path for an entry: its absolute path with "/" as "%" (cut from the front to
// fit a file name), or "unnamed-N", then "~pid".
static void
CopyPath(const StrBuf *folder, const WorkspaceEntry *entry, StrBuf *path)
{
  StrBuf name = {};
  const StrBuf *file = &entry->document->file.path;

  if (!file->length) {
    StrBufAppendFormat(&name, "unnamed-%u", entry->unnamed);
  } else {
    char here[PATH_MAX];
    bool relative = file->data[0] != '/' && getcwd(here, sizeof(here));
    if (relative) StrBufAppendFormat(&name, "%s/", here);
    StrBufAppend(&name, file->data, file->length);
    for (u64 at = 0; at < name.length; at++) {
      if (name.data[at] == '/') name.data[at] = '%';
    }
  }

  u64 skip = name.length > 200 ? name.length - 200 : 0;
  StrBufAppendFormat(path, "%s/%s~%d", folder->data, name.data + skip, getpid());
  StrBufFree(&name);
}

// the buffer's size in bytes, with "\n" line endings.
static u64
Size(const Buffer *buffer)
{
  u64 size = 0;
  for (u64 line = 0; line < BufferLineCount(buffer); line++) {
    size += BufferLine(buffer, line)->length + 1;
  }
  return size;
}

// true when a pass has something to do for the document: copy changes it has not seen,
// or remove the copy of work now saved.
static bool
Waiting(const Document *document)
{
  if (!document || document->image) return false;
  if (!DocumentModified(document)) return document->recoveredPath.length;
  return document->buffer.revision != document->recovered;
}

// writes the entry's copy, replacing any copy under an older name.
static void
Copy(const StrBuf *folder, const WorkspaceEntry *entry)
{
  Document *document = entry->document;
  File copy = {.lineEnding = document->file.lineEnding};
  CopyPath(folder, entry, &copy.path);

  StrBuf ignored = {};
  if (FileSave(&copy, &document->buffer, true, &ignored) == FileStatusOk) {
    const StrBuf *old = &document->recoveredPath;
    if (old->length && strcmp(old->data, copy.path.data) != 0) unlink(old->data);

    StrBufClear(&document->recoveredPath);
    StrBufAppend(&document->recoveredPath, copy.path.data, copy.path.length);
  }

  StrBufFree(&ignored);
  FileFree(&copy);
}

static void
Pass(Workspace *workspace, bool hangUp)
{
  StrBuf folder = {};
  bool ok = Folder(&folder);

  for (u64 index = 0; ok && index < workspace->count; index++) {
    WorkspaceEntry *entry = &workspace->entries[index];
    Document *document = entry->document;
    if (!document || document->image || (!hangUp && !Waiting(document))) continue;

    // a failed copy is tried again after the next change, not on every pass.
    document->recovered = document->buffer.revision;

    if (!DocumentModified(document)) {
      if (!document->recoveredPath.length) continue;
      unlink(document->recoveredPath.data);
      StrBufClear(&document->recoveredPath);
    } else if (hangUp || Size(&document->buffer) <= maxRecoverSize) {
      Copy(&folder, entry);
    }
  }

  StrBufFree(&folder);
}

void
RecoverWrite(Workspace *workspace)
{
  Pass(workspace, false);
}

void
RecoverHangUp(Workspace *workspace)
{
  Pass(workspace, true);
}

i32
RecoverTimeout(const Workspace *workspace)
{
  // nothing can be due before input has rested, so the entries are only looked through
  // then: this is asked on every pass of the main loop, and going through every entry of
  // a large folder would cost each key about a millisecond. the price is one pass of the
  // loop recoverDelay after the last input, with nothing to copy.
  i64 left = lastInput + recoverDelay - TermNow();
  if (left > 0) return (i32)left;

  bool waiting = false;

  for (u64 index = 0; index < workspace->count && !waiting; index++) {
    waiting = Waiting(workspace->entries[index].document);
  }

  return waiting ? 0 : -1;
}

void
RecoverInput(void)
{
  lastInput = TermNow();
}

// the number of copies left by processes no longer running; with forget, this process's
// own copies are removed on the way.
static u64
Copies(const StrBuf *folder, bool forget)
{
  DIR *listing = opendir(folder->data);
  u64 count = 0;

  for (struct dirent *item; listing && (item = readdir(listing));) {
    const char *tilde = strrchr(item->d_name, '~');
    if (!tilde || !tilde[1]) continue;

    pid_t pid = (pid_t)strtol(tilde + 1, nullptr, 10);

    if (pid == getpid()) {
      if (forget) unlinkat(dirfd(listing), item->d_name, 0);
      continue;
    }

    // a copy of a process still running is that session's own.
    if (kill(pid, 0) != 0 && errno == ESRCH) count++;
  }

  if (listing) closedir(listing);
  return count;
}

void
RecoverForget(void)
{
  StrBuf folder = {};
  if (Folder(&folder)) Copies(&folder, true);
  StrBufFree(&folder);
}

void
RecoverReport(StrBuf *message)
{
  StrBuf folder = {};
  u64 count = Folder(&folder) ? Copies(&folder, false) : 0;

  if (count) {
    // the folder is shown with "~" for the home folder.
    const char *home = getenv("HOME");
    u64 homeLength = home ? strlen(home) : 0;
    bool below = homeLength > 1 && strncmp(folder.data, home, homeLength) == 0 &&
      folder.data[homeLength] == '/';

    if (message->length) StrBufAppendString(message, "; ");
    StrBufAppendFormat(
      message,
      "%lu unsaved buffer%s recovered in %s%s",
      count,
      StrBufPlural(count),
      below ? "~" : "",
      folder.data + (below ? homeLength : 0)
    );
  }

  StrBufFree(&folder);
}
