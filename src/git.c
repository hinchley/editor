#include "git.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file.h"
#include "inflate.h"
#include "memory.h"

// how many symbolic refs HEAD may go through.
static constexpr u32 maxRefDepth = 5;

// the longest chain of deltas followed to rebuild one object.
static constexpr u32 maxDeltaDepth = 10'000;

// packed objects kept once rebuilt, and the largest one kept.
static constexpr u32 cacheSize = 32;
static constexpr u64 maxCachedSize = 4ull * 1024 * 1024;

enum ObjectType {
  ObjectNone,
  ObjectCommit,
  ObjectTree,
  ObjectBlob,
  ObjectTag,
  ObjectOffsetDelta = 6,
  ObjectRefDelta = 7
};

// one pack: its index and its data, mapped into memory until the editor exits.
typedef struct Pack {
  StrBuf name;
  const u8 *index;
  u64 indexSize;
  const u8 *data;
  u64 dataSize;
  u32 count;
} Pack;

struct GitRepo {
  // the top of the work tree, and its .git folder.
  StrBuf root;
  StrBuf gitDir;
  u32 hashSize;

  // what HEAD named when last read, and the versions of the files it was read from.
  bool headRead;
  bool headValid;
  GitHash head;
  FileStamp headStamp;
  FileStamp refStamp;
  FileStamp packedStamp;
  StrBuf refPath;

  // allocated one by one, so the cache can point to them while the list grows.
  Pack **packs;
  u32 packCount;
};

// a packed object rebuilt from its deltas, found again by its pack and offset.
typedef struct CacheEntry {
  const Pack *pack;
  u64 offset;
  enum ObjectType type;
  StrBuf data;
  u64 used;
} CacheEntry;

static GitRepo **repos;
static u32 repoCount;
static CacheEntry cache[cacheSize];
static u64 cacheClock;

// the folder looked up last and its repository, as files come a folder at a time.
static StrBuf lastFolder;
static GitRepo *lastRepo;

static const char *
TextOf(const StrBuf *text)
{
  return text->data ? text->data : "";
}

// reads a whole file as it is, binary or not, replacing what text held.
static bool
ReadRaw(const char *path, StrBuf *text)
{
  StrBufClear(text);
  i32 descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) return false;

  struct stat status;
  bool ok = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
  if (ok) StrBufReserve(text, (u64)status.st_size);

  while (ok) {
    StrBufReserve(text, text->length + 65536);
    ssize_t count = read(descriptor, text->data + text->length, text->capacity - text->length);
    if (count < 0) ok = false;
    if (count <= 0) break;

    text->length += (u64)count;
  }

  close(descriptor);
  if (text->data) text->data[text->length] = '\0';
  return ok;
}

