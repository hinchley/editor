#pragma once

// a buffer together with its file, undo history and views: everything the editor keeps
// about one open file.
// a view holds the cursors and scroll position, so switching away and back finds them
// where they were.
// there is a view for each side of a split (its slot), so the same buffer can be shown
// twice with its own cursors in each.
// a document must not move in memory once opened, because its history and views listen to
// its buffer through pointers.
#include "buffer.h"
#include "changes.h"
#include "file.h"
#include "highlight.h"
#include "image.h"
#include "lang.h"
#include "lsp.h"
#include "match.h"
#include "strbuf.h"
#include "types.h"
#include "undo.h"
#include "view.h"

typedef struct Document {
  Buffer buffer;
  File file;
  Undo undo;
  View views[2];

  // the view of the slot that last had focus on this document, which the commands use.
  View *view;

  // the line ending the file had when last loaded or saved, so switching back to it
  // clears the modified state again.
  enum LineEnding savedLineEnding;

  // what the view highlights: search matches, unknown words or nothing.
  enum MatchKind marks;

  // the language from the file's name or extension, and its highlighter (nullptr without
  // either).
  const Lang *lang;

  // the language a "#!" first line named when the file opened, for a path without an
  // extension.
  const Lang *scriptLang;
  Highlight *highlight;

  // how the text differs from the file in the last commit of its git repository, or
  // nullptr when it is not in one.
  Changes *changes;

  // what the language server knows of the document, or nullptr when its language has no
  // server or servers are off.
  LspDocument *lsp;

  // the picture of an image file, which is shown in place of the (empty) text and never
  // changed, or nullptr for a text file.
  // an image has no language, highlighter, changes or language server.
  Image *image;
} Document;

// opens path, or an unnamed empty document when path is nullptr.
// a path that does not exist yet gives an empty document that its first save creates.
// a binary file opens as an image when it is one the editor can show.
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
// a save under a new name with another extension changes the language.
// an image is never saved.
enum FileStatus
DocumentSave(Document *document, bool overwriteChanged, StrBuf *message);

// moves the document's file to path; see FileRename.
// unsaved changes stay unsaved, and another extension changes the language.
bool
DocumentRename(Document *document, const char *path, StrBuf *message);

// replaces the text with the file's current contents, forgets the history and keeps the
// cursors inside the new text, or reads an image again.
// returns false, changing nothing, when the file cannot be read.
bool
DocumentReload(Document *document, StrBuf *message);

// starts or stops following the document with its language server, after servers were
// turned on or off.
void
DocumentRefreshServer(Document *document);

// makes the view in slot the one the commands use.
// unless it was already, it first takes the cursors and scroll position of the view used
// until now, so the document shows where it was last seen.
void
DocumentShow(Document *document, u32 slot);

// makes the view in slot the one the commands use, keeping its own cursors.
void
DocumentFocus(Document *document, u32 slot);
