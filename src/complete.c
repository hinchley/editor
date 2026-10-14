#include "complete.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "memory.h"

// the names that can complete the typed one, nul separated, with "/" after folders.
typedef struct Candidates {
  StrBuf names;
  u64 *offsets;
  u64 count;
  u64 capacity;
} Candidates;

static Candidates candidates;

// the folder part of the path being completed, and the text the last step through the
// candidates produced, so the next call can tell the user has not typed since.
static StrBuf folder;
static StrBuf stepped;
static u64 step;

static void
AddCandidate(const char *name, bool isFolder)
{
  if (candidates.count == candidates.capacity) {
    candidates.capacity = candidates.capacity ? candidates.capacity * 2 : 32;
    candidates.offsets = MemoryResize(candidates.offsets, candidates.capacity, sizeof(u64));
  }

  candidates.offsets[candidates.count++] = candidates.names.length;
  StrBufAppendString(&candidates.names, name);
  if (isFolder) StrBufAppendByte(&candidates.names, '/');
  StrBufAppendByte(&candidates.names, '\0');
}

static const char *
Candidate(u64 index)
{
  return candidates.names.data + candidates.offsets[index];
}

static i32
CompareOffsets(const void *a, const void *b)
{
  const char *names = candidates.names.data;
  return strcmp(names + *(const u64 *)a, names + *(const u64 *)b);
}

static bool
IsFolder(DIR *listing, const struct dirent *entry)
{
  if (entry->d_type == DT_DIR) return true;
  if (entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN) return false;

  struct stat status;
  return fstatat(dirfd(listing), entry->d_name, &status, 0) == 0 && S_ISDIR(status.st_mode);
}

// lists the entries of the folder that start with name.
static void
Gather(const char *name, u64 length)
{
  StrBufClear(&candidates.names);
  candidates.count = 0;

  DIR *listing = opendir(folder.length ? folder.data : ".");
  if (!listing) return;

  struct dirent *entry;

  while ((entry = readdir(listing))) {
    const char *entryName = entry->d_name;
    if (strcmp(entryName, ".") == 0 || strcmp(entryName, "..") == 0) continue;
    if (entryName[0] == '.' && (!length || name[0] != '.')) continue;
    if (strncmp(entryName, name, length) != 0) continue;

    AddCandidate(entryName, IsFolder(listing, entry));
  }

  closedir(listing);
  if (!candidates.count) return;

  qsort(candidates.offsets, candidates.count, sizeof(u64), CompareOffsets);
}

// the length of the start that every candidate shares.
static u64
CommonLength(void)
{
  const char *first = Candidate(0);
  u64 length = strlen(first);

  for (u64 index = 1; index < candidates.count; index++) {
    const char *other = Candidate(index);
    u64 same = 0;
    while (same < length && first[same] == other[same]) same++;
    length = same;
  }

  return length;
}

static void
SetPath(StrBuf *path, const char *name, u64 length)
{
  StrBufClear(path);
  StrBufAppend(path, folder.data ? folder.data : "", folder.length);
  StrBufAppend(path, name, length);
}

static void
StepThrough(StrBuf *path)
{
  const char *name = Candidate(step);
  SetPath(path, name, strlen(name));
  StrBufClear(&stepped);
  StrBufAppend(&stepped, path->data, path->length);
}

void
CompletePath(StrBuf *path)
{
  const char *text = path->data ? path->data : "";

  // tab again, with nothing typed since, moves on to the next candidate.
  bool same = stepped.length == path->length;
  if (same && stepped.length) same = memcmp(stepped.data, text, path->length) == 0;

  if (candidates.count > 1 && stepped.length && same) {
    step = (step + 1) % candidates.count;
    StepThrough(path);
    return;
  }

  StrBufClear(&stepped);
  const char *slash = strrchr(text, '/');
  u64 folderLength = slash ? (u64)(slash - text) + 1 : 0;
  StrBufClear(&folder);
  StrBufAppend(&folder, text, folderLength);

  StrBuf name = {};
  StrBufAppend(&name, text + folderLength, path->length - folderLength);
  Gather(name.data ? name.data : "", name.length);
  u64 typed = name.length;
  StrBufFree(&name);

  if (!candidates.count) return;

  u64 common = CommonLength();

  if (common > typed || candidates.count == 1) {
    SetPath(path, Candidate(0), common);
    return;
  }

  step = 0;
  StepThrough(path);
}
