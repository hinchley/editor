#include "lsp.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "memory.h"
#include "rpc.h"
#include "settings.h"
#include "utf8.h"

// each server polls at most two descriptors.
static constexpr u32 maxServers = lspMaxPollFds / 2;

enum ServerState {
  // initialize was sent and no reply has come.
  ServerStarting,
  ServerReady,

  // it could not start, refused to or stopped, and stays so until servers are turned off
  // and on again.
  ServerStopped
};

// how a server wants changes sent (the LSP TextDocumentSyncKind).
enum ServerSync {
  ServerSyncNone,
  ServerSyncFull,
  ServerSyncIncremental
};

typedef struct Server {
  const Lang *lang;
  StrBuf root;

  // the program's name, for messages and the status bar.
  StrBuf name;
  Rpc *rpc;
  enum ServerState state;

  // from its capabilities: positions in utf-16 code units rather than bytes, how it takes
  // changes, and whether it wants to hear about saves.
  bool utf16;
  enum ServerSync sync;
  bool wantsSave;

  // the documents attached to it, and whether it answered shutdown.
  u32 documentCount;
  bool shutDown;
} Server;

struct LspDocument {
  Server *server;
  Buffer *buffer;
  const Lang *lang;
  StrBuf path;
  StrBuf uri;
  i64 version;

  // the server has been sent didOpen, which happens once it is ready.
  bool opened;

  // the changes since the last didChange, as json objects separated by commas, or the
  // whole text when full is set.
  StrBuf changes;
  u32 changeCount;
  bool full;
};

static StrBuf *message;
static bool enabled = languageServersDefault;

// the folder the editor started in, the root of a file with no root marker above it.
static StrBuf startFolder;

static Server *servers[maxServers];
static u32 serverCount;

static LspDocument **documents;
static u32 documentCount;
static u32 documentCapacity;

void
LspInit(StrBuf *messageBar)
{
  message = messageBar;
  char *folder = getcwd(nullptr, 0);
  StrBufAppendString(&startFolder, folder ? folder : "/");
  free(folder);
}

bool
LspEnabled(void)
{
  return enabled;
}

// the message bar, after whatever it holds already, for a report of its own.
static StrBuf *
Report(void)
{
  if (message->length) StrBufAppendString(message, "; ");
  return message;
}

// file:// and the path with every byte other than letters, digits, "-._~" and "/"
// percent encoded.
static void
AppendUri(StrBuf *uri, const char *path)
{
  StrBufAppendString(uri, "file://");

  for (const char *at = path; *at; at++) {
    u8 byte = (u8)*at;
    bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
      (byte >= '0' && byte <= '9') || strchr("-._~/", byte);
    if (plain) StrBufAppendByte(uri, (char)byte);
    else StrBufAppendFormat(uri, "%%%02X", byte);
  }
}

// the absolute path of a file, which need not exist yet (a new file) as long as its folder
// does.
static bool
AbsolutePath(const char *path, StrBuf *absolute)
{
  char *real = realpath(path, nullptr);

  if (!real) {
    StrBuf copy = {};
    StrBufAppendString(&copy, path);
    real = realpath(dirname(copy.data), nullptr);
    StrBufClear(&copy);
    StrBufAppendString(&copy, path);
    bool found = real != nullptr;
    if (found) StrBufAppendFormat(absolute, "%s/%s", real, basename(copy.data));
    StrBufFree(&copy);
    free(real);
    return found;
  }

  StrBufAppendString(absolute, real);
  free(real);
  return true;
}

// the first folder above the file at path holding one of the root names, or the folder
// the editor started in.
static void
FindRoot(const char *path, const char *const *names, StrBuf *root)
{
  StrBufAppendString(root, path);
  StrBuf probe = {};

  while (true) {
    char *slash = strrchr(root->data, '/');
    if (!slash) break;

    StrBufTruncate(root, (u64)(slash - root->data));

    for (u32 index = 0; names[index]; index++) {
      StrBufClear(&probe);
      StrBufAppendFormat(&probe, "%s/%s", root->data, names[index]);

      if (access(probe.data, F_OK) == 0) {
        if (root->length == 0) StrBufAppendByte(root, '/');
        StrBufFree(&probe);
        return;
      }
    }
  }

  StrBufFree(&probe);
  StrBufClear(root);
  StrBufAppend(root, startFolder.data, startFolder.length);
}

