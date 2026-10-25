#include "lang.h"

#include <dirent.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "builtin.h"
#include "file.h"
#include "memory.h"

// while packages are read, their texts are offsets into one string of nul terminated
// texts, and each list is a run of offsets in one array, ending with noText.
static constexpr u64 noText = UINT64_MAX;

enum ListKind {
  ListExtensions,
  ListNames,
  ListInterpreters,
  ListInjections,
  ListRoots,
  ListKindCount
};

typedef struct Draft {
  u64 id;
  u64 folder;
  u64 name;
  u64 symbol;
  u64 commentStart;
  u64 commentEnd;
  u64 server;
  u64 lists[ListKindCount];
  bool prose;
  bool freshParse;
  bool firstPatternWins;
  bool hasGrammar;
  bool hasTags;
} Draft;

typedef struct Reading {
  StrBuf texts;
  u64 *slots;
  u64 slotCount;
  u64 slotCapacity;
  Draft *drafts;
  u32 draftCount;
  u32 draftCapacity;

  // the first problem, and how many packages had one.
  StrBuf problem;
  u32 problemCount;
} Reading;

// one directive of a language file, and where its value goes in a draft: the offset of
// its text (a u64) or flag (a bool), or the list it fills.
typedef struct Directive {
  const char *key;
  enum {
    DirectiveText,
    DirectiveWord,
    DirectiveList,
    DirectiveFlag
  } kind;
  u64 place;
} Directive;

static const Directive directives[] = {
  {"name", DirectiveText, offsetof(Draft, name)},
  {"extensions", DirectiveList, ListExtensions},
  {"files", DirectiveList, ListNames},
  {"interpreters", DirectiveList, ListInterpreters},
  {"inject", DirectiveList, ListInjections},
  {"symbol", DirectiveWord, offsetof(Draft, symbol)},
  {"comment", DirectiveWord, offsetof(Draft, commentStart)},
  {"comment-end", DirectiveWord, offsetof(Draft, commentEnd)},
  {"server", DirectiveText, offsetof(Draft, server)},
  {"server-roots", DirectiveList, ListRoots},
  {"prose", DirectiveFlag, offsetof(Draft, prose)},
  {"fresh-parse", DirectiveFlag, offsetof(Draft, freshParse)},
  {"first-pattern-wins", DirectiveFlag, offsetof(Draft, firstPatternWins)}
};

static constexpr u32 directiveCount = sizeof(directives) / sizeof(directives[0]);

static Lang *langs;
static u32 langCount;

static u64
AddText(Reading *reading, const char *text, u64 length)
{
  u64 offset = reading->texts.length;
  StrBufAppend(&reading->texts, text, length);
  StrBufAppendByte(&reading->texts, '\0');
  return offset;
}

static void
AddSlot(Reading *reading, u64 slot)
{
  if (reading->slotCount == reading->slotCapacity) {
    reading->slotCapacity = reading->slotCapacity ? reading->slotCapacity * 2 : 64;
    reading->slots = MemoryResize(reading->slots, reading->slotCapacity, sizeof(u64));
  }

  reading->slots[reading->slotCount++] = slot;
}

static bool
IsBlank(char byte)
{
  return byte == ' ' || byte == '\t' || byte == '\r';
}

// the text from start to end without blanks around it.
static void
Trim(const char *text, u64 *start, u64 *end)
{
  while (*start < *end && IsBlank(text[*start])) (*start)++;
  while (*end > *start && IsBlank(text[*end - 1])) (*end)--;
}

// adds the blank separated words of a value as a list, giving its first slot.
static u64
AddList(Reading *reading, const char *value, u64 length)
{
  u64 first = reading->slotCount;

  for (u64 at = 0; at < length;) {
    while (at < length && IsBlank(value[at])) at++;
    u64 start = at;
    while (at < length && !IsBlank(value[at])) at++;
    if (at > start) AddSlot(reading, AddText(reading, value + start, at - start));
  }

  AddSlot(reading, noText);
  return first;
}

static bool
HasBlank(const char *value, u64 length)
{
  for (u64 at = 0; at < length; at++) {
    if (IsBlank(value[at])) return true;
  }

  return false;
}

