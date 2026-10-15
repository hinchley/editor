#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "memory.h"
#include "settings.h"

// how much text is gathered before each write while saving.
static constexpr u64 writeChunkSize = 64 * 1024;

// mode for new files before the umask is applied.
static constexpr u32 newFileMode = 0666;

// buffered output to the temporary file during a save.
typedef struct Writer {
  i32 descriptor;
  StrBuf chunk;
  i32 error;
} Writer;

const char *
FileLineEndingName(enum LineEnding lineEnding)
{
  return lineEnding == LineEndingCrlf ? "CRLF" : "LF";
}

void
FileFree(File *file)
{
  StrBufFree(&file->path);
  *file = (File){};
}

static FileStamp
StampOf(const struct stat *status)
{
  return (FileStamp){
    .exists = true,
    .device = status->st_dev,
    .inode = status->st_ino,
    .size = (u64)status->st_size,
    .seconds = status->st_mtim.tv_sec,
    .nanoseconds = status->st_mtim.tv_nsec
  };
}

static bool
StampEqual(FileStamp a, FileStamp b)
{
  if (a.exists != b.exists) return false;
  if (!a.exists) return true;

  return a.device == b.device && a.inode == b.inode && a.size == b.size &&
    a.seconds == b.seconds && a.nanoseconds == b.nanoseconds;
}

// true when the version current on disk is not the file's own.
static bool
Changed(const File *file, FileStamp current)
{
  return current.exists && !StampEqual(file->stamp, current);
}

bool
FileChangedOnDisk(const File *file)
{
  struct stat status;
  return stat(file->path.data, &status) == 0 && Changed(file, StampOf(&status));
}

static void
AppendError(StrBuf *message, const File *file, const char *what, i32 error)
{
  StrBufAppendFormat(message, "%s: %s: %s", file->path.data, what, strerror(error));
}

// reads up to size bytes, returning how many were read or -1 with errno set.
// the file may have shrunk since it was measured, so a short read is not an error.
static i64
ReadAll(i32 descriptor, char *data, u64 size)
{
  u64 total = 0;

  while (total < size) {
    i64 count = read(descriptor, data + total, size - total);

    if (count < 0) {
      if (errno == EINTR) continue;
      return -1;
    }

    if (count == 0) break;
    total += (u64)count;
  }

  return (i64)total;
}

// removes the "\r" of every "\r\n" in place and returns the new length.
// the more common ending wins; a "\r" anywhere else stays as content.
static u64
NormaliseEndings(char *data, u64 length, enum LineEnding *ending, bool *mixed)
{
  u64 lfCount = 0;
  u64 crlfCount = 0;
  u64 kept = 0;
  u64 at = 0;

  while (at < length) {
    char *newline = memchr(data + at, '\n', length - at);
    u64 lineEnd = newline ? (u64)(newline - data) : length;
    bool crlf = newline && lineEnd > at && data[lineEnd - 1] == '\r';
    u64 textEnd = crlf ? lineEnd - 1 : lineEnd;

    memmove(data + kept, data + at, textEnd - at);
    kept += textEnd - at;
    if (!newline) break;

    data[kept++] = '\n';
    if (crlf) crlfCount++;
    else lfCount++;
    at = lineEnd + 1;
  }

  // a tie, including a file with no line breaks, counts as lf like a new file.
  *ending = crlfCount > lfCount ? LineEndingCrlf : LineEndingLf;
  *mixed = lfCount && crlfCount;
  return kept;
}

static enum FileStatus
LoadOpened(File *file, Buffer *buffer, i32 descriptor, StrBuf *message)
{
  struct stat status;

  if (fstat(descriptor, &status) != 0) {
    AppendError(message, file, "cannot read file details", errno);
    return FileStatusError;
  }

  if (S_ISDIR(status.st_mode)) {
    StrBufAppendFormat(message, "%s: is a folder", file->path.data);
    return FileStatusError;
  }

  if (!S_ISREG(status.st_mode)) {
    StrBufAppendFormat(message, "%s: not a regular file", file->path.data);
    return FileStatusError;
  }

  u64 size = (u64)status.st_size;

  if (size > maxFileSize) {
    StrBufAppendFormat(
      message,
      "%s: larger than %lu MB, not opened",
      file->path.data,
      maxFileSize / (1024 * 1024)
    );
    return FileStatusTooLarge;
  }

  char *data = MemoryAllocate(size);
  i64 count = ReadAll(descriptor, data, size);

  if (count < 0) {
    AppendError(message, file, "cannot read", errno);
    free(data);
    return FileStatusError;
  }

  u64 length = (u64)count;
  u64 probe = length < binaryProbeSize ? length : binaryProbeSize;

  if (probe && memchr(data, '\0', probe)) {
    StrBufAppendFormat(message, "%s: binary file, not opened", file->path.data);
    free(data);
    return FileStatusBinary;
  }

  bool mixed;
  length = NormaliseEndings(data, length, &file->lineEnding, &mixed);
  BufferSetText(buffer, data, length);
  free(data);

  if (mixed) {
    StrBufAppendFormat(
      message,
      "%s: mixed line endings normalised to %s",
      file->path.data,
      FileLineEndingName(file->lineEnding)
    );
  }

  file->stamp = StampOf(&status);
  file->readOnly = access(file->path.data, W_OK) != 0;
  return FileStatusOk;
}

