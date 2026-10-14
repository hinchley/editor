#include "document.h"

// picks the language from the path and starts highlighting in it, when it changed.
static void
DetectLanguage(Document *document, StrBuf *message)
{
  const char *path = document->file.path.data;
  const Lang *lang = path ? LangOfPath(path) : nullptr;
  if (lang == document->lang && (document->highlight || !lang)) return;

  HighlightFree(document->highlight);
  document->lang = lang;
  document->highlight = lang ? HighlightNew(&document->buffer, lang->name, message) : nullptr;
}

enum FileStatus
DocumentOpen(
  Document *document,
  const char *path,
  const DisplayOptions *options,
  StrBuf *message
) {
  *document = (Document){};
  BufferInit(&document->buffer);
  UndoInit(&document->undo, &document->buffer);
  ViewInit(&document->views[0], &document->buffer, options);
  ViewInit(&document->views[1], &document->buffer, options);
  document->view = &document->views[0];
  enum FileStatus status = FileStatusOk;

  if (path) {
    StrBufAppendString(&document->file.path, path);
    status = FileLoad(&document->file, &document->buffer, message);
    if (status == FileStatusNew) StrBufAppendFormat(message, "%s: new file", path);
  }

  document->savedLineEnding = document->file.lineEnding;
  bool read = status == FileStatusOk || status == FileStatusNew;
  if (read) DetectLanguage(document, message);
  return status;
}

void
DocumentFree(Document *document)
{
  HighlightFree(document->highlight);
  ViewFree(&document->views[0]);
  ViewFree(&document->views[1]);
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
  DetectLanguage(document, message);
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
  HighlightReset(document->highlight);
  ViewClamp(&document->views[0]);
  ViewClamp(&document->views[1]);
  return true;
}

void
DocumentShow(Document *document, u32 slot)
{
  View *view = &document->views[slot];
  if (view != document->view) ViewCopy(view, document->view);
  document->view = view;
}

void
DocumentFocus(Document *document, u32 slot)
{
  document->view = &document->views[slot];
}