// sets one directive of draft from its value, or describes why it cannot in problem.
static bool
Apply(
  Reading *reading,
  Draft *draft,
  u32 directive,
  const char *value,
  u64 length,
  StrBuf *problem
)
{
  const char *key = directives[directive].key;
  u64 place = directives[directive].place;

  switch (directives[directive].kind) {
    case DirectiveWord:
      if (HasBlank(value, length)) {
        StrBufAppendFormat(problem, "%s takes one word", key);
        return false;
      }
      [[fallthrough]];

    case DirectiveText:
      *(u64 *)((u8 *)draft + place) = AddText(reading, value, length);
      return true;

    case DirectiveList:
      draft->lists[place] = AddList(reading, value, length);
      return true;

    case DirectiveFlag: {
      bool yes = length == 3 && memcmp(value, "yes", 3) == 0;
      bool no = length == 2 && memcmp(value, "no", 2) == 0;

      if (!yes && !no) {
        StrBufAppendFormat(problem, "%s takes yes or no", key);
        return false;
      }

      *(bool *)((u8 *)draft + place) = yes;
      return true;
    }
  }

  return false;
}

// reads the directives of a language file into draft, or describes the first problem
// (with its line) in problem.
static bool
ParseLanguage(Reading *reading, Draft *draft, const StrBuf *file, StrBuf *problem)
{
  const char *text = file->data ? file->data : "";
  bool seen[directiveCount] = {};
  u64 line = 0;

  for (u64 at = 0; at < file->length; line++) {
    const char *newline = memchr(text + at, '\n', file->length - at);
    u64 end = newline ? (u64)(newline - text) : file->length;
    u64 start = at;
    at = end + 1;

    // a line starting with # is a comment, and there are none after a value, since "#"
    // is a comment marker the comment directive may name.
    Trim(text, &start, &end);
    if (start == end || text[start] == '#') continue;

    const char *equals = memchr(text + start, '=', end - start);

    if (!equals) {
      StrBufAppendFormat(problem, "line %lu: expected key = value", line + 1);
      return false;
    }

    u64 keyStart = start;
    u64 keyEnd = (u64)(equals - text);
    u64 valueStart = keyEnd + 1;
    u64 valueEnd = end;
    Trim(text, &keyStart, &keyEnd);
    Trim(text, &valueStart, &valueEnd);
    u64 keyLength = keyEnd - keyStart;
    u32 directive = 0;

    while (directive < directiveCount) {
      const char *key = directives[directive].key;
      if (strlen(key) == keyLength && memcmp(key, text + keyStart, keyLength) == 0) break;
      directive++;
    }

    if (directive == directiveCount) {
      StrBufAppendFormat(
        problem,
        "line %lu: unknown key \"%.*s\"",
        line + 1,
        (int)keyLength,
        text + keyStart
      );
      return false;
    }

    const char *key = directives[directive].key;

    if (seen[directive]) {
      StrBufAppendFormat(problem, "line %lu: %s given twice", line + 1, key);
      return false;
    }

    if (valueEnd == valueStart) {
      StrBufAppendFormat(problem, "line %lu: %s has no value", line + 1, key);
      return false;
    }

    seen[directive] = true;
    StrBuf reason = {};
    bool applied =
      Apply(reading, draft, directive, text + valueStart, valueEnd - valueStart, &reason);
    if (!applied) StrBufAppendFormat(problem, "line %lu: %s", line + 1, reason.data);
    StrBufFree(&reason);
    if (!applied) return false;
  }

  bool named = draft->lists[ListExtensions] != noText || draft->lists[ListNames] != noText ||
    draft->lists[ListInterpreters] != noText;

  if (named && draft->name == noText) {
    StrBufAppendString(
      problem,
      "a language with extensions, files or interpreters needs a name"
    );
    return false;
  }

  if (draft->commentEnd != noText && draft->commentStart == noText) {
    StrBufAppendString(problem, "comment-end needs comment");
    return false;
  }

  if (draft->lists[ListRoots] != noText && draft->server == noText) {
    StrBufAppendString(problem, "server-roots needs server");
    return false;
  }

  return true;
}