// the server's standard error goes to <state folder>/editor/<language id>.log, where the
// state folder is $XDG_STATE_HOME or ~/.local/state; false when the folder cannot be made.
static bool
LogPath(const Lang *lang, StrBuf *path)
{
  const char *state = getenv("XDG_STATE_HOME");
  const char *home = getenv("HOME");
  if (state && *state) StrBufAppendFormat(path, "%s/editor", state);
  else if (home && *home) StrBufAppendFormat(path, "%s/.local/state/editor", home);
  else return false;

  // makes each folder along the way.
  for (u64 at = 1; at <= path->length; at++) {
    if (at < path->length && path->data[at] != '/') continue;

    char saved = path->data[at];
    path->data[at] = '\0';
    bool made = mkdir(path->data, 0755) == 0 || errno == EEXIST;
    path->data[at] = saved;
    if (!made) return false;
  }

  StrBufAppendFormat(path, "/%s.log", lang->id);
  return true;
}

// appends the whole text of a buffer as a json string.
static void
AppendText(StrBuf *out, const Buffer *buffer)
{
  StrBuf text = {};
  BufferGetText(buffer, (Range){{0, 0}, BufferEnd(buffer)}, &text);
  JsonAppendString(out, text.data ? text.data : "", text.length);
  StrBufFree(&text);
}

static void
SendOpen(LspDocument *document)
{
  StrBuf params = {};
  StrBufAppendString(&params, "{\"textDocument\":{\"uri\":");
  JsonAppendString(&params, document->uri.data, document->uri.length);
  StrBufAppendString(&params, ",\"languageId\":");
  JsonAppendString(&params, document->lang->id, strlen(document->lang->id));
  StrBufAppendFormat(&params, ",\"version\":%ld,\"text\":", document->version);
  AppendText(&params, document->buffer);
  StrBufAppendString(&params, "}}");
  RpcNotify(document->server->rpc, "textDocument/didOpen", params.data);
  StrBufFree(&params);

  document->opened = true;
  document->full = false;
  document->changeCount = 0;
  StrBufClear(&document->changes);
}

// sends the changes collected since the last time, if any.
static void
SendChanges(LspDocument *document)
{
  Server *server = document->server;
  bool pending = document->full || document->changeCount;
  if (!pending || !document->opened || server->state != ServerReady) return;

  document->version++;
  StrBuf params = {};
  StrBufAppendString(&params, "{\"textDocument\":{\"uri\":");
  JsonAppendString(&params, document->uri.data, document->uri.length);
  StrBufAppendFormat(&params, ",\"version\":%ld},\"contentChanges\":[", document->version);

  if (document->full) {
    StrBufAppendString(&params, "{\"text\":");
    AppendText(&params, document->buffer);
    StrBufAppendByte(&params, '}');
  } else {
    StrBufAppend(&params, document->changes.data, document->changes.length);
  }

  StrBufAppendString(&params, "]}");
  RpcNotify(server->rpc, "textDocument/didChange", params.data);
  StrBufFree(&params);

  document->full = false;
  document->changeCount = 0;
  StrBufClear(&document->changes);
}

// the utf-16 code units of text, which is how servers that do not take utf-8 count
// columns.
static u64
Utf16Length(const char *text, u64 length)
{
  u64 units = 0;

  for (u64 at = 0; at < length;) {
    u32 codepoint;
    at += Utf8Decode(text + at, length - at, &codepoint);
    units += codepoint >= 0x10000 ? 2 : 1;
  }

  return units;
}

static void
AppendPosition(StrBuf *out, u64 line, u64 character)
{
  StrBufAppendFormat(out, "{\"line\":%lu,\"character\":%lu}", line, character);
}

