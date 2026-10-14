#include "document.h"

void
DocumentOpen(Document *document, const char *path, StrBuf *message)
{
  *document = (Document){};
  BufferInit(&document->buffer);
  UndoInit(&document->undo, &document->buffer);

  if (path) {
    StrBufAppendString(&document->file.path, path);
    enum FileStatus status = FileLoad(&document->file, &document->buffer, message);

    // the reason is in the message, and the document stays unnamed.
    if (status != FileStatusOk && status != FileStatusNew) FileFree(&document->file);
    if (status == FileStatusNew) StrBufAppendString(message, "new file");
  }

  document->savedLineEnding = document->file.lineEnding;
}

void
DocumentFree(Document *document)
{
  UndoFree(&document->undo);
  BufferFree(&document->buffer);
  FileFree(&document->file);
}

bool
DocumentModified(const Document *document)
{
  if (!UndoAtSavePoint(&document->undo)) return true;
  return document->file.lineEnding != document->savedLineEnding;
}

enum FileStatus
DocumentSave(Document *document, bool overwriteChanged, StrBuf *message)
{
  File *file = &document->file;
  enum FileStatus status = FileSave(file, &document->buffer, overwriteChanged, message);
  if (status != FileStatusOk) return status;

  UndoMarkSaved(&document->undo);
  document->savedLineEnding = file->lineEnding;
  return status;
}

bool
DocumentReload(Document *document, StrBuf *message)
{
  // the file is read into a separate buffer, so a failure leaves the text alone.
  // the copy of the file shares its path but is never freed.
  File loaded = document->file;
  Buffer fresh = {};
  enum FileStatus status = FileLoad(&loaded, &fresh, message);

  if (status == FileStatusNew) {
    StrBufAppendFormat(message, "%s: no longer exists", loaded.path.data);
  }

  if (status != FileStatusOk) {
    BufferFree(&fresh);
    return false;
  }

  BufferSwapText(&document->buffer, &fresh);
  BufferFree(&fresh);
  document->file = loaded;
  document->savedLineEnding = loaded.lineEnding;
  UndoClear(&document->undo);
  return true;
}
