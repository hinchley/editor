#pragma once

// saving documents, with the questions that can come up on the way: a name for an
// unnamed buffer, and whether to replace a file that exists or changed on disk.
// the file menu also lives here: renaming, copying and deleting the current file.
// any failure or refusal stops the run, so whatever waits for it (quitting, replacing
// the buffer list) never happens after a failed save.
#include "strbuf.h"
#include "workspace.h"

// runs once every document of the run was saved.
typedef void (*SaveFinished)(void);

// saves the current document, then runs finished (which may be nullptr).
void
SaveCurrent(Workspace *workspace, StrBuf *message, SaveFinished finished);

// saves every modified document in list order, then runs finished.
// an unnamed one is made current before asking for its name.
void
SaveAll(Workspace *workspace, StrBuf *message, SaveFinished finished);

// asks whether to rename the current document's file, save a copy of it or delete it,
// then asks for the new path (with tab completion) or for confirmation.
// rename moves the file on disk and keeps any unsaved changes unsaved; a copy is written
// from the text as it is, without trimming, and leaves the document alone; delete removes
// the file and its buffer.
// replacing an existing file is only done once confirmed.
void
SaveFileMenu(Workspace *workspace, StrBuf *message);