// records a change as the server will want it: a range of the text before the change and
// the text that replaced it.
static void
Listen(void *context, const BufferChange *change)
{
  LspDocument *document = context;
  Server *server = document->server;
  if (!document->opened || document->full || server->sync == ServerSyncNone) return;

  if (server->sync == ServerSyncFull || document->changeCount == maxServerChanges) {
    document->full = true;
    document->changeCount = 0;
    StrBufClear(&document->changes);
    return;
  }

  u64 start = change->start.byte;
  u64 end = change->oldEnd.byte;

  // the start's line is the same before and after the change up to the start, and the
  // end's column comes from the old text's last line.
  if (server->utf16) {
    const StrBuf *line = BufferLine(document->buffer, change->start.line);
    start = Utf16Length(line->data, change->start.byte);
    const char *old = change->oldText;
    u64 oldLength = change->oldLength;
    u64 tail = oldLength;
    while (tail > 0 && old[tail - 1] != '\n') tail--;

    if (change->oldEnd.line > change->start.line) end = Utf16Length(old + tail, oldLength - tail);
    else end = start + Utf16Length(old, oldLength);
  }

  StrBuf *changes = &document->changes;
  if (document->changeCount++) StrBufAppendByte(changes, ',');
  StrBufAppendString(changes, "{\"range\":{\"start\":");
  AppendPosition(changes, change->start.line, start);
  StrBufAppendString(changes, ",\"end\":");
  AppendPosition(changes, change->oldEnd.line, end);
  StrBufAppendString(changes, "},\"text\":");
  JsonAppendString(changes, change->newText, change->newLength);
  StrBufAppendByte(changes, '}');
}

// reads the capabilities the server answered initialize with, and opens the documents
// that were waiting for it.
static void
Initialized(void *context, const Json *json, u32 result, u32 error)
{
  Server *server = context;

  // a server that ended is noticed by LspPump.
  if (!json) return;

  if (result == jsonNone) {
    StrBuf reason = {};
    JsonString(json, JsonGet(json, error, "message"), &reason);
    const char *why = reason.data ? reason.data : "";
    StrBufAppendFormat(Report(), "%s: initialize failed: %s", server->name.data, why);
    StrBufFree(&reason);
    server->state = ServerStopped;
    return;
  }

  u32 capabilities = JsonGet(json, result, "capabilities");
  server->utf16 = !JsonEquals(json, JsonGet(json, capabilities, "positionEncoding"), "utf-8");

  // textDocumentSync is either the kind of change, or options holding it.
  u32 sync = JsonGet(json, capabilities, "textDocumentSync");
  i64 kind = JsonInteger(json, sync, ServerSyncNone);
  bool wantsSave = false;

  if (JsonTypeOf(json, sync) == JsonTypeObject) {
    kind = JsonInteger(json, JsonGet(json, sync, "change"), ServerSyncNone);
    u32 save = JsonGet(json, sync, "save");
    wantsSave = JsonTrue(json, save) || JsonTypeOf(json, save) == JsonTypeObject;
  }

  server->sync = kind == ServerSyncIncremental ? ServerSyncIncremental
    : kind == ServerSyncFull ? ServerSyncFull : ServerSyncNone;
  server->wantsSave = wantsSave;
  server->state = ServerReady;
  RpcNotify(server->rpc, "initialized", "{}");

  for (u32 index = 0; index < documentCount; index++) {
    if (documents[index]->server == server) SendOpen(documents[index]);
  }
}

