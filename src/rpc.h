#pragma once

// rpc: a child process that speaks json-rpc 2.0 over its standard input and output, with
// each message framed by a Content-Length header (as language servers do).
// nothing here blocks: messages to send wait in a queue, and RpcPump writes what the pipe
// takes and reads what has arrived, when the main loop's poll says either is ready.
// replies go to the callback given with their request; messages the process starts go to
// one handler.
#include <poll.h>

#include "json.h"
#include "strbuf.h"
#include "types.h"

typedef struct Rpc Rpc;

// a reply to a request: result is the node of its result in message, or jsonNone when it
// failed, in which case error is the node of its error.
// message is nullptr, with both nodes jsonNone, when the process ended before replying.
typedef void (*RpcReply)(void *context, const Json *message, u32 result, u32 error);

// a message from the process: a notification when id is jsonNone, otherwise a request
// that must be answered with RpcRespond.
typedef void (*RpcHandler)(void *context, const Json *message, u32 method, u32 params, u32 id);

// starts arguments[0] (looked up in PATH) in folder, with its standard error appended to
// the file at log (or discarded when log is nullptr).
// returns nullptr when it cannot be started.
Rpc *
RpcStart(
  char *const arguments[],
  const char *folder,
  const char *log,
  RpcHandler handler,
  void *context
);

// ends the process if it is still running (closing its input, then SIGTERM, then SIGKILL,
// waiting at most until the deadline in milliseconds of CLOCK_MONOTONIC) and frees it.
// pending replies are dropped without being called.
void
RpcFree(Rpc *rpc, i64 deadline);

// true until the process closes its output or a write to it fails.
bool
RpcAlive(const Rpc *rpc);

// queues a request whose params are json text (or nullptr for none) and returns its id.
// reply is called once, from RpcPump, unless the request is forgotten.
u64
RpcRequest(Rpc *rpc, const char *method, const char *params, RpcReply reply, void *context);

// forgets a request, so its reply (if one still comes) is ignored.
void
RpcForget(Rpc *rpc, u64 id);

// queues a notification whose params are json text (or nullptr).
void
RpcNotify(Rpc *rpc, const char *method, const char *params);

// answers a request from the process with result (json text), or with an error when
// result is nullptr.
void
RpcRespond(Rpc *rpc, const Json *request, u32 id, const char *result);

// the descriptors to poll: output always, and input while there is something to send.
// fills at most two and returns how many.
u32
RpcPollFds(const Rpc *rpc, struct pollfd *fds);

// writes and reads what it can without waiting, and handles every complete message.
// handlers may send further messages.
void
RpcPump(Rpc *rpc);

// milliseconds of CLOCK_MONOTONIC, the clock the deadlines use.
i64
RpcNow(void);
