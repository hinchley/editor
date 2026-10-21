#pragma once

// lsp: the language server client.
// a language whose package names a server gets one process for each root folder, started
// when the first file of the language under that root opens (see ARCHITECTURE.md,
// "Language servers").
// each open document of the language has an LspDocument, which tells the server about the
// document and follows its buffer's changes.
// changes are collected as they happen and sent once per pass of the main loop, so a
// burst of edits costs one message.
// nothing waits for a server: its output wakes the main loop's poll, and LspPump handles
// it.
#include <poll.h>

#include "buffer.h"
#include "diagnostics.h"
#include "lang.h"
#include "strbuf.h"
#include "types.h"

typedef struct LspDocument LspDocument;

// a request a command is waiting on, or 0 for none.
typedef u64 LspRequest;

// a place a server pointed to: the file's path (relative to the folder the editor started
// in when inside it) and a range in its text, in bytes.
typedef struct LspLocation {
  const char *path;
  Range range;
} LspLocation;

// a server's answer of places, valid while the callback runs; answered is false when there
// is no answer (the server failed, did not answer in time or stopped, which was reported).
typedef void (*LspLocationsDone)(
  void *context,
  const LspLocation *locations,
  u32 count,
  bool answered
);

// a server's answer of text, as for LspLocationsDone.
typedef void (*LspTextDone)(void *context, const char *text, u64 length, bool answered);

// a change a server wants made: range in the file at path (as for LspLocation) replaced
// with text.
// a file's changes never overlap, and each range is in the text as it was before any of
// them.
typedef struct LspEdit {
  const char *path;
  Range range;
  const char *text;
  u64 length;
} LspEdit;

// a server's answer of changes, as for LspLocationsDone.
typedef void (*LspEditsDone)(void *context, const LspEdit *edits, u32 count, bool answered);

// a fix a server offers: what it is called, its changes, and whether the server thinks it
// is the one to take.
typedef struct LspAction {
  const char *title;
  const LspEdit *edits;
  u32 editCount;
  bool preferred;
} LspAction;

// a server's answer of fixes, as for LspLocationsDone.
typedef void (*LspActionsDone)(
  void *context,
  const LspAction *actions,
  u32 count,
  bool answered
);

// the most descriptors LspPollFds fills.
static constexpr u32 lspMaxPollFds = 16;

// reports what the servers have to say (failures, error messages) to message.
void
LspInit(StrBuf *message);

// whether servers are on, which LspOpen needs.
bool
LspEnabled(void);

// turns servers on or off.
// turning them off stops every server, after which each document should be closed (and
// opened again once they are on).
void
LspSetEnabled(bool enabled);

// true when a server stopped (it crashed, could not start or refused to) while servers
// were on.
bool
LspAnyStopped(void);

// starts following the buffer of the file at path in lang, starting its server if need
// be; nullptr when servers are off, the language has no server, there is no path, or the
// buffer is larger than maxServerFileSize.
LspDocument *
LspOpen(Buffer *buffer, const Lang *lang, const char *path);

// stops following the document and tells its server it closed.
void
LspClose(LspDocument *document);

// true when the document is the file at path in lang, so nothing need change.
bool
LspFollows(const LspDocument *document, const Lang *lang, const char *path);

// tells the server the document was saved.
void
LspSaved(LspDocument *document);

// the buffer's text was replaced without telling listeners (a reload), so the server is
// sent all of it.
void
LspReloaded(LspDocument *document);

// the diagnostics the server last reported for the document, following its edits since,
// or nullptr for no document.
const Diagnostics *
LspDiagnostics(const LspDocument *document);

// asks the document's server where the symbol at position is defined, where it is used
// (with its declaration), or what it is (its hover text, markdown or plain); or, from
// clangd, which file pairs with the document (a header's source, or a source's header).
// done is called once, from LspPump, unless the request is cancelled.
// returns 0, asking nothing, when the server is not ready or cannot answer such a question.
LspRequest
LspDefinition(LspDocument *document, Position position, LspLocationsDone done, void *context);

LspRequest
LspReferences(LspDocument *document, Position position, LspLocationsDone done, void *context);

LspRequest
LspHover(LspDocument *document, Position position, LspTextDone done, void *context);

LspRequest
LspOtherFile(LspDocument *document, LspLocationsDone done, void *context);

// asks the document's server whether the symbol at position can be renamed, answering
// the range of its name (one location), an empty range at position when the server leaves
// the name to the editor, or no location when it cannot be renamed.
// returns 0, asking nothing, when the server does not check names before renaming.
LspRequest
LspPrepareRename(
  LspDocument *document,
  Position position,
  LspLocationsDone done,
  void *context
);

// asks the document's server for the changes that rename the symbol at position to name,
// across the files it knows.
LspRequest
LspRename(
  LspDocument *document,
  Position position,
  const char *name,
  u64 length,
  LspEditsDone done,
  void *context
);

// asks the document's server for the fixes of the diagnostics reaching line.
LspRequest
LspFixes(LspDocument *document, u64 line, LspActionsDone done, void *context);

// asks the document's server for the changes that format the whole document, indenting
// with spaces (or tabs) to tabWidth.
LspRequest
LspFormat(LspDocument *document, bool spaces, LspEditsDone done, void *context);

// true when the document's server can rename symbols.
bool
LspRenames(const LspDocument *document);

// stops waiting for a request, telling its server, so done is never called.
void
LspCancel(LspRequest request);

// milliseconds until the first request runs out of time, or -1 when none is waiting.
i32
LspTimeout(void);

// appends what the status bar says about the document's server: " (clangd)", or with
// "starting" or "stopped" after the name.
void
LspDescribe(const LspDocument *document, StrBuf *text);

// the descriptors the main loop should poll for the servers, up to lspMaxPollFds.
u32
LspPollFds(struct pollfd *fds);

// sends the changes collected since the last pass, and handles what the servers sent.
void
LspPump(void);

// asks every server to shut down and waits at most serverShutdownTime for them to exit
// before ending them.
void
LspStopAll(void);
