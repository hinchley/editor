#include "launch.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// starts foot in a session of its own, so closing the editor's window does not end it,
// with nothing of the editor's terminal open, and returns whether it started.
// runs in a child of the editor, which exits once it returns.
static bool
Spawn(char *arguments[])
{
  if (setsid() < 0) return false;

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

  // the editor ignores SIGPIPE, which the terminal would otherwise inherit.
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

  pid_t pid;
  return posix_spawnp(&pid, arguments[0], &actions, &attributes, arguments, environ) == 0;
}

bool
LaunchTerminal(const char *folder, StrBuf *message)
{
  StrBuf option = {};
  StrBufAppendFormat(&option, "--working-directory=%s", folder);
  char *arguments[] = {"foot", option.data, nullptr};

  // the terminal is started by a child that exits at once, so it is never the editor's
  // child: nothing has to wait for it, and the editor's waits for its own children (the
  // language servers, the clipboard commands) never see it.
  // the child leaves with _exit, so none of the editor's cleanup touches the terminal.
  pid_t child = fork();
  if (child == 0) _exit(Spawn(arguments) ? 0 : 1);

  i32 status = -1;
  if (child > 0) {
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  }

  StrBufFree(&option);
  bool started = child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (started) StrBufAppendFormat(message, "opened foot in %s", folder);
  else StrBufAppendString(message, "could not start foot");
  return started;
}