// whether package id has the file name: in folder, its own folder, or in the built-in
// package when folder is nullptr.
static bool
HasFile(const char *folder, const char *id, const char *name)
{
  if (!folder) return BuiltinFindFile(id, name) != nullptr;

  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/%s", folder, name);
  struct stat status;
  bool file = stat(path.data, &status) == 0 && S_ISREG(status.st_mode);
  StrBufFree(&path);
  return file;
}

// reads the file name of package id into text, from folder or the built-in package as
// HasFile finds it.
static enum FileStatus
ReadFile(const char *folder, const char *id, const char *name, StrBuf *text)
{
  if (!folder) {
    const BuiltinFile *file = BuiltinFindFile(id, name);
    if (!file) return FileStatusNew;

    StrBufAppend(text, (const char *)file->data, file->length);
    return FileStatusOk;
  }

  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/%s", folder, name);
  FileStamp stamp;
  enum FileStatus status = FileRead(path.data, text, &stamp, nullptr);
  StrBufFree(&path);
  return status;
}

static void
AddProblem(Reading *reading, const char *id, bool builtin, const StrBuf *problem)
{
  if (reading->problemCount++ == 0) {
    const char *where = builtin ? "built-in " : "languages/";
    StrBufAppendFormat(&reading->problem, "%s%s/language: %s", where, id, problem->data);
  }
}

// reads the package id in folder, the languages folder, or the built-in one when folder
// is nullptr, adding it as a draft unless its language file is missing or bad.
static void
ReadPackage(Reading *reading, const char *folder, const char *id)
{
  StrBuf path = {};
  if (folder) StrBufAppendFormat(&path, "%s/%s", folder, id);
  const char *own = folder ? path.data : nullptr;
  u64 rewind = reading->texts.length;
  u64 slotRewind = reading->slotCount;

  Draft draft = {
    .id = AddText(reading, id, strlen(id)),
    .folder = folder ? AddText(reading, path.data, path.length) : noText,
    .name = noText,
    .symbol = noText,
    .commentStart = noText,
    .commentEnd = noText,
    .server = noText,
    .lists = {noText, noText, noText, noText, noText}
  };

  StrBuf problem = {};
  StrBuf file = {};
  enum FileStatus status = ReadFile(own, id, "language", &file);
  bool good = status == FileStatusOk;

  if (!good) StrBufAppendString(&problem, status == FileStatusNew ? "missing" : "unreadable");
  else good = ParseLanguage(reading, &draft, &file, &problem);

  if (good) {
    // the defaults: tree_sitter_<id with - as _>, injected by its id.
    if (draft.symbol == noText) {
      draft.symbol = AddText(reading, "tree_sitter_", 12);
      StrBufTruncate(&reading->texts, reading->texts.length - 1);

      for (const char *at = id; *at; at++) {
        StrBufAppendByte(&reading->texts, *at == '-' ? '_' : *at);
      }

      StrBufAppendByte(&reading->texts, '\0');
    }

    if (draft.lists[ListInjections] == noText) {
      draft.lists[ListInjections] = AddList(reading, id, strlen(id));
    }

    // a server's root is the folder of a git repository unless the package says.
    if (draft.lists[ListRoots] == noText) draft.lists[ListRoots] = AddList(reading, ".git", 4);

    // a built-in grammar serves a package in the folder too, as a static build cannot
    // load parser.so.
    const char *symbol = reading->texts.data + draft.symbol;
    draft.hasGrammar = BuiltinFindGrammar(symbol) || HasFile(own, id, "parser.so");
    draft.hasTags = HasFile(own, id, "tags.scm");

    if (reading->draftCount == reading->draftCapacity) {
      reading->draftCapacity = reading->draftCapacity ? reading->draftCapacity * 2 : 16;
      reading->drafts = MemoryResize(reading->drafts, reading->draftCapacity, sizeof(Draft));
    }

    reading->drafts[reading->draftCount++] = draft;
  } else {
    AddProblem(reading, id, !folder, &problem);
    StrBufTruncate(&reading->texts, rewind);
    reading->slotCount = slotRewind;
  }

  StrBufFree(&problem);
  StrBufFree(&file);
  StrBufFree(&path);
}

