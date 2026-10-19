#pragma once

// lsp: the language server client.
// a language whose package names a server gets one process for each root folder, started
// when the first file of the language under that root opens (see ARCHITECTURE.md,
// "Language servers"). each open document of the language has an LspDocument, which
// tells the server about the document and follows its buffer's changes.
// changes are collected as they happen and sent once per pass of the main loop, so a
// burst of edits costs one message. nothing waits for a server: its output wakes the
// main loop's poll, and LspPump handles it.
#include <poll.h>

#include "buffer.h"
#include "lang.h"
#include "strbuf.h"
#include "types.h"

typedef struct LspDocument LspDocument;

// the most descriptors LspPollFds fills.
static constexpr u32 lspMaxPollFds = 16;

// reports what the servers have to say (failures, error messages) to message.
void
LspInit(StrBuf *message);

// whether servers are on, which LspOpen needs.
bool
LspEnabled(void);

// turns servers on or off. turning them off stops every server, after which each
// document should be closed (and opened again once they are on).
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
