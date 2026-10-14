#pragma once

// tab completion of a path typed in a prompt.
#include "strbuf.h"

// completes the last name in path from the entries of the folder it names (relative to
// the working folder unless it starts with "/", and from the home folder when it starts
// with "~/").
// a "~" stays as typed, and "~" alone becomes "~/".
// it first extends the name as far as every candidate agrees, adding "/" when a single
// folder is left.
// when that adds nothing, each further call steps through the candidates in turn.
// names starting with "." are only offered when the typed name starts with one.
void
CompletePath(StrBuf *path);

// appends path with a leading "~" (alone or before "/") replaced by the home folder.
void
CompleteExpandHome(const char *path, u64 length, StrBuf *expanded);
