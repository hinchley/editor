#include "launch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// a terminal the editor knows how to start in a folder.
typedef struct Terminal {
  const char *name;

  // the option naming the folder to start in.
  char *option;

  // the folder follows the option as an argument of its own, rather than after "=".
  bool separate;
} Terminal;

// ghostty only starts in the folder it inherits when it thinks it was started from a
// shell, so every terminal is given the folder by its option too.
static const Terminal terminals[] = {
  {"foot", "--working-directory", false},
  {"alacritty", "--working-directory", true},
  {"ghostty", "--working-directory", false}
};

// the program to run, found once: the path of the terminal among the editor's parent
// processes, or $TERMINAL. empty when there is neither.
static bool searched;
static StrBuf program;

// the entry of terminals the program is, or nullptr for $TERMINAL.
static const Terminal *known;

// the parent of process pid, or 0 when it cannot be read.
static pid_t
ParentOf(pid_t pid)
{
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  FILE *file = fopen(path, "re");
  if (!file) return 0;

  char text[1024];
  u64 length = fread(text, 1, sizeof(text) - 1, file);
  fclose(file);
  text[length] = 0;

  // the command name is in parentheses and may hold anything, so the fields are found
  // after its last ')': the state, then the parent.
  char *end = strrchr(text, ')');
  char state;
  pid_t parent;
  if (!end || sscanf(end + 1, " %c %d", &state, &parent) != 2) return 0;
  return parent;
}

// reads the path of the program process pid runs into path, and returns false when it
// cannot be read.
static bool
ExecutableOf(pid_t pid, char path[PATH_MAX])
{
  char link[64];
  snprintf(link, sizeof(link), "/proc/%d/exe", pid);
  i64 read = readlink(link, path, PATH_MAX - 1);
  if (read <= 0) return false;

  // a program replaced since it started (by an upgrade) has this after its path.
  static constexpr char deleted[] = " (deleted)";
  u64 length = (u64)read;
  u64 suffix = sizeof(deleted) - 1;
  if (length > suffix && memcmp(path + length - suffix, deleted, suffix) == 0) {
    length -= suffix;
  }

  path[length] = 0;
  return true;
}

// finds the program, once.
static void
FindTerminal(void)
{
  if (searched) return;
  searched = true;

  char path[PATH_MAX];
  u64 count = sizeof(terminals) / sizeof(terminals[0]);

  for (pid_t pid = getppid(); pid > 1; pid = ParentOf(pid)) {
    if (!ExecutableOf(pid, path)) continue;

    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;

    for (u64 index = 0; index < count; index++) {
      if (strcmp(name, terminals[index].name) != 0) continue;

      known = &terminals[index];
      StrBufAppendString(&program, path);
      return;
    }
  }

  const char *fallback = getenv("TERMINAL");
  if (fallback) StrBufAppendString(&program, fallback);
}

// starts the terminal in a session of its own, so closing the editor's terminal does not
// end it, with nothing of the editor's terminal open, and returns whether it started.
// runs in a child of the editor, which exits once it returns.
static bool
Spawn(char *arguments[], const char *folder)
{
  if (setsid() < 0 || chdir(folder) != 0) return false;

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
  FindTerminal();

  if (!program.length) {
    StrBufAppendString(message, "no terminal found (set $TERMINAL)");
    return false;
  }

  StrBuf option = {};
  StrBuf path = {};
  StrBufAppendString(&path, folder);
  char *arguments[] = {program.data, nullptr, nullptr, nullptr};

  if (known && known->separate) {
    arguments[1] = known->option;
    arguments[2] = path.data;
  } else if (known) {
    StrBufAppendFormat(&option, "%s=%s", known->option, folder);
    arguments[1] = option.data;
  }

  // the terminal is started by a child that exits at once, so it is never the editor's
  // child: nothing has to wait for it, and the editor's waits for its own children (the
  // language servers, the clipboard commands) never see it.
  // the child leaves with _exit, so none of the editor's cleanup touches the terminal.
  pid_t child = fork();
  if (child == 0) _exit(Spawn(arguments, folder) ? 0 : 1);

  i32 status = -1;
  if (child > 0) {
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  }

  StrBufFree(&option);
  StrBufFree(&path);

  const char *slash = strrchr(program.data, '/');
  const char *name = known ? known->name : slash ? slash + 1 : program.data;
  bool started = child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

  if (started) StrBufAppendFormat(message, "opened %s in %s", name, folder);
  else StrBufAppendFormat(message, "could not start %s", name);
  return started;
}
