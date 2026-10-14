#include "loader.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filter.h"
#include "memory.h"

// .editor files larger than this are not read.
static constexpr u64 maxFilterSize = 1024 * 1024;

// the ids of the files listed so far, in an open addressing table.
// an inode is never 0, so a zero slot is empty.
typedef struct IdSet {
  FileId *slots;
  u64 count;
  u64 capacity;
} IdSet;

typedef struct Walker {
  LoaderList *list;
  IdSet seen;
  u64 limit;
  bool tooMany;
} Walker;

// one entry of a folder listing: its name (in the listing's name buffer) and type.
typedef struct Entry {
  u64 name;
  u8 type;
  u64 inode;
} Entry;

typedef struct Listing {
  StrBuf names;
  Entry *entries;
  u64 count;
  u64 capacity;
} Listing;

static u64
IdHash(FileId id)
{
  u64 hash = id.inode * 0x9E3779B97F4A7C15ull ^ id.device * 0xC2B2AE3D27D4EB4Full;
  return hash ^ (hash >> 29);
}

static bool
IdEqual(FileId a, FileId b)
{
  return a.device == b.device && a.inode == b.inode;
}

static bool IdSetAdd(IdSet *set, FileId id);

static void
IdSetGrow(IdSet *set)
{
  IdSet old = *set;
  set->capacity = old.capacity ? old.capacity * 2 : 1024;
  set->slots = MemoryAllocateZeroed(set->capacity, sizeof(FileId));
  set->count = 0;

  for (u64 index = 0; index < old.capacity; index++) {
    if (old.slots[index].inode) IdSetAdd(set, old.slots[index]);
  }

  free(old.slots);
}

// adds id and returns false when it was already there.
static bool
IdSetAdd(IdSet *set, FileId id)
{
  if ((set->count + 1) * 2 > set->capacity) IdSetGrow(set);

  u64 mask = set->capacity - 1;

  for (u64 slot = IdHash(id) & mask;; slot = (slot + 1) & mask) {
    if (IdEqual(set->slots[slot], id)) return false;
    if (set->slots[slot].inode) continue;

    set->slots[slot] = id;
    set->count++;
    return true;
  }
}

// true when a new (not yet existing) file with this path is already listed.
static bool
ListedNew(const LoaderList *list, const char *path, u64 length)
{
  for (u64 index = 0; index < list->count; index++) {
    const LoaderFile *file = &list->files[index];
    if (file->id.inode || file->path.length != length) continue;
    if (memcmp(file->path.data, path, length) == 0) return true;
  }

  return false;
}

static void
AddFile(Walker *walker, const char *path, u64 length, FileId id)
{
  LoaderList *list = walker->list;

  if (id.inode && !IdSetAdd(&walker->seen, id)) return;
  if (!id.inode && ListedNew(list, path, length)) return;

  if (list->count == walker->limit) {
    walker->tooMany = true;
    return;
  }

  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 64;
    list->files = MemoryResize(list->files, list->capacity, sizeof(LoaderFile));
  }

  LoaderFile *file = &list->files[list->count++];
  *file = (LoaderFile){.id = id};
  StrBufAppend(&file->path, path, length);
}

// reads a folder's .editor file into filter, returning false when there is none.
static bool
ReadFilter(i32 directory, Filter *filter)
{
  i32 descriptor = openat(directory, ".editor", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) return false;

  struct stat status;
  bool regular = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
  u64 size = regular && (u64)status.st_size < maxFilterSize ? (u64)status.st_size : 0;
  char *text = MemoryAllocate(size + 1);
  i64 count = regular ? read(descriptor, text, size) : -1;
  close(descriptor);

  if (count >= 0) FilterParse(filter, text, (u64)count);
  free(text);
  return count >= 0;
}

static void
ListingAdd(Listing *listing, const struct dirent *entry)
{
  if (listing->count == listing->capacity) {
    listing->capacity = listing->capacity ? listing->capacity * 2 : 64;
    listing->entries = MemoryResize(listing->entries, listing->capacity, sizeof(Entry));
  }

  listing->entries[listing->count++] = (Entry){
    .name = listing->names.length,
    .type = entry->d_type,
    .inode = entry->d_ino
  };

  // names are kept nul separated so each one can be used as a string.
  StrBufAppend(&listing->names, entry->d_name, strlen(entry->d_name) + 1);
}

static const char *sortNames;

static i32
CompareEntries(const void *a, const void *b)
{
  const Entry *first = a;
  const Entry *second = b;
  return strcmp(sortNames + first->name, sortNames + second->name);
}

// reads a folder's entries, sorted by name.
static void
ReadListing(DIR *folder, Listing *listing)
{
  struct dirent *entry;

  while ((entry = readdir(folder))) {
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    ListingAdd(listing, entry);
  }

  if (!listing->count) return;

  sortNames = listing->names.data;
  qsort(listing->entries, listing->count, sizeof(Entry), CompareEntries);
}

// what one entry of a listing turns out to be, after following a link to a file.
enum Kind {
  KindSkip,
  KindFile,
  KindFolder
};

static enum Kind
Classify(i32 directory, const char *name, u8 type, FileId *id)
{
  struct stat status;

  if (type == DT_UNKNOWN) {
    if (fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0) return KindSkip;
    if (S_ISLNK(status.st_mode)) type = DT_LNK;
    else if (S_ISDIR(status.st_mode)) type = DT_DIR;
    else if (S_ISREG(status.st_mode)) type = DT_REG;
    id->inode = status.st_ino;
  }

  if (type == DT_DIR) return KindFolder;
  if (type == DT_REG) return KindFile;
  if (type != DT_LNK) return KindSkip;

  // a link counts only when it leads to a regular file.
  if (fstatat(directory, name, &status, 0) != 0 || !S_ISREG(status.st_mode)) return KindSkip;

  *id = (FileId){status.st_dev, status.st_ino};
  return KindFile;
}

