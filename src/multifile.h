#pragma once

// multifile: changes made to several files at once: a project replace, and a rename, a
// fix or formatting from a language server.
// each file's part of a change is one undo step in its buffer, and the last change is
// remembered: alt+shift+z lists the places it changed, with a preview, and in that list
// ctrl+z undoes it in every file at once and ctrl+y redoes it.
#include "document.h"
#include "types.h"

// starts a change across files, named by what ("rename", "replace"), which takes the
// place of the last one once it changes a file.
void
MultifileBegin(const char *what);

// opens the entry at index for its part of the change, and starts one undo step for it;
// nullptr when it cannot be opened.
// MultifileEditEnd ends the step.
Document *
MultifileEditBegin(u64 index);

void
MultifileEditEnd(Document *document);

// after undo (or redo) of group in the entry with serial, says when that was only the
// entry's part of the last change, and the other files still have theirs as it was.
void
MultifileNoteStep(u64 serial, u64 group, bool redo);

// lists the places the last change made, as the files have them now, done or undone.
void
MultifileReview(void);
