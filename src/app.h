#pragma once

// application entry point, and the state the commands share.
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

// the one workspace and the message bar, cleared by the next key.
// command modules (search, save, session and the like) use these; data-structure modules
// (workspace, document, file and below) take what they act on as parameters, and do not
// include this header.
extern Workspace *const workspace;
extern StrBuf *const message;

// runs the editor and returns the process exit status.
i32
AppRun(i32 argumentCount, char **arguments);