enum FileStatus
FileLoad(File *file, Buffer *buffer, StrBuf *message)
{
  // without O_NONBLOCK, opening a named pipe would wait for a writer.
  // it changes nothing for regular files.
  i32 descriptor = open(file->path.data, O_RDONLY | O_CLOEXEC | O_NONBLOCK);

  if (descriptor < 0 && errno == ENOENT) {
    // a new file: it is created by the first save.
    BufferSetText(buffer, "", 0);
    file->lineEnding = LineEndingLf;
    file->stamp = (FileStamp){};
    file->readOnly = false;
    return FileStatusNew;
  }

  if (descriptor < 0) {
    AppendError(message, file, "cannot open", errno);
    return FileStatusError;
  }

  enum FileStatus result = LoadOpened(file, buffer, descriptor, message);
  close(descriptor);
  return result;
}

static void
WriterFlush(Writer *writer)
{
  const char *data = writer->chunk.data;
  u64 length = writer->chunk.length;

  while (length && !writer->error) {
    i64 count = write(writer->descriptor, data, length);

    if (count < 0) {
      if (errno != EINTR) writer->error = errno;
      continue;
    }

    data += count;
    length -= (u64)count;
  }

  StrBufClear(&writer->chunk);
}

static void
WriterAppend(Writer *writer, const char *data, u64 length)
{
  // large lines go straight out instead of growing the chunk.
  if (writer->chunk.length + length > writeChunkSize) WriterFlush(writer);
  StrBufAppend(&writer->chunk, data, length);
  if (writer->chunk.length >= writeChunkSize) WriterFlush(writer);
}

static i32
WriteText(i32 descriptor, const Buffer *buffer, enum LineEnding lineEnding)
{
  Writer writer = {.descriptor = descriptor};
  StrBufReserve(&writer.chunk, writeChunkSize);

  const char *ending = lineEnding == LineEndingCrlf ? "\r\n" : "\n";
  u64 endingLength = strlen(ending);
  u64 count = BufferLineCount(buffer);

  for (u64 index = 0; index < count && !writer.error; index++) {
    const StrBuf *line = BufferLine(buffer, index);
    WriterAppend(&writer, line->data, line->length);
    if (index + 1 < count) WriterAppend(&writer, ending, endingLength);
  }

  WriterFlush(&writer);
  StrBufFree(&writer.chunk);
  return writer.error;
}

// the state of one save as it moves through its steps.
typedef struct Save {
  File *file;
  const Buffer *buffer;
  StrBuf *message;
  bool overwriteChanged;

  // the final target of any symbolic links, or the path itself for a new file.
  StrBuf target;

  // permissions for the written file.
  u32 mode;
} Save;

static bool
ResolveTarget(Save *save)
{
  const File *file = save->file;
  char resolved[PATH_MAX];

  if (realpath(file->path.data, resolved)) {
    StrBufAppendString(&save->target, resolved);
    return true;
  }

  if (errno != ENOENT) {
    AppendError(save->message, file, "cannot resolve path", errno);
    return false;
  }

  // replacing a dangling link would silently turn it into a regular file.
  struct stat status;

  if (lstat(file->path.data, &status) == 0) {
    StrBufAppendFormat(save->message, "%s: broken symbolic link", file->path.data);
    return false;
  }

  StrBufAppend(&save->target, file->path.data, file->path.length);
  return true;
}

// the temporary file sits next to the target so the final rename stays on one file system.
static void
TemporaryPath(const StrBuf *target, StrBuf *temporary)
{
  const char *slash = strrchr(target->data, '/');
  u64 folderLength = slash ? (u64)(slash - target->data) + 1 : 0;
  const char *name = target->data + folderLength;

  StrBufAppend(temporary, target->data, folderLength);
  StrBufAppendFormat(temporary, ".%s.editor-XXXXXX", name);
}

static u32
NewFileMode(void)
{
  // umask can only be read by setting it.
  mode_t mask = umask(0);
  umask(mask);
  return newFileMode & ~(u32)mask;
}

// checks the target before writing and finds the mode to give the new file.
static enum FileStatus
CheckTarget(Save *save)
{
  File *file = save->file;
  struct stat status;
  FileStamp current = {};
  save->mode = NewFileMode();

  if (stat(save->target.data, &status) == 0) {
    current = StampOf(&status);
    save->mode = status.st_mode & 07777;
  } else if (errno != ENOENT) {
    AppendError(save->message, file, "cannot read file details", errno);
    return FileStatusError;
  }

  // a file deleted since loading is simply written again.
  bool changed = Changed(file, current);

  if (changed && !save->overwriteChanged) {
    const char *what = file->stamp.exists ? "changed on disk" : "already exists";
    StrBufAppendFormat(save->message, "%s: %s", file->path.data, what);
    return FileStatusChanged;
  }

  if (current.exists && access(save->target.data, W_OK) != 0) {
    AppendError(save->message, file, "cannot write", errno);
    return FileStatusError;
  }

  return FileStatusOk;
}