// answers what a server asks of the editor, and takes in what it reports.
static void
Handle(void *context, const Json *json, u32 method, u32 params, u32 id)
{
  Server *server = context;

  if (id != jsonNone) {
    // configuration is asked for as a list of sections, and the editor has none of them.
    if (JsonEquals(json, method, "workspace/configuration")) {
      u32 count = JsonSize(json, JsonGet(json, params, "items"));
      StrBuf result = {};
      StrBufAppendByte(&result, '[');

      for (u32 index = 0; index < count; index++) {
        StrBufAppendString(&result, index ? ",null" : "null");
      }

      StrBufAppendByte(&result, ']');
      RpcRespond(server->rpc, json, id, result.data);
      StrBufFree(&result);
      return;
    }

    bool acknowledged = JsonEquals(json, method, "window/workDoneProgress/create") ||
      JsonEquals(json, method, "client/registerCapability") ||
      JsonEquals(json, method, "client/unregisterCapability") ||
      JsonEquals(json, method, "window/showMessageRequest");

    if (acknowledged) RpcRespond(server->rpc, json, id, "null");
    else if (JsonEquals(json, method, "workspace/applyEdit")) {
      RpcRespond(server->rpc, json, id, "{\"applied\":false}");
    } else {
      RpcRespond(server->rpc, json, id, nullptr);
    }

    return;
  }

  // only errors are worth the message bar (type 1 is an error).
  if (JsonEquals(json, method, "window/showMessage")) {
    if (JsonInteger(json, JsonGet(json, params, "type"), 0) != 1) return;

    StrBuf text = {};
    JsonString(json, JsonGet(json, params, "message"), &text);
    StrBufAppendFormat(Report(), "%s: %s", server->name.data, text.data ? text.data : "");
    StrBufFree(&text);
  }
}

static void
SendInitialize(Server *server)
{
  StrBuf uri = {};
  AppendUri(&uri, server->root.data);
  StrBuf params = {};
  StrBufAppendFormat(&params, "{\"processId\":%d,\"clientInfo\":{\"name\":\"editor\"}",
    (i32)getpid());
  StrBufAppendString(&params, ",\"rootUri\":");
  JsonAppendString(&params, uri.data, uri.length);
  StrBufAppendString(&params, ",\"workspaceFolders\":[{\"uri\":");
  JsonAppendString(&params, uri.data, uri.length);
  StrBufAppendString(&params, ",\"name\":");
  const char *slash = strrchr(server->root.data, '/');
  const char *name = slash && slash[1] ? slash + 1 : server->root.data;
  JsonAppendString(&params, name, strlen(name));
  StrBufAppendString(&params,
    "}],\"capabilities\":{"
      "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
      "\"workspace\":{\"workspaceFolders\":true},"
      "\"textDocument\":{\"synchronization\":{\"didSave\":true}}"
    "}}");
  RpcRequest(server->rpc, "initialize", params.data, Initialized, server);
  StrBufFree(&params);
  StrBufFree(&uri);
}

// starts the language's server in root, or records that it could not be started.
static Server *
StartServer(const Lang *lang, const char *root)
{
  Server *server = MemoryAllocateZeroed(1, sizeof(Server));
  server->lang = lang;
  StrBufAppendString(&server->root, root);

  // the words of the server line, as the program's arguments.
  StrBuf words = {};
  StrBufAppendString(&words, lang->server);
  char **arguments = MemoryAllocateZeroed(words.length / 2 + 2, sizeof(char *));
  u32 count = 0;

  for (char *at = words.data; *at;) {
    while (*at == ' ' || *at == '\t') *at++ = '\0';
    if (!*at) break;

    arguments[count++] = at;
    while (*at && *at != ' ' && *at != '\t') at++;
  }

  const char *slash = strrchr(arguments[0], '/');
  StrBufAppendString(&server->name, slash ? slash + 1 : arguments[0]);

  StrBuf log = {};
  bool logged = LogPath(lang, &log);
  server->rpc = RpcStart(arguments, root, logged ? log.data : nullptr, Handle, server);
  StrBufFree(&log);
  free(arguments);
  StrBufFree(&words);

  if (server->rpc) {
    SendInitialize(server);
  } else {
    server->state = ServerStopped;
    StrBufAppendFormat(Report(), "%s: cannot start (is it installed?)", server->name.data);
  }

  servers[serverCount++] = server;
  return server;
}

