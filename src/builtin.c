#include "builtin.h"

#include <string.h>

const BuiltinGrammar *
BuiltinFindGrammar(const char *symbol)
{
  for (const BuiltinGrammar *grammar = builtinGrammars; grammar->symbol; grammar++) {
    if (strcmp(grammar->symbol, symbol) == 0) return grammar;
  }

  return nullptr;
}

const BuiltinFile *
BuiltinFindFile(const char *id, const char *name)
{
  for (const BuiltinFile *file = builtinFiles; file->id; file++) {
    if (strcmp(file->id, id) == 0 && strcmp(file->name, name) == 0) return file;
  }

  return nullptr;
}
