#pragma once

// path patterns, as used by .editor files to leave files out of a folder.
// a pattern names a file (foo.txt, *.c) or, with a trailing slash, a folder and all it
// contains (bar/).
// a pattern starting with a slash, or with a slash before its end, is matched against the
// whole path relative to where the patterns came from (/foo/foo.txt, /*.c, /bar/).
// any other pattern is matched against one name at any depth.
// "*" matches any run of characters within one name, and is the only wildcard.
#include "strbuf.h"
#include "types.h"

typedef struct FilterPattern {
  // the pattern without its leading and trailing slashes.
  StrBuf text;
  bool anchored;
  bool folder;
} FilterPattern;

typedef struct Filter {
  FilterPattern *patterns;
  u32 count;
  u32 capacity;
} Filter;

// adds one pattern.
// an empty pattern (or a lone slash) is ignored.
void
FilterAdd(Filter *filter, const char *pattern, u64 length);

// adds the patterns of a .editor file: one per line, where "#" at the start of a line or
// after a space or tab starts a comment, and blank lines are ignored.
void
FilterParse(Filter *filter, const char *text, u64 length);

void
FilterFree(Filter *filter);

// true when a pattern matches path, which is relative to where the patterns came from and
// uses "/" between names.
// a file also matches a folder pattern that matches any folder above it.
bool
FilterMatches(const Filter *filter, const char *path, u64 length, bool isFolder);
