#include "document.h"

#include <string.h>

// picks the language from the path and starts highlighting in it, when it changed.
static void
DetectLanguage(Document *document, StrBuf *message)
{
  const char *path = document->file.path.data;
  const Lang *lang = path ? LangOfPath(path, document->scriptLang) : nullptr;
  if (lang == document->lang && (document->highlight || !lang)) return;

  HighlightFree(document->highlight);
  document->lang = lang;
  document->highlight = lang ? HighlightNew(&document->buffer, lang, message) : nullptr;
}

// finds the repository of the file again when its path changed, and otherwise reads its
// committed text again when HEAD moved.
static void
DetectChanges(Document *document)
{
  const char *path = document->file.path.data;
  Changes *changes = document->changes;

  if (changes && path && strcmp(ChangesPath(changes), path) == 0) {
    ChangesRefresh(changes);
    return;
  }

  ChangesFree(changes);
  document->changes = path ? ChangesNew(&document->buffer, path) : nullptr;
}

void
DocumentRefreshServer(Document *document)
{
  const char *path = document->file.path.data;
  if (LspEnabled() && LspFollows(document->lsp, document->lang, path)) return;

  LspClose(document->lsp);
  document->lsp = LspOpen(&document->buffer, document->lang, path);
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
  if (!read) return status;

  const StrBuf *first = BufferLine(&document->buffer, 0);
  document->scriptLang = LangOfScript(first->data, first->length);
  DetectLanguage(document, message);
  DetectChanges(document);
  DocumentRefreshServer(document);
  return status;
}

void
DocumentFree(Document *document)
{
  LspClose(document->lsp);
  HighlightFree(document->highlight);
  ChangesFree(document->changes);
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
  DetectChanges(document);

  // a save under another name opens the document again, which needs no word of the save.
  bool same = LspFollows(document->lsp, document->lang, file->path.data);
  DocumentRefreshServer(document);
  if (same) LspSaved(document->lsp);
  return status;
}

bool
DocumentRename(Document *document, const char *path, StrBuf *message)
{
  if (!FileRename(&document->file, path, message)) return false;

  DetectLanguage(document, message);
  DetectChanges(document);
  DocumentRefreshServer(document);
  return true;
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
  ChangesRefresh(document->changes);
  LspReloaded(document->lsp);
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
