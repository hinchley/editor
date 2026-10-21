#include "lsp.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file.h"
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
  // changes, whether it wants to hear about saves, which questions it answers, and whether
  // it is clangd (which has requests of its own).
  bool utf16;
  enum ServerSync sync;
  bool wantsSave;
  bool definitions;
  bool references;
  bool hovers;
  bool renames;
  bool preparesRenames;
  bool fixes;
  bool formats;
  bool clangd;

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

  Diagnostics diagnostics;
};

static StrBuf *message;
static bool enabled = languageServersDefault;

// the folder the editor started in, the root of a file with no root marker above it.
static StrBuf startFolder;

static Server *servers[maxServers];
static u32 serverCount;

// a request a command waits on: the server asked, its id there, when it runs out of time,
// and what to call with the answer.
// it also keeps the uri of the document asked about, for an answer of ranges in it.
enum PendingKind {
  PendingLocations,
  PendingText,
  PendingPrepare,
  PendingEdits,
  PendingActions
};

typedef struct Pending {
  LspRequest request;
  Server *server;
  u64 id;
  i64 deadline;
  enum PendingKind kind;
  StrBuf uri;
  Position position;
  LspLocationsDone locationsDone;
  LspTextDone textDone;
  LspEditsDone editsDone;
  LspActionsDone actionsDone;
  void *context;
} Pending;

static Pending *pendings;
static u32 pendingCount;
static u32 pendingCapacity;
static LspRequest lastRequest;

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
  DiagnosticsAdjust(&document->diagnostics, change);
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

    if (change->oldEnd.line > change->start.line) {
      end = Utf16Length(old + tail, oldLength - tail);
    } else end = start + Utf16Length(old, oldLength);
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
    : kind == ServerSyncFull                   ? ServerSyncFull
                                               : ServerSyncNone;
  server->wantsSave = wantsSave;

  // a provider is either true or options.
  const char *providers[] = {
    "definitionProvider",
    "referencesProvider",
    "hoverProvider",
    "renameProvider",
    "codeActionProvider",
    "documentFormattingProvider"
  };
  bool *answers[] = {
    &server->definitions,
    &server->references,
    &server->hovers,
    &server->renames,
    &server->fixes,
    &server->formats
  };

  for (u32 index = 0; index < sizeof(providers) / sizeof(providers[0]); index++) {
    u32 provider = JsonGet(json, capabilities, providers[index]);
    *answers[index] = JsonTrue(json, provider) || JsonTypeOf(json, provider) == JsonTypeObject;
  }

  u32 rename = JsonGet(json, capabilities, "renameProvider");
  server->preparesRenames = JsonTrue(json, JsonGet(json, rename, "prepareProvider"));

  u32 serverInfo = JsonGet(json, result, "serverInfo");
  server->clangd = JsonEquals(json, JsonGet(json, serverInfo, "name"), "clangd");
  server->state = ServerReady;
  RpcNotify(server->rpc, "initialized", "{}");

  for (u32 index = 0; index < documentCount; index++) {
    if (documents[index]->server == server) SendOpen(documents[index]);
  }
}

// the byte at a server's column on a line: the column itself in utf-8, or else the byte
// after that many utf-16 code units; either way no further than the line's end.
static u64
ByteOfColumn(const Server *server, const char *line, u64 length, u64 column)
{
  if (!server->utf16) return column < length ? column : length;

  u64 units = 0;
  u64 at = 0;

  while (at < length && units < column) {
    u32 codepoint;
    at += Utf8Decode(line + at, length - at, &codepoint);
    units += codepoint >= 0x10000 ? 2 : 1;
  }

  return at;
}

// a position a server gave in a document, kept inside its text.
static Position
ReadPosition(const LspDocument *document, const Json *json, u32 node)
{
  const Buffer *buffer = document->buffer;
  i64 line = JsonInteger(json, JsonGet(json, node, "line"), 0);
  i64 column = JsonInteger(json, JsonGet(json, node, "character"), 0);
  if (line < 0) line = 0;
  if (column < 0) column = 0;
  if ((u64)line >= BufferLineCount(buffer)) return BufferEnd(buffer);

  const StrBuf *text = BufferLine(buffer, (u64)line);
  u64 byte = ByteOfColumn(document->server, text->data, text->length, (u64)column);
  return (Position){(u64)line, byte};
}

