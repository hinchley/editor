#pragma once

// turns the paths given to the editor into the list of files to load.
// a folder is walked at any depth, skipping .git folders, symbolic links to folders and
// whatever its .editor files leave out; symbolic links to files are followed.
// a file reached twice (named again, inside a named folder, or through a link) is listed
// once, under the path it was first reached by.
// only folder listings and .editor files are read; the files themselves are not touched.
#include "file.h"
#include "strbuf.h"
#include "types.h"

typedef struct LoaderFile {
  // the display path: as given, or the folder as given followed by the path inside it.
  StrBuf path;

  // zero for a path that does not exist yet.
  FileId id;
} LoaderFile;

typedef struct LoaderList {
  LoaderFile *files;
  u64 count;
  u64 capacity;

  // one of the paths was a folder.
  bool folders;

  // folders inside the walk that could not be read.
  u64 unreadable;
} LoaderList;

enum LoaderStatus {
  LoaderStatusOk,

  // more than the limit; the list is incomplete and nothing is added to the message.
  LoaderStatusTooMany,

  // a path cannot be used; the reason is in the message.
  LoaderStatusError
};

// adds the files reached from paths to list, refusing to go beyond limit files.
// a path that does not exist is listed as a new file, unless it ends in "/".
enum LoaderStatus
LoaderLoad(LoaderList *list, char *const *paths, u64 pathCount, u64 limit, StrBuf *message);

void
LoaderFree(LoaderList *list);
