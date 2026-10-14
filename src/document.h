#pragma once

// a buffer together with its file and undo history: everything the editor keeps about
// one file.
// a document must not move in memory once opened, because its history listens to its
// buffer through a pointer.
#include "buffer.h"
#include "file.h"
#include "strbuf.h"
#include "types.h"
#include "undo.h"

typedef struct Document {
  Buffer buffer;
  File file;
  Undo undo;

  // the line ending the file had when last loaded or saved, so switching back to it
  // clears the modified state again.
  enum LineEnding savedLineEnding;
} Document;

// opens path, or an unnamed empty document when path is nullptr.
// a file that cannot be opened also leaves an unnamed empty document.
// anything worth telling the user is appended to message.
void
DocumentOpen(Document *document, const char *path, StrBuf *message);

void
DocumentFree(Document *document);

// true when the text or line ending differs from the file.
bool
DocumentModified(const Document *document);

// saves to the document's path; see FileSave.
enum FileStatus
DocumentSave(Document *document, bool overwriteChanged, StrBuf *message);

// replaces the text with the file's current contents and forgets the history.
// returns false, changing nothing, when the file cannot be read.
bool
DocumentReload(Document *document, StrBuf *message);