// appends the path a file:// uri names, decoding its percent escapes, and leaving out the
// folder the editor started in when the path is inside it.
static void
AppendPathOfUri(StrBuf *path, const char *uri, u64 length)
{
  static constexpr char scheme[] = "file://";
  u64 at = 0;
  if (length >= sizeof(scheme) - 1 && memcmp(uri, scheme, sizeof(scheme) - 1) == 0) {
    at = sizeof(scheme) - 1;
  }

  u64 start = path->length;

  for (; at < length; at++) {
    char byte = uri[at];
    u32 value = 0;
    bool escaped = byte == '%' && at + 2 < length;

    for (u64 digit = at + 1; escaped && digit <= at + 2; digit++) {
      char hex = uri[digit];
      bool decimal = hex >= '0' && hex <= '9';
      bool letter = (hex | 0x20) >= 'a' && (hex | 0x20) <= 'f';
      escaped = decimal || letter;
      value = value << 4 | (u32)(decimal ? hex - '0' : (hex | 0x20) - 'a' + 10);
    }

    if (escaped) {
      byte = (char)value;
      at += 2;
    }

    StrBufAppendByte(path, byte);
  }

  u64 folder = startFolder.length;
  bool inside = path->length - start > folder + 1 &&
    memcmp(path->data + start, startFolder.data, folder) == 0 &&
    path->data[start + folder] == '/';
  if (inside) StrBufRemove(path, start, folder + 1);
}

// the related notes of a diagnostic, one "file:line: text" a line.
static void
ReadNotes(const Json *json, u32 related, StrBuf *notes)
{
  StrBuf text = {};

  u32 note = JsonFirst(json, related);

  for (; note != jsonNone; note = JsonNext(json, related, note)) {
    u32 location = JsonGet(json, note, "location");
    StrBufClear(&text);
    JsonString(json, JsonGet(json, location, "uri"), &text);
    if (notes->length) StrBufAppendByte(notes, '\n');
    AppendPathOfUri(notes, text.data ? text.data : "", text.length);
    u32 start = JsonGet(json, JsonGet(json, location, "range"), "start");
    StrBufAppendFormat(
      notes,
      ":%ld: ",
      JsonInteger(json, JsonGet(json, start, "line"), 0) + 1
    );
    JsonString(json, JsonGet(json, note, "message"), notes);
  }

  StrBufFree(&text);
}