static void
FreeServer(Server *server)
{
  for (u32 index = 0; index < serverCount; index++) {
    if (servers[index] != server) continue;

    servers[index] = servers[--serverCount];
    break;
  }

  RpcFree(server->rpc, RpcNow());
  StrBufFree(&server->root);
  StrBufFree(&server->name);
  free(server);
}

// a server that stopped no longer knows its documents.
static void
Stopped(Server *server)
{
  RpcFree(server->rpc, RpcNow());
  server->rpc = nullptr;
  server->state = ServerStopped;

  for (u32 index = 0; index < documentCount; index++) {
    if (documents[index]->server == server) documents[index]->opened = false;
  }
}

static u64
BufferSize(const Buffer *buffer)
{
  u64 size = BufferLineCount(buffer) - 1;

  for (u64 line = 0; line < BufferLineCount(buffer); line++) {
    size += BufferLine(buffer, line)->length;
  }

  return size;
}

LspDocument *
LspOpen(Buffer *buffer, const Lang *lang, const char *path)
{
  if (!enabled || !lang || !lang->server || !path) return nullptr;
  if (BufferSize(buffer) > maxServerFileSize) return nullptr;

  StrBuf absolute = {};

  if (!AbsolutePath(path, &absolute)) {
    StrBufFree(&absolute);
    return nullptr;
  }

  StrBuf root = {};
  FindRoot(absolute.data, lang->serverRoots, &root);
  Server *server = nullptr;

  for (u32 index = 0; index < serverCount && !server; index++) {
    Server *candidate = servers[index];
    bool same = candidate->lang == lang && strcmp(candidate->root.data, root.data) == 0;
    if (same) server = candidate;
  }

  if (!server && serverCount == maxServers) {
    StrBufAppendFormat(Report(), "%s: too many language servers", lang->server);
    StrBufFree(&root);
    StrBufFree(&absolute);
    return nullptr;
  }

  if (!server) server = StartServer(lang, root.data);
  StrBufFree(&root);

  LspDocument *document = MemoryAllocateZeroed(1, sizeof(LspDocument));
  document->server = server;
  document->buffer = buffer;
  document->lang = lang;
  StrBufAppendString(&document->path, path);
  AppendUri(&document->uri, absolute.data);
  StrBufFree(&absolute);

  if (documentCount == documentCapacity) {
    documentCapacity = documentCapacity ? documentCapacity * 2 : 16;
    documents = MemoryResize(documents, documentCapacity, sizeof(LspDocument *));
  }

  documents[documentCount++] = document;
  server->documentCount++;
  BufferListen(buffer, Listen, document);
  if (server->state == ServerReady) SendOpen(document);
  return document;
}

void
LspClose(LspDocument *document)
{
  if (!document) return;

  Server *server = document->server;
  BufferUnlisten(document->buffer, Listen, document);

  if (document->opened && server->state == ServerReady) {
    StrBuf params = {};
    StrBufAppendString(&params, "{\"textDocument\":{\"uri\":");
    JsonAppendString(&params, document->uri.data, document->uri.length);
    StrBufAppendString(&params, "}}");
    RpcNotify(server->rpc, "textDocument/didClose", params.data);
    StrBufFree(&params);
  }

  for (u32 index = 0; index < documentCount; index++) {
    if (documents[index] != document) continue;

    documents[index] = documents[--documentCount];
    break;
  }

  // a server that stopped goes once nothing shows it; a running one stays for the next
  // file.
  if (--server->documentCount == 0 && server->state == ServerStopped) FreeServer(server);

  StrBufFree(&document->path);
  StrBufFree(&document->uri);
  StrBufFree(&document->changes);
  free(document);
}

bool
LspFollows(const LspDocument *document, const Lang *lang, const char *path)
{
  return document && path && document->lang == lang && strcmp(document->path.data, path) == 0;
}

