#include "clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "term.h"

extern char **environ;

// how long wl-copy or wl-paste may take before it is stopped.
static constexpr i64 commandTimeout = 2000;

static char *copyCommand[] = {"wl-copy", nullptr};
static char *pasteCommand[] = {"wl-paste", "--no-newline", "--type", "text", nullptr};

// the in-process copy, and whether it was copied line wise.
static StrBuf saved;
static bool savedLineWise;

// a command run with its input and output connected to pipes.
typedef struct Command {
  pid_t pid;
  i32 input;
  i32 output;
  const char *text;
  u64 length;
  u64 written;
  StrBuf *result;
} Command;

static bool
WaylandSession(void)
{
  const char *display = getenv("WAYLAND_DISPLAY");
  return display && *display;
}

// a pipe whose ends are closed in the command, apart from the end moved onto its input
// or output.
static bool
OpenPipe(i32 ends[2])
{
  if (pipe(ends) != 0) return false;

  fcntl(ends[0], F_SETFD, FD_CLOEXEC);
  fcntl(ends[1], F_SETFD, FD_CLOEXEC);
  return true;
}

static void
ClosePipe(i32 ends[2])
{
  if (ends[0] >= 0) close(ends[0]);
  if (ends[1] >= 0) close(ends[1]);
}

// starts the command with standard input from a pipe when there is text to send, and
// standard output to a pipe when output is wanted; everything else goes to /dev/null.
static bool
Spawn(Command *command, char *arguments[])
{
  i32 inputPipe[2] = {-1, -1};
  i32 outputPipe[2] = {-1, -1};
  bool piped = OpenPipe(inputPipe) && OpenPipe(outputPipe);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, inputPipe[0], STDIN_FILENO);
  i32 output = STDOUT_FILENO;
  if (command->result) posix_spawn_file_actions_adddup2(&actions, outputPipe[1], output);
  else posix_spawn_file_actions_addopen(&actions, output, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  // the editor ignores SIGPIPE, which the command would otherwise inherit.
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

  bool started = piped &&
    posix_spawnp(&command->pid, arguments[0], &actions, &attributes, arguments, environ) == 0;

  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);

  if (!started) {
    ClosePipe(inputPipe);
    ClosePipe(outputPipe);
    return false;
  }

  close(inputPipe[0]);
  close(outputPipe[1]);
  command->input = inputPipe[1];
  command->output = outputPipe[0];
  fcntl(command->input, F_SETFL, O_NONBLOCK);
  return true;
}

static void
CloseInput(Command *command)
{
  close(command->input);
  command->input = -1;
}

// writes what the pipe takes of the remaining text, closing the pipe when it is all sent.
static bool
WriteSome(Command *command)
{
  const char *from = command->text + command->written;
  i64 count = write(command->input, from, command->length - command->written);

  if (count < 0 && errno != EAGAIN && errno != EINTR) return false;
  if (count > 0) command->written += (u64)count;
  if (command->written == command->length) CloseInput(command);
  return true;
}

// reads what is available, closing the pipe at the end of the output.
static bool
ReadSome(Command *command)
{
  char chunk[65536];
  i64 count = read(command->output, chunk, sizeof(chunk));
  if (count < 0) return errno == EAGAIN || errno == EINTR;

  if (count == 0) {
    close(command->output);
    command->output = -1;
    return true;
  }

  if (command->result) StrBufAppend(command->result, chunk, (u64)count);
  return true;
}

// feeds the input and collects the output until both pipes are closed or time runs out.
static bool
Exchange(Command *command)
{
  i64 deadline = TermNow() + commandTimeout;
  if (command->length == 0) CloseInput(command);

  while (command->input >= 0 || command->output >= 0) {
    i64 remaining = deadline - TermNow();
    if (remaining <= 0) return false;

    struct pollfd descriptors[2] = {
      {.fd = command->input, .events = POLLOUT},
      {.fd = command->output, .events = POLLIN}
    };

    i32 ready = poll(descriptors, 2, (i32)remaining);
    if (ready < 0 && errno != EINTR) return false;
    if (ready <= 0) continue;

    bool writable = command->input >= 0 && descriptors[0].revents;
    bool readable = command->output >= 0 && descriptors[1].revents;
    if (writable && !WriteSome(command)) return false;
    if (readable && !ReadSome(command)) return false;
  }

  return true;
}

// runs a command, sending it text and appending its output to result (when not nullptr).
// returns true when it succeeded in time.
static bool
Run(char *arguments[], const char *text, u64 length, StrBuf *result)
{
  Command command = {.text = text, .length = length, .result = result};
  if (!Spawn(&command, arguments)) return false;

  bool exchanged = Exchange(&command);
  if (command.input >= 0) close(command.input);
  if (command.output >= 0) close(command.output);
  if (!exchanged) kill(command.pid, SIGKILL);

  i32 status;
  while (waitpid(command.pid, &status, 0) < 0 && errno == EINTR) {}
  return exchanged && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool
ClipboardSet(const char *text, u64 length, bool lineWise)
{
  StrBufClear(&saved);
  StrBufAppend(&saved, text, length);
  savedLineWise = lineWise;

  return WaylandSession() && Run(copyCommand, text, length, nullptr);
}

bool
ClipboardGet(StrBuf *text, bool *lineWise)
{
  u64 start = text->length;
  bool fromSystem = WaylandSession() && Run(pasteCommand, nullptr, 0, text);

  if (!fromSystem) {
    text->length = start;
    StrBufAppend(text, saved.data, saved.length);
  }

  // the system clipboard may still hold what was copied line wise here.
  u64 length = text->length - start;
  bool same = length == saved.length;
  if (same && length) same = memcmp(text->data + start, saved.data, length) == 0;
  *lineWise = savedLineWise && same;
  return fromSystem;
}