static int
CompareNames(const void *first, const void *second)
{
  return strcmp(*(char *const *)first, *(char *const *)second);
}

// the package folder names in folder, sorted so lookups do not depend on the file system.
static char **
ListPackages(const char *folder, u32 *count)
{
  *count = 0;
  DIR *directory = opendir(folder);
  if (!directory) return nullptr;

  u32 capacity = 0;
  char **names = nullptr;
  struct dirent *entry;

  while ((entry = readdir(directory))) {
    if (entry->d_name[0] == '.') continue;

    struct stat status;
    StrBuf path = {};
    StrBufAppendFormat(&path, "%s/%s", folder, entry->d_name);
    bool isFolder = stat(path.data, &status) == 0 && S_ISDIR(status.st_mode);
    StrBufFree(&path);
    if (!isFolder) continue;

    if (*count == capacity) {
      capacity = capacity ? capacity * 2 : 16;
      names = MemoryResize(names, capacity, sizeof(char *));
    }

    names[(*count)++] = strdup(entry->d_name);
  }

  closedir(directory);
  if (*count) qsort(names, *count, sizeof(char *), CompareNames);
  return names;
}

static const char *
TextAt(const char *texts, u64 offset)
{
  return offset == noText ? nullptr : texts + offset;
}

// moves the drafts into one allocation: the languages, then their lists, then the texts.
static void
Settle(const Reading *reading)
{
  u64 listSize = reading->slotCount * sizeof(const char *);
  u64 langSize = reading->draftCount * sizeof(Lang);
  u8 *block = MemoryAllocateZeroed(1, langSize + listSize + reading->texts.length + 1);
  langs = (Lang *)block;
  langCount = reading->draftCount;
  const char **lists = (const char **)(block + langSize);
  char *texts = (char *)(block + langSize + listSize);
  if (reading->texts.length) memcpy(texts, reading->texts.data, reading->texts.length);

  for (u64 slot = 0; slot < reading->slotCount; slot++) {
    lists[slot] = TextAt(texts, reading->slots[slot]);
  }

  // a missing list is the empty one: the terminator of any list will do, and there is
  // always the injection list.
  static const char *const empty[] = {nullptr};

  for (u32 index = 0; index < langCount; index++) {
    const Draft *draft = &reading->drafts[index];
    const char *const *list[ListKindCount];

    for (u32 kind = 0; kind < ListKindCount; kind++) {
      list[kind] = draft->lists[kind] == noText ? empty : lists + draft->lists[kind];
    }

    const char *commentStart = TextAt(texts, draft->commentStart);
    const char *commentEnd = TextAt(texts, draft->commentEnd);

    langs[index] = (Lang){
      .name = TextAt(texts, draft->name),
      .id = TextAt(texts, draft->id),
      .folder = TextAt(texts, draft->folder),
      .extensions = list[ListExtensions],
      .names = list[ListNames],
      .interpreters = list[ListInterpreters],
      .injectionIds = list[ListInjections],
      .serverRoots = list[ListRoots],
      .symbol = TextAt(texts, draft->symbol),
      .commentStart = commentStart,
      .commentEnd = commentStart && !commentEnd ? "" : commentEnd,
      .server = TextAt(texts, draft->server),
      .prose = draft->prose,
      .freshParse = draft->freshParse,
      .firstPatternWins = draft->firstPatternWins,
      .hasGrammar = draft->hasGrammar,
      .hasTags = draft->hasTags,
      .index = index
    };
  }
}

