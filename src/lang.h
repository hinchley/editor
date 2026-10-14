#pragma once

// the languages the editor knows, chosen by file extension: a display name (shown in the
// status bar and used to find the grammar) and how a line is commented out.
#include "types.h"

typedef struct Lang {
  const char *name;

  // the extensions, without the dot, ending with nullptr.
  const char *extensions[4];

  // the text put before a commented line and, for block comment languages, after it.
  // commentStart is nullptr for a language with no comments.
  const char *commentStart;
  const char *commentEnd;
} Lang;

// the language of a path, from its extension (ignoring case), or nullptr.
const Lang *
LangOfPath(const char *path);

// the language with an extension (ignoring case), or nullptr.
const Lang *
LangOfExtension(const char *extension, u64 length);
