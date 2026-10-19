#pragma once

// the languages the editor knows: the packages in the languages folder of the config
// folder, one folder each, read once at startup (see ARCHITECTURE.md, "Languages").
// a package names its files (by extension, file name or "#!" program), how a line is
// commented out and, when it has a parser.so, the grammar that highlights it.
#include "strbuf.h"
#include "types.h"

typedef struct Lang {
  // the display name (status bar, messages), or nullptr for a grammar that is only ever
  // injected into others.
  const char *name;

  // the package's folder name, and the folder itself.
  const char *id;
  const char *folder;

  // the extensions (without the dot), whole file names (matching case), "#!" programs and
  // the lowercase names other grammars inject it by, each ending with nullptr.
  const char *const *extensions;
  const char *const *names;
  const char *const *interpreters;
  const char *const *injectionIds;

  // the function of parser.so that gives the grammar.
  const char *symbol;

  // the text put before a commented line and, for block comment languages, after it
  // (otherwise empty).
  // commentStart is nullptr for a language with no comments.
  const char *commentStart;
  const char *commentEnd;

  // the language server's program and its arguments, separated by blanks, or nullptr for
  // a language without one; and the names whose folder is the server's root (the first
  // found walking up from a file), ending with nullptr.
  const char *server;
  const char *const *serverRoots;

  // prose is spell checked outside code; other languages only in comments and strings.
  bool prose;

  // parse again from scratch after an edit, for a grammar whose scanner gives different
  // trees when parsing incrementally (markdown's inline grammar).
  bool freshParse;

  // for captures of the same node, the earlier pattern wins (the tree-sitter cli rule)
  // rather than the later one (go's query puts the specific patterns first).
  bool firstPatternWins;

  // whether the folder held parser.so and tags.scm when it was read.
  bool hasParser;
  bool hasTags;

  // the position in the registry, for state kept per language.
  u32 index;
} Lang;

// reads every package once, before any language is asked for.
// a package with a bad language file is left out and the first problem is appended to
// message.
// the languages stay valid until the editor exits.
void
LangLoadAll(StrBuf *message);

u32
LangCount(void);

const Lang *
LangAt(u32 index);

// the language of a path, from its file name or its extension (ignoring case).
// a path with neither gets script, the language of its first line (LangOfScript), which
// may be nullptr.
const Lang *
LangOfPath(const char *path, const Lang *script);

// the language with an extension (ignoring case), or nullptr.
const Lang *
LangOfExtension(const char *extension, u64 length);

// the language of a script whose first line is "#!" naming one of its interpreters,
// directly or through env, or nullptr.
const Lang *
LangOfScript(const char *line, u64 length);

// the language an injection names (ignoring case) by one of its injection ids or,
// failing that, by an extension, so a fenced code block can say "js" as well as
// "javascript"; or nullptr.
const Lang *
LangOfInjection(const char *name, u64 length);