// what every entry of one folder shares while it is walked.
typedef struct Folder {
  i32 directory;
  u64 device;

  // the patterns in force, and where the path relative to their .editor file starts.
  const Filter *filter;
  u64 base;
} Folder;

static void Walk(Walker *walker, i32 directory, StrBuf *path, const Filter *filter, u64 base);

// handles one entry, whose display path is in path.
static void
WalkEntry(
  Walker *walker,
  const Folder *folder,
  const char *name,
  const Entry *entry,
  StrBuf *path
) {
  FileId id = {folder->device, entry->inode};
  enum Kind kind = Classify(folder->directory, name, entry->type, &id);
  if (kind == KindSkip || (kind == KindFolder && strcmp(name, ".git") == 0)) return;

  const char *relative = path->data + folder->base;
  u64 relativeLength = path->length - folder->base;
  if (FilterMatches(folder->filter, relative, relativeLength, kind == KindFolder)) return;

  if (kind == KindFile) {
    AddFile(walker, path->data, path->length, id);
    return;
  }

  // links to folders are skipped by Classify, and O_NOFOLLOW makes sure of it.
  i32 flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
  i32 child = openat(folder->directory, name, flags);

  if (child < 0) {
    walker->list->unreadable++;
    return;
  }

  StrBufAppendByte(path, '/');
  Walk(walker, child, path, folder->filter, folder->base);
}

// walks the folder open at directory, whose display path (ending in "/", or empty) is in
// path.
// base is where the path relative to the governing .editor file starts.
// takes ownership of directory.
static void
Walk(Walker *walker, i32 directory, StrBuf *path, const Filter *filter, u64 base)
{
  struct stat status;
  DIR *listed = fstat(directory, &status) == 0 ? fdopendir(directory) : nullptr;

  if (!listed) {
    close(directory);
    walker->list->unreadable++;
    return;
  }

  // a .editor file replaces the patterns from the folders above.
  Filter own = {};

  if (ReadFilter(directory, &own)) {
    filter = &own;
    base = path->length;
  }

  Listing listing = {};
  ReadListing(listed, &listing);

  Folder folder = {directory, status.st_dev, filter, base};
  u64 mark = path->length;

  for (u64 index = 0; index < listing.count && !walker->tooMany; index++) {
    const Entry *entry = &listing.entries[index];
    const char *name = listing.names.data + entry->name;
    StrBufAppendString(path, name);
    WalkEntry(walker, &folder, name, entry, path);
    StrBufTruncate(path, mark);
  }

  closedir(listed);
  free(listing.entries);
  StrBufFree(&listing.names);
  FilterFree(&own);
}

// the display path prefix for a folder argument: the argument without trailing slashes,
// then one slash, except that "." gives an empty prefix.
static void
FolderPrefix(const char *argument, StrBuf *path)
{
  u64 length = strlen(argument);
  while (length > 1 && argument[length - 1] == '/') length--;
  if (length == 1 && argument[0] == '.') return;

  StrBufAppend(path, argument, length);
  if (argument[length - 1] != '/') StrBufAppendByte(path, '/');
}

static enum LoaderStatus
LoadFolder(Walker *walker, const char *argument, StrBuf *message)
{
  i32 directory = open(argument, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  if (directory < 0) {
    StrBufAppendFormat(message, "%s: cannot open folder: %s", argument, strerror(errno));
    return LoaderStatusError;
  }

  StrBuf path = {};
  FolderPrefix(argument, &path);
  walker->list->folders = true;

  // the walk starts with no patterns: only .editor files inside the folder count.
  Filter none = {};
  Walk(walker, directory, &path, &none, path.length);
  StrBufFree(&path);
  return LoaderStatusOk;
}

static enum LoaderStatus
LoadPath(Walker *walker, const char *argument, StrBuf *message)
{
  u64 length = strlen(argument);
  struct stat status;

  if (stat(argument, &status) != 0) {
    // a path that does not exist yet is a new file, created by its first save.
    if (errno == ENOENT && length && argument[length - 1] != '/') {
      AddFile(walker, argument, length, (FileId){});
      return LoaderStatusOk;
    }

    StrBufAppendFormat(message, "%s: %s", argument, strerror(errno));
    return LoaderStatusError;
  }

  if (S_ISDIR(status.st_mode)) return LoadFolder(walker, argument, message);

  // anything else named on the command line is listed, and refused when opened if it is
  // not a regular file.
  AddFile(walker, argument, length, (FileId){status.st_dev, status.st_ino});
  return LoaderStatusOk;
}

enum LoaderStatus
LoaderLoad(LoaderList *list, char *const *paths, u64 pathCount, u64 limit, StrBuf *message)
{
  Walker walker = {.list = list, .limit = limit};
  enum LoaderStatus status = LoaderStatusOk;

  for (u64 index = 0; index < pathCount && status == LoaderStatusOk; index++) {
    status = LoadPath(&walker, paths[index], message);
    if (walker.tooMany) status = LoaderStatusTooMany;
  }

  free(walker.seen.slots);
  return status;
}

void
LoaderFree(LoaderList *list)
{
  for (u64 index = 0; index < list->count; index++) StrBufFree(&list->files[index].path);
  free(list->files);
  *list = (LoaderList){};
}
