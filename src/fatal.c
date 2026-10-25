#include "fatal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void (*fatalCleanup)(void);

void
FatalSetCleanup(void (*cleanup)(void))
{
  fatalCleanup = cleanup;
}

void
Fatal(const char *format, ...)
{
  // clear the hook first so a failure inside cleanup cannot recurse.
  void (*cleanup)(void) = fatalCleanup;
  fatalCleanup = nullptr;
  if (cleanup) cleanup();

  va_list arguments;
  va_start(arguments, format);
  fputs("editor: ", stderr);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
  exit(EXIT_FAILURE);
}
