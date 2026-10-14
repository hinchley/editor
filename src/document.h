#pragma once

// a buffer together with its file, undo history and view: everything the editor keeps
// about one open file.
// the view holds the cursors and scroll position, so switching away and back finds them
// where they were.
// a document must not move in memory once opened, because its history and view listen to
// its buffer through pointers.
#include "buffer.h"
#include "file.h"
#include "strbuf.h"
#include "types.h"
#include "undo.h"
#include "view.h"

typedef struct Document {
  Buffer buffer;
  File file;
  Undo undo;
  View view;

  // the line ending the file had when last loaded or saved, so switching back to it
  // clears the modified state again.
  enum LineEnding savedLineEnding;
} Document;

// opens path, or an unnamed empty document when path is nullptr.
// a path that does not exist yet gives an empty document that its first save creates.
// anything worth telling the user is appended to message.
// unless the status is FileStatusOk or FileStatusNew the file was not read, and the
// document (left empty) should be freed.
enum FileStatus
DocumentOpen(
  Document *document,
  const char *path,
  const DisplayOptions *options,
  StrBuf *message
);

void
DocumentFree(Document *document);

// true when the text or line ending differs from the file.
bool
DocumentModified(const Document *document);

// saves to the document's path; see FileSave.
enum FileStatus
DocumentSave(Document *document, bool overwriteChanged, StrBuf *message);

// replaces the text with the file's current contents, forgets the history and keeps the
// cursors inside the new text.
// returns false, changing nothing, when the file cannot be read.
bool
DocumentReload(Document *document, StrBuf *message);
