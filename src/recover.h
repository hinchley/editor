#pragma once

// recover: keeps a copy of every modified buffer in <state folder>/editor/recover, so the
// work survives the window closing, a signal, a crash or a power cut.
// a copy is written once input has rested for recoverDelay, and at once when the terminal
// hangs up; it is named after the file's absolute path, with "/" as "%", or "unnamed-N",
// followed by "~" and the editor's process id.
// a copy goes once its buffer is saved, unmodified or closed, and every copy of this
// process goes at a normal exit, so the folder only holds work that was lost.
#include "strbuf.h"
#include "types.h"
#include "workspace.h"

// writes copies of the buffers changed since the last pass, and removes the copies of
// buffers no longer modified.
// a buffer over maxRecoverSize is only copied by RecoverHangUp.
void
RecoverWrite(Workspace *workspace);

// writes a copy of every modified buffer, when the terminal has gone.
void
RecoverHangUp(Workspace *workspace);

// milliseconds until RecoverWrite is due, or -1 when nothing is waiting.
i32
RecoverTimeout(const Workspace *workspace);

// records that input arrived, which postpones the next copy.
void
RecoverInput(void);

// removes every copy this process wrote, at a normal exit.
void
RecoverForget(void);

// tells, through message, how many copies earlier sessions left behind.
void
RecoverReport(StrBuf *message);