// writes the text to a new temporary file and moves it over the target.
static enum FileStatus
WriteAndReplace(Save *save)
{
  File *file = save->file;
  StrBuf temporary = {};
  TemporaryPath(&save->target, &temporary);

  i32 descriptor = mkstemp(temporary.data);

  if (descriptor < 0) {
    AppendError(save->message, file, "cannot create a temporary file", errno);
    StrBufFree(&temporary);
    return FileStatusError;
  }

  const char *step = "cannot set permissions";
  i32 error = fchmod(descriptor, save->mode) == 0 ? 0 : errno;

  if (!error) {
    step = "cannot write";
    error = WriteText(descriptor, save->buffer, file->lineEnding);
  }

  if (!error && fsync(descriptor) != 0) error = errno;

  struct stat status;
  if (!error && fstat(descriptor, &status) != 0) error = errno;
  if (close(descriptor) != 0 && !error) error = errno;

  if (!error && rename(temporary.data, save->target.data) != 0) {
    step = "cannot replace the file";
    error = errno;
  }

  if (error) {
    unlink(temporary.data);
    AppendError(save->message, file, step, error);
  } else {
    file->stamp = StampOf(&status);
    file->readOnly = false;
  }

  StrBufFree(&temporary);
  return error ? FileStatusError : FileStatusOk;
}

enum FileStatus
FileSave(File *file, const Buffer *buffer, bool overwriteChanged, StrBuf *message)
{
  if (!file->path.length) {
    StrBufAppendString(message, "the buffer has no file name");
    return FileStatusError;
  }

  Save save = {
    .file = file,
    .buffer = buffer,
    .message = message,
    .overwriteChanged = overwriteChanged
  };
  enum FileStatus result = FileStatusError;

  if (ResolveTarget(&save)) result = CheckTarget(&save);
  if (result == FileStatusOk) result = WriteAndReplace(&save);

  StrBufFree(&save.target);
  return result;
}

// copies the file at from over to through a temporary file next to it, keeping the
// permissions, and returns 0 or the error with step set to what failed.
static i32
CopyFile(const char *from, const char *to, const char **step)
{
  *step = "cannot read";
  i32 source = open(from, O_RDONLY | O_CLOEXEC);
  if (source < 0) return errno;

  struct stat status;

  if (fstat(source, &status) != 0) {
    i32 error = errno;
    close(source);
    return error;
  }

  StrBuf target = {};
  StrBuf temporary = {};
  StrBufAppendString(&target, to);
  TemporaryPath(&target, &temporary);
  StrBufFree(&target);

  *step = "cannot create a temporary file";
  i32 descriptor = mkstemp(temporary.data);
  i32 error = descriptor < 0 ? errno : 0;
  if (!error && fchmod(descriptor, status.st_mode & 07777) != 0) error = errno;

  char chunk[writeChunkSize];

  while (!error) {
    *step = "cannot read";
    i64 count = read(source, chunk, sizeof chunk);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) error = errno;
    if (count <= 0) break;

    *step = "cannot write";

    for (i64 at = 0; at < count && !error;) {
      i64 written = write(descriptor, chunk + at, (u64)(count - at));
      if (written < 0 && errno != EINTR) error = errno;
      if (written > 0) at += written;
    }
  }

  if (!error && fsync(descriptor) != 0) error = errno;
  if (descriptor >= 0 && close(descriptor) != 0 && !error) error = errno;
  close(source);

  if (!error && rename(temporary.data, to) != 0) {
    *step = "cannot replace the file";
    error = errno;
  }

  if (error && descriptor >= 0) unlink(temporary.data);
  StrBufFree(&temporary);
  return error;
}

bool
FileRename(File *file, const char *path, StrBuf *message)
{
  struct stat status;
  bool current = stat(file->path.data, &status) == 0 &&
    StampEqual(file->stamp, StampOf(&status));
  i32 error = rename(file->path.data, path) == 0 ? 0 : errno;
  const char *step = "cannot rename";
  bool copied = false;

  if (error == EXDEV) {
    error = CopyFile(file->path.data, path, &step);
    copied = !error;
  }

  // once the copy is made the file has moved, even if the original stays behind.
  if (copied && unlink(file->path.data) != 0) {
    AppendError(message, file, "copied, but cannot remove the original", errno);
  }

  if (error == ENOENT && !file->stamp.exists) error = 0;

  if (error) {
    AppendError(message, file, step, error);
    return false;
  }

  StrBufClear(&file->path);
  StrBufAppendString(&file->path, path);
  if (current && stat(path, &status) == 0) file->stamp = StampOf(&status);
  return true;
}