static bool
IsDirectory(const char *path)
{
  struct stat status;
  return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static void
TrimEnd(StrBuf *text)
{
  while (text->length && (u8)text->data[text->length - 1] <= ' ') text->length--;
  if (text->data) text->data[text->length] = '\0';
}

static i32
HexDigit(char digit)
{
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  return -1;
}

// parses a full object name in hex, which must be followed by the end of the text or a
// character that is not a hex digit.
static bool
ParseHex(const GitRepo *repo, const char *text, u64 length, GitHash *hash)
{
  *hash = (GitHash){};
  if (length < repo->hashSize * 2) return false;

  for (u32 index = 0; index < repo->hashSize; index++) {
    i32 high = HexDigit(text[index * 2]);
    i32 low = HexDigit(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;

    hash->bytes[index] = (u8)(high << 4 | low);
  }

  return length == repo->hashSize * 2 || HexDigit(text[repo->hashSize * 2]) < 0;
}

bool
GitHashEqual(const GitHash *a, const GitHash *b)
{
  return memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0;
}

// config: only the settings that decide whether the repository can be read.
// returns false for one that cannot: bare, reftable refs or an unknown object format.
static bool
ReadConfig(GitRepo *repo)
{
  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/config", repo->gitDir.data);
  StrBuf text = {};
  bool ok = ReadRaw(path.data, &text);
  StrBufFree(&path);
  repo->hashSize = 20;
  if (!ok) return false;

  char section[32] = "";
  char *line = text.data ? text.data : (char *)"";

  while (ok && *line) {
    char *end = strchr(line, '\n');
    if (end) *end = '\0';

    // comments, then blanks at either end.
    char *comment = strpbrk(line, "#;");
    if (comment) *comment = '\0';
    while (*line == ' ' || *line == '\t') line++;
    u64 length = strlen(line);
    while (length && (u8)line[length - 1] <= ' ') line[--length] = '\0';

    if (*line == '[') {
      // a subsection ([remote "origin"]) never holds the settings looked for.
      u64 name = strcspn(line + 1, "] \t\"");
      bool plain = line[1 + name] == ']';
      snprintf(section, sizeof(section), "%.*s", plain ? (i32)name : 0, line + 1);
    } else if (*line) {
      char *equals = strchr(line, '=');
      char *value = equals ? equals + 1 : (char *)"true";
      if (equals) *equals = '\0';
      u64 keyLength = strlen(line);
      while (keyLength && (u8)line[keyLength - 1] <= ' ') line[--keyLength] = '\0';
      while (*value == ' ' || *value == '\t') value++;

      bool core = strcasecmp(section, "core") == 0;
      bool extensions = strcasecmp(section, "extensions") == 0;

      if (core && strcasecmp(line, "bare") == 0) ok = strcasecmp(value, "true") != 0;
      if (core && strcasecmp(line, "repositoryformatversion") == 0) ok = atoi(value) <= 1;
      if (extensions && strcasecmp(line, "refstorage") == 0) ok = strcasecmp(value, "files") == 0;

      if (extensions && strcasecmp(line, "objectformat") == 0) {
        if (strcasecmp(value, "sha256") == 0) {
          repo->hashSize = 32;
        } else if (strcasecmp(value, "sha1") != 0) {
          ok = false;
        }
      }
    }

    if (!end) break;
    line = end + 1;
  }

  StrBufFree(&text);
  return ok;
}

static void
MapPack(GitRepo *repo, const char *name)
{
  for (u32 index = 0; index < repo->packCount; index++) {
    if (strcmp(repo->packs[index]->name.data, name) == 0) return;
  }

  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/objects/pack/%s", repo->gitDir.data, name);
  StrBuf packPath = {};
  StrBufAppend(&packPath, path.data, path.length - strlen(".idx"));
  StrBufAppendString(&packPath, ".pack");

  Pack pack = {};
  const char *paths[2] = {path.data, packPath.data};
  const u8 *maps[2] = {};
  u64 sizes[2] = {};

  for (u32 file = 0; file < 2; file++) {
    i32 descriptor = open(paths[file], O_RDONLY | O_CLOEXEC);
    struct stat status;

    if (descriptor >= 0 && fstat(descriptor, &status) == 0 && status.st_size > 0) {
      void *map = mmap(nullptr, (u64)status.st_size, PROT_READ, MAP_PRIVATE, descriptor, 0);

      if (map != MAP_FAILED) {
        maps[file] = map;
        sizes[file] = (u64)status.st_size;
      }
    }

    if (descriptor >= 0) close(descriptor);
  }

  StrBufFree(&path);
  StrBufFree(&packPath);

  // index version 2: a magic number, the version, 256 fan out counts, then the names,
  // checksums and offsets of every object, and two hashes at the end.
  const u8 *index = maps[0];
  static const u8 magic[8] = {0xFF, 't', 'O', 'c', 0, 0, 0, 2};
  bool ok = index && maps[1] && sizes[0] >= 8 + 1024 && memcmp(index, magic, 8) == 0 &&
    sizes[1] >= 12 && memcmp(maps[1], "PACK", 4) == 0;

  if (ok) {
    const u8 *last = index + 8 + 255 * 4;
    pack.count = (u32)last[0] << 24 | (u32)last[1] << 16 | (u32)last[2] << 8 | last[3];
    u64 minimum = 8 + 1024 + (u64)pack.count * (repo->hashSize + 8) + 2ull * repo->hashSize;
    ok = sizes[0] >= minimum;
  }

  if (!ok) {
    if (maps[0]) munmap((void *)maps[0], sizes[0]);
    if (maps[1]) munmap((void *)maps[1], sizes[1]);
    return;
  }

  StrBufAppendString(&pack.name, name);
  pack.index = maps[0];
  pack.indexSize = sizes[0];
  pack.data = maps[1];
  pack.dataSize = sizes[1];
  repo->packs = MemoryResize(repo->packs, repo->packCount + 1, sizeof(Pack *));
  repo->packs[repo->packCount++] = MemoryDuplicate(&pack, sizeof(Pack));
}

// maps every pack not mapped yet; returns true when it found any.
static bool
FindPacks(GitRepo *repo)
{
  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/objects/pack", repo->gitDir.data);
  DIR *folder = opendir(path.data);
  StrBufFree(&path);
  if (!folder) return false;

  u32 before = repo->packCount;
  struct dirent *entry;

  while ((entry = readdir(folder))) {
    u64 length = strlen(entry->d_name);
    if (length > 4 && strcmp(entry->d_name + length - 4, ".idx") == 0) MapPack(repo, entry->d_name);
  }

  closedir(folder);
  return repo->packCount > before;
}

static u32
Big32(const u8 *bytes)
{
  return (u32)bytes[0] << 24 | (u32)bytes[1] << 16 | (u32)bytes[2] << 8 | bytes[3];
}

// the offset of an object in a pack, or false when the pack does not hold it.
static bool
PackFind(const GitRepo *repo, const Pack *pack, const GitHash *hash, u64 *offset)
{
  const u8 *fanout = pack->index + 8;
  u8 first = hash->bytes[0];
  u32 low = first ? Big32(fanout + (first - 1) * 4) : 0;
  u32 high = Big32(fanout + first * 4);
  if (high > pack->count || low > high) return false;

  const u8 *names = fanout + 1024;
  u32 size = repo->hashSize;

  while (low < high) {
    u32 middle = low + (high - low) / 2;
    i32 order = memcmp(names + (u64)middle * size, hash->bytes, size);

    if (order == 0) {
      const u8 *offsets = names + (u64)pack->count * (size + 4);
      u32 small = Big32(offsets + (u64)middle * 4);

      if (!(small & 0x80000000u)) {
        *offset = small;
      } else {
        // larger offsets are in a table of 64-bit values after the 32-bit ones.
        const u8 *large = offsets + (u64)pack->count * 4 + (u64)(small & 0x7FFFFFFFu) * 8;
        if (large + 8 > pack->index + pack->indexSize) return false;
        *offset = (u64)Big32(large) << 32 | Big32(large + 4);
      }

      return *offset + 2 <= pack->dataSize;
    }

    if (order < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }

  return false;
}

// applies a delta to base, giving out.
static bool
ApplyDelta(const StrBuf *base, const StrBuf *delta, StrBuf *out)
{
  const u8 *at = (const u8 *)TextOf(delta);
  const u8 *end = at + delta->length;
  u64 sizes[2] = {};

  for (u32 index = 0; index < 2; index++) {
    for (u32 shift = 0;; shift += 7) {
      if (at == end || shift > 56) return false;

      u8 byte = *at++;
      sizes[index] |= (u64)(byte & 0x7F) << shift;
      if (!(byte & 0x80)) break;
    }
  }

  if (sizes[0] != base->length) return false;

  StrBufClear(out);
  StrBufReserve(out, sizes[1]);

  while (at < end) {
    u8 op = *at++;

    if (op & 0x80) {
      // a copy from the base: which bytes of offset and size follow is in the low bits.
      u64 offset = 0;
      u64 size = 0;

      for (u32 bit = 0; bit < 7; bit++) {
        if (!(op & (1u << bit))) continue;
        if (at == end) return false;

        if (bit < 4) {
          offset |= (u64)*at++ << (bit * 8);
        } else {
          size |= (u64)*at++ << ((bit - 4) * 8);
        }
      }

      if (size == 0) size = 0x10000;
      if (offset > base->length || size > base->length - offset) return false;
      StrBufAppend(out, base->data + offset, size);
    } else if (op) {
      if ((u64)(end - at) < op) return false;
      StrBufAppend(out, (const char *)at, op);
      at += op;
    } else {
      return false;
    }
  }

  return out->length == sizes[1];
}

static CacheEntry *
CacheFind(const Pack *pack, u64 offset)
{
  for (u32 index = 0; index < cacheSize; index++) {
    if (cache[index].pack == pack && cache[index].offset == offset) {
      cache[index].used = ++cacheClock;
      return &cache[index];
    }
  }

  return nullptr;
}

static void
CacheAdd(const Pack *pack, u64 offset, enum ObjectType type, const StrBuf *data)
{
  if (data->length > maxCachedSize || CacheFind(pack, offset)) return;

  CacheEntry *oldest = &cache[0];
  for (u32 index = 1; index < cacheSize; index++) {
    if (cache[index].used < oldest->used) oldest = &cache[index];
  }

  oldest->pack = pack;
  oldest->offset = offset;
  oldest->type = type;
  oldest->used = ++cacheClock;
  StrBufClear(&oldest->data);
  StrBufAppend(&oldest->data, TextOf(data), data->length);
}

static bool ReadObject(GitRepo *repo, const GitHash *hash, enum ObjectType *type, StrBuf *out);

// one entry of a pack: its type, its size once inflated, and where its data starts.
// a delta also gives its base, by offset in the same pack or by name.
typedef struct PackEntry {
  const Pack *pack;
  u64 offset;
  enum ObjectType type;
  u64 size;
  u64 dataStart;
  u64 baseOffset;
  GitHash baseHash;
} PackEntry;

static bool
ParseEntry(const GitRepo *repo, const Pack *pack, u64 offset, PackEntry *entry)
{
  const u8 *data = pack->data;
  u64 at = offset;
  u64 end = pack->dataSize;
  if (at >= end) return false;

  u8 byte = data[at++];
  *entry = (PackEntry){.pack = pack, .offset = offset, .type = (enum ObjectType)((byte >> 4) & 7)};
  entry->size = byte & 0x0F;

  for (u32 shift = 4; byte & 0x80; shift += 7) {
    if (at == end || shift > 57) return false;

    byte = data[at++];
    entry->size |= (u64)(byte & 0x7F) << shift;
  }

  if (entry->type == ObjectOffsetDelta) {
    // a big-endian number where each continuation adds one before shifting.
    if (at == end) return false;

    byte = data[at++];
    u64 back = byte & 0x7F;

    while (byte & 0x80) {
      if (at == end || back >> 56) return false;

      byte = data[at++];
      back = ((back + 1) << 7) | (byte & 0x7F);
    }

    if (back == 0 || back > offset) return false;
    entry->baseOffset = offset - back;
  } else if (entry->type == ObjectRefDelta) {
    if (end - at < repo->hashSize) return false;

    memcpy(entry->baseHash.bytes, data + at, repo->hashSize);
    at += repo->hashSize;
  } else if (entry->type == ObjectNone || entry->type == 5) {
    return false;
  }

  entry->dataStart = at;
  return true;
}

static bool
InflateEntry(const PackEntry *entry, StrBuf *out)
{
  const Pack *pack = entry->pack;
  StrBufClear(out);
  const u8 *start = pack->data + entry->dataStart;
  u64 available = pack->dataSize - entry->dataStart;

  // an empty object still has a zlib stream, but the expected size of 0 means "unknown".
  if (!InflateZlib(start, available, entry->size, out)) return false;
  return out->length == entry->size;
}

// rebuilds the object at offset in pack: the chain of deltas down to a whole object (or a
// cached one), then the deltas applied from the bottom up.
static bool
ReadPacked(GitRepo *repo, const Pack *pack, u64 offset, enum ObjectType *type, StrBuf *out)
{
  CacheEntry *cached = CacheFind(pack, offset);

  if (cached) {
    *type = cached->type;
    StrBufClear(out);
    StrBufAppend(out, TextOf(&cached->data), cached->data.length);
    return true;
  }

  PackEntry *chain = nullptr;
  u32 count = 0;
  u32 capacity = 0;
  bool ok = true;
  StrBuf base = {};
  enum ObjectType baseType = ObjectNone;
  const Pack *at = pack;
  u64 atOffset = offset;

  for (;;) {
    cached = at != pack || atOffset != offset ? CacheFind(at, atOffset) : nullptr;

    if (cached) {
      baseType = cached->type;
      StrBufAppend(&base, TextOf(&cached->data), cached->data.length);
      break;
    }

    PackEntry entry;
    if (count == maxDeltaDepth || !ParseEntry(repo, at, atOffset, &entry)) {
      ok = false;
      break;
    }

    if (entry.type != ObjectOffsetDelta && entry.type != ObjectRefDelta) {
      baseType = entry.type;
      ok = InflateEntry(&entry, &base);
      if (ok && count) CacheAdd(at, atOffset, baseType, &base);
      break;
    }

    if (count == capacity) {
      capacity = capacity ? capacity * 2 : 8;
      chain = MemoryResize(chain, capacity, sizeof(PackEntry));
    }

    chain[count++] = entry;

    // a delta's base is earlier in the same pack, or named, usually in the same pack.
    if (entry.type == ObjectOffsetDelta) {
      atOffset = entry.baseOffset;
      continue;
    }

    bool found = false;
    for (u32 index = 0; index < repo->packCount && !found; index++) {
      found = PackFind(repo, repo->packs[index], &entry.baseHash, &atOffset);
      if (found) at = repo->packs[index];
    }

    if (!found) {
      ok = ReadObject(repo, &entry.baseHash, &baseType, &base);
      break;
    }
  }

  // each delta, from the one nearest the base, turns the base into the next object.
  StrBuf delta = {};
  StrBuf next = {};

  for (u32 index = count; ok && index-- > 0;) {
    ok = InflateEntry(&chain[index], &delta) && ApplyDelta(&base, &delta, &next);

    StrBuf swap = base;
    base = next;
    next = swap;
  }

  if (ok) {
    *type = baseType;
    StrBufClear(out);
    StrBufAppend(out, TextOf(&base), base.length);
    CacheAdd(pack, offset, baseType, out);
  }

  free(chain);
  StrBufFree(&base);
  StrBufFree(&delta);
  StrBufFree(&next);
  return ok && baseType >= ObjectCommit && baseType <= ObjectTag;
}

static bool
ReadLoose(GitRepo *repo, const GitHash *hash, enum ObjectType *type, StrBuf *out)
{
  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/objects/", repo->gitDir.data);

  for (u32 index = 0; index < repo->hashSize; index++) {
    StrBufAppendFormat(&path, "%02x", hash->bytes[index]);
    if (index == 0) StrBufAppendByte(&path, '/');
  }

  StrBuf raw = {};
  bool ok = ReadRaw(path.data, &raw);
  StrBufFree(&path);
  StrBufClear(out);
  ok = ok && InflateZlib((const u8 *)TextOf(&raw), raw.length, 0, out);
  StrBufFree(&raw);
  if (!ok) return false;

  // "<type> <size>\0" comes before the content.
  const char *text = TextOf(out);
  const char *nul = memchr(text, '\0', out->length);
  if (!nul) return false;

  static const char *const names[] = {"", "commit", "tree", "blob", "tag"};
  *type = ObjectNone;

  for (u32 index = ObjectCommit; index <= ObjectTag; index++) {
    u64 length = strlen(names[index]);
    if (strncmp(text, names[index], length) == 0 && text[length] == ' ') *type = index;
  }

  u64 header = (u64)(nul - text) + 1;
  u64 size = strtoull(text + strcspn(text, " ") + 1, nullptr, 10);
  StrBufRemove(out, 0, header);
  return *type != ObjectNone && size == out->length;
}

static bool
ReadObject(GitRepo *repo, const GitHash *hash, enum ObjectType *type, StrBuf *out)
{
  // a pack written since the packs were listed is found by listing them again.
  for (u32 attempt = 0; attempt < 2; attempt++) {
    for (u32 index = 0; index < repo->packCount; index++) {
      u64 offset;
      Pack *pack = repo->packs[index];
      if (PackFind(repo, pack, hash, &offset)) return ReadPacked(repo, pack, offset, type, out);
    }

    if (ReadLoose(repo, hash, type, out)) return true;
    if (attempt == 0 && !FindPacks(repo)) break;
  }

  return false;
}

// the object name a ref holds: its loose file, or else its line in packed-refs.
// a symbolic ref gives the name of the ref it points to instead.
static bool
ReadRef(GitRepo *repo, const char *name, GitHash *hash, StrBuf *target, FileStamp *stamp)
{
  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/%s", repo->gitDir.data, name);
  StrBuf text = {};
  *stamp = FileStampOf(path.data);
  bool loose = ReadRaw(path.data, &text);
  StrBufFree(&path);
  StrBufClear(target);

  if (loose) {
    TrimEnd(&text);
    bool symbolic = strncmp(TextOf(&text), "ref:", 4) == 0;

    if (symbolic) {
      const char *rest = text.data + 4;
      while (*rest == ' ') rest++;
      StrBufAppendString(target, rest);
    }

    bool ok = symbolic || ParseHex(repo, TextOf(&text), text.length, hash);
    StrBufFree(&text);
    return ok;
  }

  // packed-refs has a line "<hex> <name>" per ref, with "#" and "^" lines between.
  StrBufAppendFormat(&path, "%s/packed-refs", repo->gitDir.data);
  bool ok = ReadRaw(path.data, &text);
  StrBufFree(&path);
  bool found = false;
  const char *line = TextOf(&text);
  u64 nameLength = strlen(name);

  while (ok && *line && !found) {
    const char *end = strchr(line, '\n');
    u64 length = end ? (u64)(end - line) : strlen(line);
    if (length && line[length - 1] == '\r') length--;
    u64 hexLength = repo->hashSize * 2;

    if (*line != '#' && *line != '^' && length == hexLength + 1 + nameLength &&
        line[hexLength] == ' ' && memcmp(line + hexLength + 1, name, nameLength) == 0) {
      found = ParseHex(repo, line, hexLength, hash);
    }

    if (!end) break;
    line = end + 1;
  }

  StrBufFree(&text);
  return found;
}

static void
ReadHead(GitRepo *repo)
{
  StrBuf packed = {};
  StrBufAppendFormat(&packed, "%s/packed-refs", repo->gitDir.data);
  repo->packedStamp = FileStampOf(packed.data);
  StrBufFree(&packed);

  StrBuf name = {};
  StrBuf target = {};
  StrBufAppendString(&name, "HEAD");
  repo->headValid = false;
  repo->refStamp = (FileStamp){};
  StrBufClear(&repo->refPath);

  for (u32 depth = 0; depth <= maxRefDepth; depth++) {
    FileStamp stamp;
    if (!ReadRef(repo, name.data, &repo->head, &target, &stamp)) break;

    if (depth == 0) {
      repo->headStamp = stamp;
    } else {
      repo->refStamp = stamp;
      StrBufClear(&repo->refPath);
      StrBufAppendFormat(&repo->refPath, "%s/%s", repo->gitDir.data, name.data);
    }

    if (!target.length) {
      repo->headValid = true;
      break;
    }

    // a symbolic ref only ever points under refs/, never back out of the folder.
    if (strncmp(target.data, "refs/", 5) != 0 || strstr(target.data, "..")) break;

    StrBufClear(&name);
    StrBufAppend(&name, target.data, target.length);
  }

  // a branch with no commits yet has no ref file, which is noticed when one appears.
  if (!repo->refPath.length && name.length && strcmp(name.data, "HEAD") != 0) {
    StrBufAppendFormat(&repo->refPath, "%s/%s", repo->gitDir.data, name.data);
    repo->refStamp = FileStampOf(repo->refPath.data);
  }

  StrBufFree(&name);
  StrBufFree(&target);
  repo->headRead = true;
}

bool
GitHead(GitRepo *repo, GitHash *commit)
{
  bool fresh = repo->headRead;

  if (fresh) {
    StrBuf path = {};
    StrBufAppendFormat(&path, "%s/HEAD", repo->gitDir.data);
    fresh = FileStampEqual(FileStampOf(path.data), repo->headStamp);
    StrBufClear(&path);
    StrBufAppendFormat(&path, "%s/packed-refs", repo->gitDir.data);
    fresh = fresh && FileStampEqual(FileStampOf(path.data), repo->packedStamp);
    StrBufFree(&path);

    if (fresh && repo->refPath.length) {
      fresh = FileStampEqual(FileStampOf(repo->refPath.data), repo->refStamp);
    }
  }

  if (!fresh) ReadHead(repo);
  *commit = repo->head;
  return repo->headValid;
}

static GitRepo *
OpenRepo(const char *root)
{
  for (u32 index = 0; index < repoCount; index++) {
    if (strcmp(repos[index]->root.data, root) == 0) return repos[index];
  }

  GitRepo *repo = MemoryAllocateZeroed(1, sizeof(GitRepo));
  StrBufAppendString(&repo->root, root);
  StrBufAppendFormat(&repo->gitDir, "%s%s.git", root, strcmp(root, "/") ? "/" : "");

  StrBuf objects = {};
  StrBufAppendFormat(&objects, "%s/objects", repo->gitDir.data);
  bool ok = IsDirectory(objects.data) && ReadConfig(repo);
  StrBufFree(&objects);

  // a repository that cannot be read is remembered too, so it is not tried again.
  repos = MemoryResize(repos, repoCount + 1, sizeof(GitRepo *));
  repos[repoCount++] = repo;

  if (!ok) {
    repo->hashSize = 0;
    return repo;
  }

  FindPacks(repo);
  return repo;
}

// the repository whose work tree holds folder (a real path), or nullptr.
static GitRepo *
RepoOfFolder(const char *folder)
{
  StrBuf candidate = {};
  StrBufAppendString(&candidate, folder);
  GitRepo *repo = nullptr;

  for (;;) {
    StrBuf dotGit = {};
    StrBufAppendFormat(&dotGit, "%s%s.git", candidate.data, candidate.length > 1 ? "/" : "");
    struct stat status;
    bool exists = lstat(dotGit.data, &status) == 0;
    StrBufFree(&dotGit);

    // a .git file (a worktree or submodule) ends the search: the folders above belong to
    // another repository, which does not track these files.
    if (exists) {
      if (S_ISDIR(status.st_mode)) repo = OpenRepo(candidate.data);
      break;
    }

    char *slash = strrchr(candidate.data, '/');
    if (!slash || candidate.length == 1) break;

    StrBufTruncate(&candidate, slash == candidate.data ? 1 : (u64)(slash - candidate.data));
  }

  StrBufFree(&candidate);
  return repo && repo->hashSize ? repo : nullptr;
}

GitRepo *
GitRepoOf(const char *path, StrBuf *relative)
{
  StrBufClear(relative);
  const char *slash = strrchr(path, '/');
  StrBuf folder = {};

  if (slash) {
    StrBufAppend(&folder, path, slash == path ? 1 : (u64)(slash - path));
  } else {
    StrBufAppendByte(&folder, '.');
  }

  const char *name = slash ? slash + 1 : path;
  char real[PATH_MAX];
  bool found = *name && realpath(folder.data, real);
  StrBufFree(&folder);
  if (!found) return nullptr;

  GitRepo *repo;

  if (lastFolder.data && strcmp(lastFolder.data, real) == 0) {
    repo = lastRepo;
  } else {
    repo = RepoOfFolder(real);
    StrBufClear(&lastFolder);
    StrBufAppendString(&lastFolder, real);
    lastRepo = repo;
  }

  if (!repo) return nullptr;

  // the path below the top of the work tree; a file in .git itself is not in it.
  u64 rootLength = repo->root.length;
  const char *below = real + rootLength;
  while (*below == '/') below++;

  StrBufAppendString(relative, below);
  if (relative->length) StrBufAppendByte(relative, '/');
  StrBufAppendString(relative, name);

  bool inGit = strncmp(relative->data, ".git/", 5) == 0 || strcmp(relative->data, ".git") == 0;
  return inGit ? nullptr : repo;
}

// finds a name in a tree, giving its mode and object.
static bool
TreeFind(
  const GitRepo *repo,
  const StrBuf *tree,
  const char *name,
  u64 nameLength,
  u32 *mode,
  GitHash *hash
) {
  const char *at = TextOf(tree);
  const char *end = at + tree->length;

  // each entry is "<octal mode> <name>\0<object name in binary>".
  while (at < end) {
    const char *space = memchr(at, ' ', (u64)(end - at));
    if (!space) return false;

    const char *entryName = space + 1;
    const char *nul = memchr(entryName, '\0', (u64)(end - entryName));
    if (!nul || (u64)(end - nul - 1) < repo->hashSize) return false;

    u64 length = (u64)(nul - entryName);

    if (length == nameLength && memcmp(entryName, name, length) == 0) {
      *mode = (u32)strtoul(at, nullptr, 8);
      *hash = (GitHash){};
      memcpy(hash->bytes, nul + 1, repo->hashSize);
      return true;
    }

    at = nul + 1 + repo->hashSize;
  }

  return false;
}

enum GitFile
GitReadFile(GitRepo *repo, const GitHash *commit, const char *relative, StrBuf *text)
{
  enum ObjectType type;
  StrBufClear(text);
  if (!ReadObject(repo, commit, &type, text) || type != ObjectCommit) return GitFileUnreadable;

  // a commit starts "tree <hex>".
  GitHash hash;
  if (strncmp(TextOf(text), "tree ", 5) != 0) return GitFileUnreadable;
  if (!ParseHex(repo, text->data + 5, text->length - 5, &hash)) return GitFileUnreadable;

  const char *name = relative;

  for (;;) {
    if (!ReadObject(repo, &hash, &type, text) || type != ObjectTree) return GitFileUnreadable;

    const char *slash = strchr(name, '/');
    u64 length = slash ? (u64)(slash - name) : strlen(name);
    u32 mode;
    if (!TreeFind(repo, text, name, length, &mode, &hash)) return GitFileUntracked;

    // folders are 040000, regular files 100644 or 100755; links and submodules are not
    // files the editor would show.
    bool folder = (mode & 0170000) == 0040000;
    bool file = (mode & 0170000) == 0100000;

    if (!slash) {
      if (!file) return GitFileUntracked;
      bool read = ReadObject(repo, &hash, &type, text) && type == ObjectBlob;
      return read ? GitFileOk : GitFileUnreadable;
    }

    if (!folder) return GitFileUntracked;
    name = slash + 1;
  }
}