// replaces a document's diagnostics with those the server just published, unless they
// are for text older than the document's (newer changes are pending, or were sent), as
// another publication will follow for those.
static void
Publish(Server *server, const Json *json, u32 params)
{
  StrBuf uri = {};
  JsonString(json, JsonGet(json, params, "uri"), &uri);
  LspDocument *document = nullptr;

  for (u32 index = 0; index < documentCount && !document; index++) {
    LspDocument *candidate = documents[index];
    bool same = candidate->server == server && candidate->uri.length == uri.length &&
      memcmp(candidate->uri.data, uri.data, uri.length) == 0;
    if (same) document = candidate;
  }

  StrBufFree(&uri);
  if (!document || document->full || document->changeCount) return;

  i64 version = JsonInteger(json, JsonGet(json, params, "version"), -1);
  if (version >= 0 && version != document->version) return;

  Diagnostics *diagnostics = &document->diagnostics;
  DiagnosticsClear(diagnostics);
  u32 list = JsonGet(json, params, "diagnostics");
  StrBuf text = {};

  for (u32 item = JsonFirst(json, list); item != jsonNone; item = JsonNext(json, list, item)) {
    u32 range = JsonGet(json, item, "range");
    i64 severity = JsonInteger(json, JsonGet(json, item, "severity"), 1);
    if (severity < DiagnosticSeverityError || severity > DiagnosticSeverityHint) severity = 1;

    Diagnostic diagnostic = {
      .range =
        {ReadPosition(document, json, JsonGet(json, range, "start")),
          ReadPosition(document, json, JsonGet(json, range, "end"))},
      .severity = (enum DiagnosticSeverity)severity
    };

    if (PositionCompare(diagnostic.range.end, diagnostic.range.start) < 0) {
      diagnostic.range.end = diagnostic.range.start;
    }

    StrBufClear(&text);
    JsonString(json, JsonGet(json, item, "message"), &text);
    diagnostic.message = DiagnosticsAddText(diagnostics, text.data, text.length);

    // a code may be a string or a number.
    StrBufClear(&text);
    u32 code = JsonGet(json, item, "code");
    if (!JsonString(json, code, &text) && JsonTypeOf(json, code) == JsonTypeNumber) {
      JsonAppendRaw(&text, json, code);
    }
    diagnostic.code = DiagnosticsAddText(diagnostics, text.data, text.length);

    StrBufClear(&text);
    JsonString(json, JsonGet(json, item, "source"), &text);
    diagnostic.source = DiagnosticsAddText(diagnostics, text.data, text.length);

    StrBufClear(&text);
    ReadNotes(json, JsonGet(json, item, "relatedInformation"), &text);
    diagnostic.notes = DiagnosticsAddText(diagnostics, text.data, text.length);
    DiagnosticsAdd(diagnostics, &diagnostic);
  }

  StrBufFree(&text);
  DiagnosticsSort(diagnostics);
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

  if (JsonEquals(json, method, "textDocument/publishDiagnostics")) {
    Publish(server, json, params);
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
  StrBufAppendFormat(
    &params,
    "{\"processId\":%d,\"clientInfo\":{\"name\":\"editor\"}",
    (i32)getpid()
  );
  StrBufAppendString(&params, ",\"rootUri\":");
  JsonAppendString(&params, uri.data, uri.length);
  StrBufAppendString(&params, ",\"workspaceFolders\":[{\"uri\":");
  JsonAppendString(&params, uri.data, uri.length);
  StrBufAppendString(&params, ",\"name\":");
  const char *slash = strrchr(server->root.data, '/');
  const char *name = slash && slash[1] ? slash + 1 : server->root.data;
  JsonAppendString(&params, name, strlen(name));
  // clang-format off
  StrBufAppendString(
    &params,
    "}],\"capabilities\":{"
      "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
      "\"workspace\":{\"workspaceFolders\":true,\"workspaceEdit\":{\"documentChanges\":true}},"
      "\"textDocument\":{"
        "\"synchronization\":{\"didSave\":true},"
        "\"publishDiagnostics\":{\"relatedInformation\":true,\"versionSupport\":true},"
        "\"definition\":{},\"references\":{},"
        "\"hover\":{\"contentFormat\":[\"markdown\",\"plaintext\"]},"
        "\"rename\":{\"prepareSupport\":true},"
        "\"codeAction\":{\"isPreferredSupport\":true,\"codeActionLiteralSupport\":"
          "{\"codeActionKind\":{\"valueSet\":[\"quickfix\"]}}},"
        "\"formatting\":{}"
      "}"
    "}}"
  );
  // clang-format on
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

// a document its server no longer knows, and whose diagnostics are gone with it.
static void
Forget(LspDocument *document)
{
  document->opened = false;
  DiagnosticsClear(&document->diagnostics);
}

// takes the pending request at index out of the list, so its callback may ask again.
static Pending
TakePending(u32 index)
{
  Pending pending = pendings[index];
  pendings[index] = pendings[--pendingCount];
  return pending;
}

// tells the request's callback there is no answer, and lets the request go.
static void
Unanswered(Pending *pending)
{
  void *context = pending->context;

  switch (pending->kind) {
    case PendingLocations:
    case PendingPrepare:
      pending->locationsDone(context, nullptr, 0, false);
      break;
    case PendingText:
      pending->textDone(context, nullptr, 0, false);
      break;
    case PendingEdits:
      pending->editsDone(context, nullptr, 0, false);
      break;
    case PendingActions:
      pending->actionsDone(context, nullptr, 0, false);
      break;
  }

  StrBufFree(&pending->uri);
}

// the requests a server will not answer now that it stopped.
static void
FailPendings(Server *server)
{
  for (u32 index = pendingCount; index-- > 0;) {
    if (index >= pendingCount || pendings[index].server != server) continue;

    Pending pending = TakePending(index);
    Unanswered(&pending);
  }
}

// a server that stopped no longer knows its documents.
static void
Stopped(Server *server)
{
  RpcFree(server->rpc, RpcNow());
  server->rpc = nullptr;
  server->state = ServerStopped;
  FailPendings(server);

  for (u32 index = 0; index < documentCount; index++) {
    if (documents[index]->server == server) Forget(documents[index]);
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
  DiagnosticsFree(&document->diagnostics);
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

const Diagnostics *
LspDiagnostics(const LspDocument *document)
{
  return document ? &document->diagnostics : nullptr;
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

static void
Withdraw(const Pending *pending);

void
LspPump(void)
{
  for (u32 index = 0; index < documentCount; index++) SendChanges(documents[index]);

  // a request that ran out of time is given up on, and its server told.
  i64 now = RpcNow();

  for (u32 index = pendingCount; index-- > 0;) {
    if (index >= pendingCount || pendings[index].deadline > now) continue;

    Pending pending = TakePending(index);
    Withdraw(&pending);
    StrBufAppendFormat(Report(), "%s did not answer in time", pending.server->name.data);
    Unanswered(&pending);
  }

  for (u32 index = 0; index < serverCount; index++) {
    Server *server = servers[index];
    if (!server->rpc) continue;

    RpcPump(server->rpc);
    if (RpcAlive(server->rpc) && server->state != ServerStopped) continue;

    // a server that refused to initialize said why when it did.
    bool crashed = server->state != ServerStopped;
    Stopped(server);
    if (crashed) {
      StrBufAppendFormat(Report(), "%s stopped (alt+v restarts it)", server->name.data);
    }
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
ShutDown(
  void *context,
  [[maybe_unused]] const Json *json,
  [[maybe_unused]] u32 result,
  [[maybe_unused]] u32 error
)
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
    FailPendings(server);
  }

  for (u32 index = 0; index < documentCount; index++) Forget(documents[index]);

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

// tells a server it need not answer a request any more.
static void
Withdraw(const Pending *pending)
{
  Rpc *rpc = pending->server->rpc;
  if (!rpc) return;

  RpcForget(rpc, pending->id);
  StrBuf params = {};
  StrBufAppendFormat(&params, "{\"id\":%lu}", pending->id);
  RpcNotify(rpc, "$/cancelRequest", params.data);
  StrBufFree(&params);
}

// the position of a server's line and column in text read from a file.
static Position
PositionInText(const Server *server, const char *text, u64 length, u64 line, u64 column)
{
  u64 start = 0;

  for (u64 at = 0; at < line; at++) {
    const char *newline = memchr(text + start, '\n', length - start);
    if (!newline) return (Position){line, 0};
    start = (u64)(newline - text) + 1;
  }

  const char *newline = memchr(text + start, '\n', length - start);
  u64 end = newline ? (u64)(newline - text) : length;
  if (end > start && text[end - 1] == '\r') end--;
  return (Position){line, ByteOfColumn(server, text + start, end - start, column)};
}

// the open document of a server with uri, or nullptr.
static LspDocument *
DocumentOfUri(const Server *server, const char *uri, u64 length)
{
  for (u32 index = 0; index < documentCount; index++) {
    LspDocument *document = documents[index];
    bool same = document->server == server && document->uri.length == length &&
      memcmp(document->uri.data, uri, length) == 0;
    if (same) return document;
  }

  return nullptr;
}

// the file a utf-16 server's positions are converted in, read once for all its places.
typedef struct Converting {
  StrBuf uri;
  StrBuf text;
  bool read;
} Converting;

// a range a server gave in the file at uri, in bytes: from the open document when there
// is one, or else (for a utf-16 server) from the file as read.
static Range
ReadRange(
  const Server *server,
  const Json *json,
  u32 range,
  const StrBuf *uri,
  Converting *converting
)
{
  u32 ends[2] = {JsonGet(json, range, "start"), JsonGet(json, range, "end")};
  LspDocument *document = DocumentOfUri(server, uri->data, uri->length);
  Position positions[2];

  for (u32 index = 0; index < 2; index++) {
    if (document) {
      positions[index] = ReadPosition(document, json, ends[index]);
      continue;
    }

    i64 line = JsonInteger(json, JsonGet(json, ends[index], "line"), 0);
    i64 column = JsonInteger(json, JsonGet(json, ends[index], "character"), 0);
    positions[index] = (Position){line > 0 ? (u64)line : 0, column > 0 ? (u64)column : 0};
    if (!server->utf16) continue;

    bool same = converting->uri.length == uri->length &&
      memcmp(converting->uri.data, uri->data, uri->length) == 0;

    if (!same) {
      StrBufClear(&converting->uri);
      StrBufAppend(&converting->uri, uri->data, uri->length);
      StrBufClear(&converting->text);
      StrBuf path = {};
      AppendPathOfUri(&path, uri->data, uri->length);
      FileStamp stamp;
      converting->read = FileRead(path.data, &converting->text, &stamp, nullptr) ==
        FileStatusOk;
      StrBufFree(&path);
    }

    if (converting->read) {
      Position at = positions[index];
      positions[index] = PositionInText(
        server,
        converting->text.data,
        converting->text.length,
        at.line,
        at.byte
      );
    }
  }

  return (Range){positions[0], positions[1]};
}

// hands a server's answer of places (a location, a list of locations or links, a bare uri,
// or null) to the request's callback.
static void
AnswerLocations(const Pending *pending, const Json *json, u32 result)
{
  // a lone location (or uri) is read as a list of one.
  u32 count = JsonSize(json, result);
  enum JsonType type = JsonTypeOf(json, result);
  bool list = type == JsonTypeArray;
  bool one = type == JsonTypeObject || type == JsonTypeString;
  if (!list) count = one ? 1 : 0;

  LspLocation *locations = MemoryAllocate((count ? count : 1) * sizeof(LspLocation));
  u64 *starts = MemoryAllocate((count ? count : 1) * sizeof(u64));
  StrBuf paths = {};
  StrBuf uri = {};
  Converting converting = {};
  u32 item = list ? JsonFirst(json, result) : result;

  for (u32 index = 0; index < count; index++) {
    if (index) item = JsonNext(json, result, item);
    u32 range = jsonNone;
    StrBufClear(&uri);

    if (!JsonString(json, item, &uri)) {
      bool link = JsonGet(json, item, "targetUri") != jsonNone;
      JsonString(json, JsonGet(json, item, link ? "targetUri" : "uri"), &uri);
      range = JsonGet(json, item, link ? "targetSelectionRange" : "range");
    }

    starts[index] = paths.length;
    AppendPathOfUri(&paths, uri.data ? uri.data : "", uri.length);
    StrBufAppendByte(&paths, '\0');
    Range found = {};
    if (range != jsonNone) found = ReadRange(pending->server, json, range, &uri, &converting);
    locations[index] = (LspLocation){.range = found};
  }

  for (u32 index = 0; index < count; index++) {
    locations[index].path = paths.data + starts[index];
  }
  pending->locationsDone(pending->context, locations, count, true);

  free(locations);
  free(starts);
  StrBufFree(&paths);
  StrBufFree(&uri);
  StrBufFree(&converting.uri);
  StrBufFree(&converting.text);
}

// appends a piece of hover contents: markup's value, a string, or a marked string's code
// in a fence.
static void
AppendHover(StrBuf *text, const Json *json, u32 node)
{
  if (text->length) StrBufAppendString(text, "\n\n");
  if (JsonString(json, node, text)) return;

  u32 language = JsonGet(json, node, "language");
  if (language != jsonNone) StrBufAppendString(text, "```\n");
  JsonString(json, JsonGet(json, node, "value"), text);
  if (language != jsonNone) StrBufAppendString(text, "\n```");
}

static void
AnswerText(const Pending *pending, const Json *json, u32 result)
{
  StrBuf text = {};
  u32 contents = JsonGet(json, result, "contents");

  if (JsonTypeOf(json, contents) == JsonTypeArray) {
    u32 item = JsonFirst(json, contents);
    for (; item != jsonNone; item = JsonNext(json, contents, item)) {
      AppendHover(&text, json, item);
    }
  } else if (contents != jsonNone) {
    AppendHover(&text, json, contents);
  }

  pending->textDone(pending->context, text.data ? text.data : "", text.length, true);
  StrBufFree(&text);
}

// changes being read from an answer: the edits, with the offsets of their paths and texts
// in one string until every one is read, the file read to convert their positions, and
// how many file operations (creating, renaming, deleting) were left out.
typedef struct Collecting {
  LspEdit *edits;
  u64 *offsets;
  u32 count;
  u32 capacity;
  StrBuf strings;
  Converting converting;
  u32 skipped;
} Collecting;

// reads a list of text edits for the file at uri.
static void
CollectTextEdits(
  Collecting *collecting,
  const Server *server,
  const Json *json,
  const StrBuf *uri,
  u32 edits
)
{
  for (u32 item = JsonFirst(json, edits); item != jsonNone;
    item = JsonNext(json, edits, item)) {
    if (collecting->count == collecting->capacity) {
      collecting->capacity = collecting->capacity ? collecting->capacity * 2 : 32;
      collecting->edits =
        MemoryResize(collecting->edits, collecting->capacity, sizeof(LspEdit));
      collecting->offsets =
        MemoryResize(collecting->offsets, collecting->capacity * 2, sizeof(u64));
    }

    StrBuf *strings = &collecting->strings;
    u64 *offsets = &collecting->offsets[collecting->count * 2];
    offsets[0] = strings->length;
    AppendPathOfUri(strings, uri->data ? uri->data : "", uri->length);
    StrBufAppendByte(strings, '\0');
    offsets[1] = strings->length;
    JsonString(json, JsonGet(json, item, "newText"), strings);
    u64 length = strings->length - offsets[1];
    StrBufAppendByte(strings, '\0');

    u32 range = JsonGet(json, item, "range");
    Range found = ReadRange(server, json, range, uri, &collecting->converting);
    collecting->edits[collecting->count++] = (LspEdit){.range = found, .length = length};
  }
}

// reads a workspace edit: its changes by uri, or its document changes, leaving out the
// file operations among them.
static void
CollectWorkspaceEdit(Collecting *collecting, const Server *server, const Json *json, u32 edit)
{
  StrBuf uri = {};
  u32 changes = JsonGet(json, edit, "changes");

  for (u32 key = JsonFirst(json, changes); key != jsonNone;
    key = JsonNext(json, changes, key)) {
    StrBufClear(&uri);
    JsonString(json, key, &uri);
    CollectTextEdits(collecting, server, json, &uri, key + 1);
  }

  u32 changed = JsonGet(json, edit, "documentChanges");
  u32 item = JsonFirst(json, changed);

  for (; item != jsonNone; item = JsonNext(json, changed, item)) {
    u32 edits = JsonGet(json, item, "edits");

    if (edits == jsonNone) {
      collecting->skipped++;
      continue;
    }

    StrBufClear(&uri);
    JsonString(json, JsonGet(json, JsonGet(json, item, "textDocument"), "uri"), &uri);
    CollectTextEdits(collecting, server, json, &uri, edits);
  }

  StrBufFree(&uri);
}

// points the edits at their paths and texts, once every one is read.
static void
Collected(Collecting *collecting)
{
  for (u32 index = 0; index < collecting->count; index++) {
    collecting->edits[index].path = collecting->strings.data + collecting->offsets[index * 2];
    collecting->edits[index].text = collecting->strings.data +
      collecting->offsets[index * 2 + 1];
  }
}

static void
FreeCollecting(Collecting *collecting)
{
  free(collecting->edits);
  free(collecting->offsets);
  StrBufFree(&collecting->strings);
  StrBufFree(&collecting->converting.uri);
  StrBufFree(&collecting->converting.text);
}

// the answer to prepareRename: the name's range (alone, or with a placeholder), the
// editor's choice (defaultBehavior), or null when the symbol cannot be renamed.
static void
AnswerPrepare(const Pending *pending, const Json *json, u32 result)
{
  if (JsonTypeOf(json, result) != JsonTypeObject) {
    pending->locationsDone(pending->context, nullptr, 0, true);
    return;
  }

  StrBuf path = {};
  AppendPathOfUri(&path, pending->uri.data, pending->uri.length);
  LspLocation location = {.path = path.data, .range = {pending->position, pending->position}};
  u32 range = JsonGet(json, result, "range");
  if (range == jsonNone && JsonGet(json, result, "start") != jsonNone) range = result;

  if (range != jsonNone) {
    Converting converting = {};
    location.range = ReadRange(pending->server, json, range, &pending->uri, &converting);
    StrBufFree(&converting.uri);
    StrBufFree(&converting.text);
  }

  pending->locationsDone(pending->context, &location, 1, true);
  StrBufFree(&path);
}

// changes: a workspace edit (a rename), or a list of text edits of the document asked
// about (formatting); null is no change.
static void
AnswerEdits(const Pending *pending, const Json *json, u32 result)
{
  Collecting collecting = {};

  if (JsonTypeOf(json, result) == JsonTypeArray) {
    CollectTextEdits(&collecting, pending->server, json, &pending->uri, result);
  } else {
    CollectWorkspaceEdit(&collecting, pending->server, json, result);
  }

  if (collecting.skipped) {
    StrBufAppendFormat(Report(), "%u file operations left out", collecting.skipped);
  }

  Collected(&collecting);
  pending->editsDone(pending->context, collecting.edits, collecting.count, true);
  FreeCollecting(&collecting);
}

// fixes: the code actions that carry their changes (commands, which would need the server
// to change the files, are left out).
static void
AnswerActions(const Pending *pending, const Json *json, u32 result)
{
  Collecting collecting = {};
  StrBuf titles = {};
  u32 count = JsonSize(json, result);
  LspAction *actions = MemoryAllocate((count ? count : 1) * sizeof(LspAction));
  u64 *titleStarts = MemoryAllocate((count ? count : 1) * sizeof(u64));
  u32 *editStarts = MemoryAllocate((count ? count : 1) * sizeof(u32));
  u32 found = 0;
  u32 item = JsonFirst(json, result);

  for (; item != jsonNone; item = JsonNext(json, result, item)) {
    u32 edit = JsonGet(json, item, "edit");
    if (edit == jsonNone) continue;

    u32 start = collecting.count;
    CollectWorkspaceEdit(&collecting, pending->server, json, edit);
    if (collecting.count == start) continue;

    titleStarts[found] = titles.length;
    JsonString(json, JsonGet(json, item, "title"), &titles);
    StrBufAppendByte(&titles, '\0');
    editStarts[found] = start;
    actions[found++] = (LspAction){
      .editCount = collecting.count - start,
      .preferred = JsonTrue(json, JsonGet(json, item, "isPreferred"))
    };
  }

  Collected(&collecting);

  for (u32 index = 0; index < found; index++) {
    actions[index].title = titles.data + titleStarts[index];
    actions[index].edits = collecting.edits + editStarts[index];
  }

  pending->actionsDone(pending->context, actions, found, true);
  free(actions);
  free(titleStarts);
  free(editStarts);
  StrBufFree(&titles);
  FreeCollecting(&collecting);
}

static void
Replied(void *context, const Json *json, u32 result, u32 error)
{
  LspRequest request = (LspRequest)(uintptr_t)context;
  u32 index = 0;
  while (index < pendingCount && pendings[index].request != request) index++;
  if (index == pendingCount) return;

  Pending pending = TakePending(index);

  // a server that stopped is reported by LspPump, and one that dropped the request
  // because the text changed (-32801) or it was cancelled (-32800) needs no word.
  if (json && result == jsonNone) {
    i64 code = JsonInteger(json, JsonGet(json, error, "code"), 0);

    if (code != -32800 && code != -32801) {
      StrBuf reason = {};
      JsonString(json, JsonGet(json, error, "message"), &reason);
      StrBufAppendFormat(
        Report(),
        "%s: %s",
        pending.server->name.data,
        reason.data ? reason.data : "the request failed"
      );
      StrBufFree(&reason);
    }
  }

  if (!json || result == jsonNone) {
    Unanswered(&pending);
    return;
  }

  switch (pending.kind) {
    case PendingLocations:
      AnswerLocations(&pending, json, result);
      break;
    case PendingText:
      AnswerText(&pending, json, result);
      break;
    case PendingPrepare:
      AnswerPrepare(&pending, json, result);
      break;
    case PendingEdits:
      AnswerEdits(&pending, json, result);
      break;
    case PendingActions:
      AnswerActions(&pending, json, result);
      break;
  }

  StrBufFree(&pending.uri);
}

// sends a request about a document, once its pending changes are sent.
static LspRequest
Ask(
  LspDocument *document,
  bool answers,
  const char *method,
  const StrBuf *params,
  Pending pending
)
{
  if (!document || !document->opened || !answers) return 0;

  Server *server = document->server;
  if (server->state != ServerReady) return 0;

  SendChanges(document);
  pending.request = ++lastRequest;
  pending.server = server;
  pending.uri = (StrBuf){};
  StrBufAppend(&pending.uri, document->uri.data, document->uri.length);
  pending.deadline = RpcNow() + serverRequestTimeout;
  void *context = (void *)(uintptr_t)pending.request;
  pending.id = RpcRequest(server->rpc, method, params->data, Replied, context);

  if (pendingCount == pendingCapacity) {
    pendingCapacity = pendingCapacity ? pendingCapacity * 2 : 8;
    pendings = MemoryResize(pendings, pendingCapacity, sizeof(Pending));
  }

  pendings[pendingCount++] = pending;
  return pending.request;
}

// appends a position in a document as its server counts it.
static void
AppendServerPosition(StrBuf *params, const LspDocument *document, Position position)
{
  u64 column = position.byte;
  const Buffer *buffer = document->buffer;

  if (document->server->utf16 && position.line < BufferLineCount(buffer)) {
    const StrBuf *line = BufferLine(buffer, position.line);
    column =
      Utf16Length(line->data, position.byte < line->length ? position.byte : line->length);
  }

  AppendPosition(params, position.line, column);
}

static void
AppendDocument(StrBuf *params, const LspDocument *document)
{
  StrBufAppendString(params, "{\"textDocument\":{\"uri\":");
  JsonAppendString(params, document->uri.data, document->uri.length);
  StrBufAppendByte(params, '}');
}

// the parameters of a question about a position in a document.
static void
AppendPositionParams(StrBuf *params, const LspDocument *document, Position position)
{
  AppendDocument(params, document);
  StrBufAppendString(params, ",\"position\":");
  AppendServerPosition(params, document, position);
}

LspRequest
LspDefinition(LspDocument *document, Position position, LspLocationsDone done, void *context)
{
  if (!document) return 0;

  StrBuf params = {};
  AppendPositionParams(&params, document, position);
  StrBufAppendByte(&params, '}');
  Pending pending = {.kind = PendingLocations, .locationsDone = done, .context = context};
  bool answers = document->server->definitions;
  LspRequest request = Ask(document, answers, "textDocument/definition", &params, pending);
  StrBufFree(&params);
  return request;
}

LspRequest
LspReferences(LspDocument *document, Position position, LspLocationsDone done, void *context)
{
  if (!document) return 0;

  StrBuf params = {};
  AppendPositionParams(&params, document, position);
  StrBufAppendString(&params, ",\"context\":{\"includeDeclaration\":true}}");
  Pending pending = {.kind = PendingLocations, .locationsDone = done, .context = context};
  bool answers = document->server->references;
  LspRequest request = Ask(document, answers, "textDocument/references", &params, pending);
  StrBufFree(&params);
  return request;
}

LspRequest
LspHover(LspDocument *document, Position position, LspTextDone done, void *context)
{
  if (!document) return 0;

  StrBuf params = {};
  AppendPositionParams(&params, document, position);
  StrBufAppendByte(&params, '}');
  Pending pending = {.kind = PendingText, .textDone = done, .context = context};
  bool answers = document->server->hovers;
  LspRequest request = Ask(document, answers, "textDocument/hover", &params, pending);
  StrBufFree(&params);
  return request;
}

LspRequest
LspOtherFile(LspDocument *document, LspLocationsDone done, void *context)
{
  if (!document) return 0;

  StrBuf params = {};
  StrBufAppendString(&params, "{\"uri\":");
  JsonAppendString(&params, document->uri.data, document->uri.length);
  StrBufAppendByte(&params, '}');
  Pending pending = {.kind = PendingLocations, .locationsDone = done, .context = context};
  bool answers = document->server->clangd;
  LspRequest request =
    Ask(document, answers, "textDocument/switchSourceHeader", &params, pending);
  StrBufFree(&params);
  return request;
}

void
LspCancel(LspRequest request)
{
  for (u32 index = 0; index < pendingCount; index++) {
    if (pendings[index].request != request) continue;

    Pending pending = TakePending(index);
    Withdraw(&pending);
    StrBufFree(&pending.uri);
    return;
  }
}

i32
LspTimeout(void)
{
  if (!pendingCount) return -1;

  i64 first = pendings[0].deadline;
  for (u32 index = 1; index < pendingCount; index++) {
    if (pendings[index].deadline < first) first = pendings[index].deadline;
  }

  i64 left = first - RpcNow();
  return left > 0 ? (i32)left : 0;
}

bool
LspRenames(const LspDocument *document)
{
  return document && document->server->renames;
}

LspRequest
LspPrepareRename(
  LspDocument *document,
  Position position,
  LspLocationsDone done,
  void *context
)
{
  if (!document || !document->server->preparesRenames) return 0;

  StrBuf params = {};
  AppendPositionParams(&params, document, position);
  StrBufAppendByte(&params, '}');
  Pending pending =
    {.kind = PendingPrepare, .position = position, .locationsDone = done, .context = context};
  LspRequest request = Ask(document, true, "textDocument/prepareRename", &params, pending);
  StrBufFree(&params);
  return request;
}

LspRequest
LspRename(
  LspDocument *document,
  Position position,
  const char *name,
  u64 length,
  LspEditsDone done,
  void *context
)
{
  if (!document) return 0;

  StrBuf params = {};
  AppendPositionParams(&params, document, position);
  StrBufAppendString(&params, ",\"newName\":");
  JsonAppendString(&params, name, length);
  StrBufAppendByte(&params, '}');
  Pending pending = {.kind = PendingEdits, .editsDone = done, .context = context};
  bool answers = document->server->renames;
  LspRequest request = Ask(document, answers, "textDocument/rename", &params, pending);
  StrBufFree(&params);
  return request;
}

// appends a diagnostic as the server sent it (its range in its own counting), so it can
// tell which of its diagnostics a question is about.
static void
AppendDiagnostic(StrBuf *params, const LspDocument *document, const Diagnostic *diagnostic)
{
  const Diagnostics *diagnostics = &document->diagnostics;
  StrBufAppendString(params, "{\"range\":{\"start\":");
  AppendServerPosition(params, document, diagnostic->range.start);
  StrBufAppendString(params, ",\"end\":");
  AppendServerPosition(params, document, diagnostic->range.end);
  StrBufAppendFormat(params, "},\"severity\":%d,\"message\":", (i32)diagnostic->severity);
  const char *text = DiagnosticsText(diagnostics, diagnostic->message);
  JsonAppendString(params, text, strlen(text));

  const char *code = DiagnosticsText(diagnostics, diagnostic->code);
  const char *source = DiagnosticsText(diagnostics, diagnostic->source);

  if (*code) {
    StrBufAppendString(params, ",\"code\":");
    JsonAppendString(params, code, strlen(code));
  }

  if (*source) {
    StrBufAppendString(params, ",\"source\":");
    JsonAppendString(params, source, strlen(source));
  }

  StrBufAppendByte(params, '}');
}

LspRequest
LspFixes(LspDocument *document, u64 line, LspActionsDone done, void *context)
{
  if (!document) return 0;

  // the whole line, with every diagnostic reaching it.
  const Buffer *buffer = document->buffer;
  if (line >= BufferLineCount(buffer)) line = BufferLineCount(buffer) - 1;
  Position end = {line, BufferLine(buffer, line)->length};

  StrBuf params = {};
  AppendDocument(&params, document);
  StrBufAppendString(&params, ",\"range\":{\"start\":");
  AppendPosition(&params, line, 0);
  StrBufAppendString(&params, ",\"end\":");
  AppendServerPosition(&params, document, end);
  StrBufAppendString(&params, "},\"context\":{\"only\":[\"quickfix\"],\"diagnostics\":[");

  const Diagnostics *diagnostics = &document->diagnostics;
  u32 first;
  u32 last;
  DiagnosticsAround(diagnostics, line, &first, &last);
  bool any = false;

  for (u32 index = first; index < last; index++) {
    const Diagnostic *diagnostic = &diagnostics->items[index];
    if (diagnostic->range.end.line < line) continue;

    if (any) StrBufAppendByte(&params, ',');
    any = true;
    AppendDiagnostic(&params, document, diagnostic);
  }

  StrBufAppendString(&params, "]}}");
  Pending pending = {.kind = PendingActions, .actionsDone = done, .context = context};
  bool answers = document->server->fixes;
  LspRequest request = Ask(document, answers, "textDocument/codeAction", &params, pending);
  StrBufFree(&params);
  return request;
}

LspRequest
LspFormat(LspDocument *document, bool spaces, LspEditsDone done, void *context)
{
  if (!document) return 0;

  StrBuf params = {};
  AppendDocument(&params, document);
  StrBufAppendFormat(
    &params,
    ",\"options\":{\"tabSize\":%d,\"insertSpaces\":%s}}",
    tabWidth,
    spaces ? "true" : "false"
  );
  Pending pending = {.kind = PendingEdits, .editsDone = done, .context = context};
  bool answers = document->server->formats;
  LspRequest request = Ask(document, answers, "textDocument/formatting", &params, pending);
  StrBufFree(&params);
  return request;
}
