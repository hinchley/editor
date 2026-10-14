#pragma once

// reading files into buffers and writing them back.
// a buffer's text always uses "\n" between lines; the file's own line ending is detected
// on load and written back on save, so the bytes on disk round trip exactly.
// saving is atomic: the text goes to a temporary file that then replaces the original.
#include "buffer.h"
#include "strbuf.h"
#include "types.h"

enum LineEnding {
  LineEndingLf,
  LineEndingCrlf
};

// identifies a file on disk, to notice the same file reached by two paths.
typedef struct FileId {
  u64 device;
  u64 inode;
} FileId;

// identifies one version of a file on disk, to notice changes made by other programs.
typedef struct FileStamp {
  bool exists;
  u64 device;
  u64 inode;
  u64 size;
  i64 seconds;
  i64 nanoseconds;
} FileStamp;

// the file behind a buffer.
typedef struct File {
  // the path as the user gave it, or empty for an unnamed buffer.
  StrBuf path;
  enum LineEnding lineEnding;

  // the version last loaded or saved.
  FileStamp stamp;

  // the file exists but cannot be written.
  bool readOnly;
} File;

enum FileStatus {
  FileStatusOk,

  // load: nothing exists at the path yet, so the buffer starts empty.
  FileStatusNew,

  // load: the file is not opened because of its size or content.
  FileStatusTooLarge,
  FileStatusBinary,

  // save: the file changed on disk (or appeared) since it was loaded or saved.
  // saving again with overwriteChanged replaces it.
  FileStatusChanged,

  FileStatusError
};

// reads the file at file->path into buffer and sets its line ending and stamp.
// mixed line endings are normalised to the more common one.
// anything worth telling the user, such as an error or the normalisation, is appended to
// message.
// buffer is only changed when the status is FileStatusOk or FileStatusNew.
enum FileStatus
FileLoad(File *file, Buffer *buffer, StrBuf *message);

// writes buffer to file->path using its line ending, following symbolic links and keeping
// the file's permissions.
// problems are appended to message.
enum FileStatus
FileSave(File *file, const Buffer *buffer, bool overwriteChanged, StrBuf *message);

// short name of a line ending for display, "LF" or "CRLF".
const char *
FileLineEndingName(enum LineEnding lineEnding);

// frees the path and leaves a zeroed File.
void
FileFree(File *file);
