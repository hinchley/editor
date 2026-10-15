#pragma once

// project search: finds a pattern in every loaded buffer whose path is in a scope, and
// lists the lines that match beside a preview of the text around them.
// the search is a scan (see scan.h): open buffers are searched as they are in memory, and
// the others are read straight from disk without opening them, a slice at a time, so it
// shows its progress and escape stops it, keeping the lines found so far.
// ctrl+r in the list replaces every match on the lines it shows, in their buffers, leaving
// them unsaved.
// the list is kept until escape in the list or a replace clears it, a new search replaces
// it or a folder replaces every buffer.
#include "pattern.h"
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

// the commands act on workspace and report to message.
void
ProjectInit(Workspace *workspace, StrBuf *message);

// asks which files to search for pattern, starting with the last scope, then starts the
// search.
// a scope is a list of path patterns separated by spaces, as in .editor files, matched
// against display paths: plain ones pick the files to search (all of them when there is
// none) and ones starting with "!" leave files out.
void
ProjectAsk(const Pattern *pattern);

// opens the list again, with the line after the one last opened selected.
void
ProjectShow(void);

// forgets the list.
void
ProjectClear(void);
