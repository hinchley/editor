#include "rpc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "memory.h"

extern char **environ;

// how much is read from the process at a time.
static constexpr u64 readSize = 64 * 1024;

// how long a process has to exit after SIGTERM before it is killed.
static constexpr i64 termGrace = 100;

typedef struct Pending {
  u64 id;
  RpcReply reply;
  void *context;
} Pending;

struct Rpc {
  pid_t pid;

  // the process's standard input (written) and output (read), or -1 once closed.
  i32 input;
  i32 output;
  bool alive;

  // bytes still to write from sent onwards, and bytes read but not yet handled.
  StrBuf outgoing;
  u64 sent;
  StrBuf incoming;

  Pending *pending;
  u32 pendingCount;
  u32 pendingCapacity;
  u64 lastId;

  RpcHandler handler;
  void *context;

  // the message being handled, reused for each one.
  Json json;
};

i64
RpcNow(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1'000'000;
}

Rpc *
RpcStart(char *const arguments[], const char *folder, const char *log, RpcHandler handler,
  void *context)
{
  i32 inputPipe[2];
  i32 outputPipe[2];
  if (pipe(inputPipe) != 0) return nullptr;

  if (pipe(outputPipe) != 0) {
    close(inputPipe[0]);
    close(inputPipe[1]);
    return nullptr;
  }

  // every end closes on exec, so a process started later does not hold this one's ends
  // open (which would hide its exit).
  for (u32 index = 0; index < 2; index++) {
    fcntl(inputPipe[index], F_SETFD, FD_CLOEXEC);
    fcntl(outputPipe[index], F_SETFD, FD_CLOEXEC);
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, inputPipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
  i32 flags = O_WRONLY | O_CREAT | O_TRUNC;
  const char *errors = log ? log : "/dev/null";
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errors, flags, 0644);
  posix_spawn_file_actions_addchdir_np(&actions, folder);

  // the editor ignores SIGPIPE, which the process would otherwise inherit.
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

  pid_t pid;
  i32 failed = posix_spawnp(&pid, arguments[0], &actions, &attributes, arguments, environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  close(inputPipe[0]);
  close(outputPipe[1]);

  if (failed) {
    close(inputPipe[1]);
    close(outputPipe[0]);
    return nullptr;
  }

  fcntl(inputPipe[1], F_SETFL, O_NONBLOCK);
  fcntl(outputPipe[0], F_SETFL, O_NONBLOCK);

  Rpc *rpc = MemoryAllocateZeroed(1, sizeof(Rpc));
  rpc->pid = pid;
  rpc->input = inputPipe[1];
  rpc->output = outputPipe[0];
  rpc->alive = true;
  rpc->handler = handler;
  rpc->context = context;
  return rpc;
}

static void
CloseInput(Rpc *rpc)
{
  if (rpc->input >= 0) close(rpc->input);
  rpc->input = -1;
}

// waits for the process to exit until deadline, and returns whether it did.
static bool
Reap(pid_t pid, i64 deadline)
{
  while (true) {
    pid_t done = waitpid(pid, nullptr, WNOHANG);
    if (done == pid || (done < 0 && errno != EINTR)) return true;
    if (RpcNow() >= deadline) return false;

    struct timespec pause = {.tv_nsec = 5'000'000};
    nanosleep(&pause, nullptr);
  }
}

void
RpcFree(Rpc *rpc, i64 deadline)
{
  if (!rpc) return;

  // most servers exit when their input closes; the others are asked, then made to.
  CloseInput(rpc);

  if (!Reap(rpc->pid, deadline)) {
    kill(rpc->pid, SIGTERM);

    if (!Reap(rpc->pid, RpcNow() + termGrace)) {
      kill(rpc->pid, SIGKILL);
      while (waitpid(rpc->pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
  }

  if (rpc->output >= 0) close(rpc->output);
  StrBufFree(&rpc->outgoing);
  StrBufFree(&rpc->incoming);
  JsonFree(&rpc->json);
  free(rpc->pending);
  free(rpc);
}

bool
RpcAlive(const Rpc *rpc)
{
  return rpc->alive;
}

// queues one message, framed.
static void
Send(Rpc *rpc, const StrBuf *body)
{
  if (!rpc->alive) return;

  StrBufAppendFormat(&rpc->outgoing, "Content-Length: %lu\r\n\r\n", body->length);
  StrBufAppend(&rpc->outgoing, body->data, body->length);
}

u64
RpcRequest(Rpc *rpc, const char *method, const char *params, RpcReply reply, void *context)
{
  u64 id = ++rpc->lastId;
  StrBuf body = {};
  StrBufAppendFormat(&body, "{\"jsonrpc\":\"2.0\",\"id\":%lu,\"method\":", id);
  JsonAppendString(&body, method, strlen(method));
  if (params) StrBufAppendFormat(&body, ",\"params\":%s", params);
  StrBufAppendByte(&body, '}');
  Send(rpc, &body);
  StrBufFree(&body);

  if (rpc->pendingCount == rpc->pendingCapacity) {
    rpc->pendingCapacity = rpc->pendingCapacity ? rpc->pendingCapacity * 2 : 16;
    rpc->pending = MemoryResize(rpc->pending, rpc->pendingCapacity, sizeof(Pending));
  }

  rpc->pending[rpc->pendingCount++] = (Pending){id, reply, context};
  return id;
}

// takes the pending request with id out of the list, returning false when there is none.
static bool
TakePending(Rpc *rpc, u64 id, Pending *pending)
{
  for (u32 index = 0; index < rpc->pendingCount; index++) {
    if (rpc->pending[index].id != id) continue;

    *pending = rpc->pending[index];
    rpc->pending[index] = rpc->pending[--rpc->pendingCount];
    return true;
  }

  return false;
}

void
RpcForget(Rpc *rpc, u64 id)
{
  Pending ignored;
  TakePending(rpc, id, &ignored);
}

void
RpcNotify(Rpc *rpc, const char *method, const char *params)
{
  StrBuf body = {};
  StrBufAppendString(&body, "{\"jsonrpc\":\"2.0\",\"method\":");
  JsonAppendString(&body, method, strlen(method));
  if (params) StrBufAppendFormat(&body, ",\"params\":%s", params);
  StrBufAppendByte(&body, '}');
  Send(rpc, &body);
  StrBufFree(&body);
}

void
RpcRespond(Rpc *rpc, const Json *request, u32 id, const char *result)
{
  StrBuf body = {};
  StrBufAppendString(&body, "{\"jsonrpc\":\"2.0\",\"id\":");
  JsonAppendRaw(&body, request, id);

  // -32601 is json-rpc's "method not found".
  if (result) StrBufAppendFormat(&body, ",\"result\":%s}", result);
  else StrBufAppendString(&body, ",\"error\":{\"code\":-32601,\"message\":\"not supported\"}}");

  Send(rpc, &body);
  StrBufFree(&body);
}

u32
RpcPollFds(const Rpc *rpc, struct pollfd *fds)
{
  if (!rpc->alive) return 0;

  u32 count = 0;
  fds[count++] = (struct pollfd){.fd = rpc->output, .events = POLLIN};
  bool queued = rpc->outgoing.length > rpc->sent;
  if (queued) fds[count++] = (struct pollfd){.fd = rpc->input, .events = POLLOUT};
  return count;
}

// the process has gone (or stopped reading); every pending reply hears so.
static void
Died(Rpc *rpc)
{
  if (!rpc->alive) return;

  rpc->alive = false;
  CloseInput(rpc);

  while (rpc->pendingCount) {
    Pending pending = rpc->pending[--rpc->pendingCount];
    pending.reply(pending.context, nullptr, jsonNone, jsonNone);
  }
}

static void
Write(Rpc *rpc)
{
  while (rpc->alive && rpc->sent < rpc->outgoing.length) {
    u64 left = rpc->outgoing.length - rpc->sent;
    i64 written = write(rpc->input, rpc->outgoing.data + rpc->sent, left);

    if (written < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN) Died(rpc);
      break;
    }

    rpc->sent += (u64)written;
  }

  // the queue is emptied once everything went, so it never grows without bound.
  if (rpc->sent == rpc->outgoing.length) {
    StrBufClear(&rpc->outgoing);
    rpc->sent = 0;
  }
}

// the length of the body after a complete header block at the start of text, and the
// header's length; false while the header is incomplete.
static bool
ParseHeader(const char *text, u64 length, u64 *header, u64 *body)
{
  const char *end = memmem(text, length, "\r\n\r\n", 4);
  if (!end) return false;

  *header = (u64)(end - text) + 4;
  *body = 0;
  static constexpr char name[] = "content-length:";
  static constexpr u64 nameLength = sizeof name - 1;

  for (const char *line = text; line < end;) {
    const char *lineEnd = memmem(line, (u64)(end - line) + 2, "\r\n", 2);

    if ((u64)(lineEnd - line) > nameLength && strncasecmp(line, name, nameLength) == 0) {
      *body = strtoull(line + nameLength, nullptr, 10);
    }

    line = lineEnd + 2;
  }

  return true;
}

static void
Dispatch(Rpc *rpc, const char *text, u64 length)
{
  Json *json = &rpc->json;

  // a message that does not parse cannot be answered, so it is ignored.
  if (!JsonParse(json, text, length) || JsonTypeOf(json, 0) != JsonTypeObject) return;

  u32 method = JsonGet(json, 0, "method");
  u32 id = JsonGet(json, 0, "id");

  if (method != jsonNone) {
    rpc->handler(rpc->context, json, method, JsonGet(json, 0, "params"), id);
    return;
  }

  Pending pending;
  i64 number = JsonInteger(json, id, -1);
  if (number < 0 || !TakePending(rpc, (u64)number, &pending)) return;

  u32 result = JsonGet(json, 0, "result");
  u32 error = result == jsonNone ? JsonGet(json, 0, "error") : jsonNone;
  pending.reply(pending.context, json, result, error);
}

static void
Read(Rpc *rpc)
{
  while (rpc->alive) {
    StrBuf *incoming = &rpc->incoming;
    StrBufReserve(incoming, incoming->length + readSize);
    i64 count = read(rpc->output, incoming->data + incoming->length, readSize);

    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && errno == EAGAIN) return;

    if (count <= 0) {
      Died(rpc);
      return;
    }

    incoming->length += (u64)count;
    incoming->data[incoming->length] = '\0';

    // handles every complete message, then keeps the rest for the next read.
    // a handler may send, but never reads, so the buffer stays put while it runs.
    u64 used = 0;
    u64 header;
    u64 body;

    while (ParseHeader(incoming->data + used, incoming->length - used, &header, &body)) {
      if (incoming->length - used - header < body) break;

      Dispatch(rpc, incoming->data + used + header, body);
      used += header + body;
    }

    StrBufRemove(incoming, 0, used);
  }
}

void
RpcPump(Rpc *rpc)
{
  Write(rpc);
  Read(rpc);
  Write(rpc);
}