void
LspSaved(LspDocument *document)
{
  if (!document || !document->opened || !document->server->wantsSave) return;

  // the server hears about the text before it hears it was saved.
  SendChanges(document);
  StrBuf params = {};
  StrBufAppendString(&params, "{\"textDocument\":{\"uri\":");
  JsonAppendString(&params, document->uri.data, document->uri.length);
  StrBufAppendString(&params, "}}");
  RpcNotify(document->server->rpc, "textDocument/didSave", params.data);
  StrBufFree(&params);
}

void
LspReloaded(LspDocument *document)
{
  if (!document || !document->opened) return;

  document->full = true;
  document->changeCount = 0;
  StrBufClear(&document->changes);
}

void
LspDescribe(const LspDocument *document, StrBuf *text)
{
  if (!document) return;

  const Server *server = document->server;
  StrBufAppendFormat(text, " (%s", server->name.data);
  if (server->state == ServerStarting) StrBufAppendString(text, " starting");
  if (server->state == ServerStopped) StrBufAppendString(text, " stopped");
  StrBufAppendByte(text, ')');
}

u32
LspPollFds(struct pollfd *fds)
{
  u32 count = 0;

  for (u32 index = 0; index < serverCount; index++) {
    if (servers[index]->rpc) count += RpcPollFds(servers[index]->rpc, fds + count);
  }

  return count;
}

void
LspPump(void)
{
  for (u32 index = 0; index < documentCount; index++) SendChanges(documents[index]);

  for (u32 index = 0; index < serverCount; index++) {
    Server *server = servers[index];
    if (!server->rpc) continue;

    RpcPump(server->rpc);
    if (RpcAlive(server->rpc) && server->state != ServerStopped) continue;

    // a server that refused to initialize said why when it did.
    bool crashed = server->state != ServerStopped;
    Stopped(server);
    if (crashed) StrBufAppendFormat(Report(), "%s stopped (alt+v restarts it)", server->name.data);
  }
}

bool
LspAnyStopped(void)
{
  for (u32 index = 0; index < serverCount; index++) {
    if (servers[index]->state == ServerStopped) return true;
  }

  return false;
}

static void
ShutDown(void *context, [[maybe_unused]] const Json *json, [[maybe_unused]] u32 result,
  [[maybe_unused]] u32 error)
{
  Server *server = context;
  server->shutDown = true;
}

void
LspStopAll(void)
{
  i64 deadline = RpcNow() + serverShutdownTime;

  // shutdown is a request, so its reply says the server is ready to exit.
  for (u32 index = 0; index < serverCount; index++) {
    Server *server = servers[index];
    server->shutDown = server->state != ServerReady;
    if (!server->shutDown) RpcRequest(server->rpc, "shutdown", nullptr, ShutDown, server);
  }

  while (RpcNow() < deadline) {
    struct pollfd fds[lspMaxPollFds];
    u32 count = 0;
    bool waiting = false;

    for (u32 index = 0; index < serverCount; index++) {
      Server *server = servers[index];
      if (server->shutDown || !server->rpc) continue;

      waiting = true;
      count += RpcPollFds(server->rpc, fds + count);
    }

    if (!waiting) break;

    poll(fds, count, (i32)(deadline - RpcNow()));
    for (u32 index = 0; index < serverCount; index++) {
      if (servers[index]->rpc) RpcPump(servers[index]->rpc);
    }
  }

  for (u32 index = 0; index < serverCount; index++) {
    Server *server = servers[index];
    if (!server->rpc) continue;

    if (server->state == ServerReady) {
      RpcNotify(server->rpc, "exit", nullptr);
      RpcPump(server->rpc);
    }

    RpcFree(server->rpc, deadline);
    server->rpc = nullptr;
    server->state = ServerStopped;
  }

  for (u32 index = 0; index < documentCount; index++) documents[index]->opened = false;

  // servers no document shows are gone; the others go with their last document.
  for (u32 index = serverCount; index-- > 0;) {
    if (servers[index]->documentCount == 0) FreeServer(servers[index]);
  }
}

void
LspSetEnabled(bool on)
{
  if (enabled && !on) LspStopAll();
  enabled = on;
}
