#pragma once

// saving documents, with the questions that can come up on the way: a name for an
// unnamed buffer, and whether to replace a file that exists or changed on disk.
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