void
LangLoadAll(StrBuf *message)
{
  StrBuf folder = {};
  FileConfigPath("languages", &folder);

  u32 count;
  char **names = ListPackages(folder.data, &count);

  Reading reading = {};
  const char *const *builtin = builtinPackages;

  // the packages in the folder and the built-in ones, both sorted, are read in order of
  // id; a package in the folder replaces the built-in one of the same id.
  for (u32 index = 0; index < count || *builtin;) {
    i32 order = index == count ? 1 : *builtin ? strcmp(names[index], *builtin) : -1;
    if (order == 0) builtin++;

    if (order > 0) {
      ReadPackage(&reading, nullptr, *builtin++);
    } else {
      ReadPackage(&reading, folder.data, names[index]);
      free(names[index++]);
    }
  }

  Settle(&reading);

  if (reading.problemCount) {
    if (message->length) StrBufAppendString(message, "; ");
    StrBufAppend(message, reading.problem.data, reading.problem.length);
    u32 more = reading.problemCount - 1;
    if (more) StrBufAppendFormat(message, " (and %u more)", more);
  }

  free(names);
  free(reading.slots);
  free(reading.drafts);
  StrBufFree(&reading.texts);
  StrBufFree(&reading.problem);
  StrBufFree(&folder);
}

u32
LangCount(void)
{
  return langCount;
}

const Lang *
LangAt(u32 index)
{
  return &langs[index];
}

enum FileStatus
LangReadFile(const Lang *lang, const char *name, StrBuf *text)
{
  return ReadFile(lang->folder, lang->id, name, text);
}

static const Lang *
LangOfName(const char *name)
{
  for (u32 index = 0; index < langCount; index++) {
    const char *const *names = langs[index].names;

    for (u32 at = 0; names[at]; at++) {
      if (strcmp(names[at], name) == 0) return &langs[index];
    }
  }

  return nullptr;
}

// the language with an extension (ignoring case), or nullptr.
static const Lang *
LangOfExtension(const char *extension, u64 length)
{
  for (u32 index = 0; index < langCount; index++) {
    const char *const *extensions = langs[index].extensions;

    for (u32 at = 0; extensions[at]; at++) {
      const char *candidate = extensions[at];
      if (strlen(candidate) != length) continue;
      if (strncasecmp(candidate, extension, length) == 0) return &langs[index];
    }
  }

  return nullptr;
}

const Lang *
LangOfPath(const char *path, const Lang *script)
{
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;

  const Lang *lang = LangOfName(name);
  if (lang) return lang;

  // a leading dot starts a hidden name, not an extension.
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name) return script;

  return LangOfExtension(dot + 1, strlen(dot + 1));
}

const Lang *
LangOfInjection(const char *name, u64 length)
{
  for (u32 index = 0; index < langCount; index++) {
    const char *const *ids = langs[index].injectionIds;

    for (u32 at = 0; ids[at]; at++) {
      if (strlen(ids[at]) == length && strncasecmp(ids[at], name, length) == 0) {
        return &langs[index];
      }
    }
  }

  return LangOfExtension(name, length);
}

// the next word of a line from at, skipping blanks, as its start; at ends up after it.
static u64
NextWord(const char *line, u64 length, u64 *at)
{
  while (*at < length && (line[*at] == ' ' || line[*at] == '\t')) (*at)++;

  u64 start = *at;
  while (*at < length && line[*at] != ' ' && line[*at] != '\t') (*at)++;
  return start;
}

// whether the word from start to end names program, ignoring its folder.
static bool
RunsProgram(const char *line, u64 start, u64 end, const char *program)
{
  for (u64 at = start; at < end; at++) {
    if (line[at] == '/') start = at + 1;
  }

  return strlen(program) == end - start && memcmp(line + start, program, end - start) == 0;
}

const Lang *
LangOfScript(const char *line, u64 length)
{
  if (length < 2 || line[0] != '#' || line[1] != '!') return nullptr;

  u64 at = 2;
  u64 start = NextWord(line, length, &at);

  // env runs the first word that is not an option or a variable setting.
  if (RunsProgram(line, start, at, "env")) {
    do {
      start = NextWord(line, length, &at);
    } while (at > start && (line[start] == '-' || memchr(line + start, '=', at - start)));
  }

  for (u32 index = 0; index < langCount; index++) {
    const char *const *interpreters = langs[index].interpreters;

    for (u32 program = 0; interpreters[program]; program++) {
      if (RunsProgram(line, start, at, interpreters[program])) return &langs[index];
    }
  }

  return nullptr;
}
