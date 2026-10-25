#pragma once

// what a static build compiles into the editor so that it needs nothing installed: the
// vendored grammars, the files of the default packages and the default word list.
// a release or debug build has none of them, and loads grammars from parser.so instead.
// the tables are defined in build/<mode>/builtin-data.c, which build.sh generates; each
// ends with an entry of nullptr.
#include <tree_sitter/api.h>

#include "types.h"

// a grammar, by the name of the function that gives it (tree_sitter_<id>).
typedef struct BuiltinGrammar {
  const char *symbol;
  const TSLanguage *(*language)(void);
} BuiltinGrammar;

// a file of a package (id is the package's) or the word list (id is nullptr).
typedef struct BuiltinFile {
  const char *id;
  const char *name;
  const u8 *data;
  u64 length;
} BuiltinFile;

extern const BuiltinGrammar builtinGrammars[];

// the package ids, sorted.
extern const char *const builtinPackages[];

// the files of every package, sorted by package.
extern const BuiltinFile builtinFiles[];

// the word list, whose data is nullptr when there is none.
extern const BuiltinFile builtinWords;

// the grammar named symbol, or nullptr.
const BuiltinGrammar *
BuiltinFindGrammar(const char *symbol);

// the file name of package id, or nullptr.
const BuiltinFile *
BuiltinFindFile(const char *id, const char *name);
