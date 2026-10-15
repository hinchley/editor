#pragma once

// the languages the editor knows, chosen by file name or extension: a display name (shown
// in the status bar and used to find the grammar) and how a line is commented out.
#include "types.h"

typedef struct Lang {
  const char *name;

  // the extensions, without the dot, ending with nullptr.
  const char *extensions[5];

  // whole file names (matching case) for files without an extension, ending with nullptr.
  const char *names[5];

  // the text put before a commented line and, for block comment languages, after it.
  // commentStart is nullptr for a language with no comments.
  const char *commentStart;
  const char *commentEnd;
} Lang;

// the language of a path, from its file name or its extension (ignoring case).
// a path with neither gets script, the language of its first line (LangOfScript), which
// may be nullptr.
const Lang *
LangOfPath(const char *path, const Lang *script);

// the language with an extension (ignoring case), or nullptr.
const Lang *
LangOfExtension(const char *extension, u64 length);

// the language of a script whose first line is "#!" naming sh or bash, directly or through
// env, or nullptr.
const Lang *
LangOfScript(const char *line, u64 length);
