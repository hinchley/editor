#include "lang.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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
  ListKindCount
};

typedef struct Draft {
  u64 id;
  u64 folder;
  u64 name;
  u64 symbol;
  u64 commentStart;
  u64 commentEnd;
  u64 lists[ListKindCount];
  bool prose;
  bool freshParse;
  bool firstPatternWins;
  bool hasParser;
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

// one directive of a language file.
typedef struct Directive {
  const char *key;
  enum {
    DirectiveText,
    DirectiveWord,
    DirectiveList,
    DirectiveFlag
  } kind;
} Directive;

static const Directive directives[] = {
  {"name", DirectiveText},
  {"extensions", DirectiveList},
  {"files", DirectiveList},
  {"interpreters", DirectiveList},
  {"inject", DirectiveList},
  {"symbol", DirectiveWord},
  {"comment", DirectiveWord},
  {"comment-end", DirectiveWord},
  {"prose", DirectiveFlag},
  {"fresh-parse", DirectiveFlag},
  {"first-pattern-wins", DirectiveFlag}
};

static constexpr u32 directiveCount = sizeof directives / sizeof directives[0];

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
) {
  const char *key = directives[directive].key;

  switch (directives[directive].kind) {
  case DirectiveWord:
    if (HasBlank(value, length)) {
      StrBufAppendFormat(problem, "%s takes one word", key);
      return false;
    }
    [[fallthrough]];

  case DirectiveText: {
    u64 text = AddText(reading, value, length);
    if (strcmp(key, "name") == 0) draft->name = text;
    else if (strcmp(key, "symbol") == 0) draft->symbol = text;
    else if (strcmp(key, "comment") == 0) draft->commentStart = text;
    else draft->commentEnd = text;
    return true;
  }

  case DirectiveList: {
    u64 first = AddList(reading, value, length);
    enum ListKind list = ListInjections;
    if (strcmp(key, "extensions") == 0) list = ListExtensions;
    else if (strcmp(key, "files") == 0) list = ListNames;
    else if (strcmp(key, "interpreters") == 0) list = ListInterpreters;
    draft->lists[list] = first;
    return true;
  }

  case DirectiveFlag: {
    bool yes = length == 3 && memcmp(value, "yes", 3) == 0;
    bool no = length == 2 && memcmp(value, "no", 2) == 0;

    if (!yes && !no) {
      StrBufAppendFormat(problem, "%s takes yes or no", key);
      return false;
    }

    if (strcmp(key, "prose") == 0) draft->prose = yes;
    else if (strcmp(key, "fresh-parse") == 0) draft->freshParse = yes;
    else draft->firstPatternWins = yes;
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
      StrBufAppendFormat(problem, "line %lu: unknown key \"%.*s\"", line + 1, (int)keyLength,
        text + keyStart);
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
    bool applied = Apply(reading, draft, directive, text + valueStart, valueEnd - valueStart,
      &reason);
    if (!applied) StrBufAppendFormat(problem, "line %lu: %s", line + 1, reason.data);
    StrBufFree(&reason);
    if (!applied) return false;
  }

  bool named = draft->lists[ListExtensions] != noText || draft->lists[ListNames] != noText ||
    draft->lists[ListInterpreters] != noText;

  if (named && draft->name == noText) {
    StrBufAppendString(problem, "a language with extensions, files or interpreters needs a name");
    return false;
  }

  if (draft->commentEnd != noText && draft->commentStart == noText) {
    StrBufAppendString(problem, "comment-end needs comment");
    return false;
  }

  return true;
}

static bool
IsFile(const char *folder, const char *name)
{
  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/%s", folder, name);
  struct stat status;
  bool file = stat(path.data, &status) == 0 && S_ISREG(status.st_mode);
  StrBufFree(&path);
  return file;
}

static void
AddProblem(Reading *reading, const char *id, const StrBuf *problem)
{
  if (reading->problemCount++ == 0) {
    StrBufAppendFormat(&reading->problem, "languages/%s/language: %s", id, problem->data);
  }
}

// reads the package in folder/id, adding it as a draft unless its language file is
// missing or bad.
static void
ReadPackage(Reading *reading, const char *folder, const char *id)
{
  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/%s", folder, id);
  u64 rewind = reading->texts.length;
  u64 slotRewind = reading->slotCount;

  Draft draft = {
    .id = AddText(reading, id, strlen(id)),
    .folder = AddText(reading, path.data, path.length),
    .name = noText,
    .symbol = noText,
    .commentStart = noText,
    .commentEnd = noText,
    .lists = {noText, noText, noText, noText}
  };

  StrBuf problem = {};
  StrBuf file = {};
  FileStamp stamp;
  StrBufAppendString(&path, "/language");
  enum FileStatus status = FileRead(path.data, &file, &stamp, nullptr);
  bool good = status == FileStatusOk;

  if (!good) StrBufAppendString(&problem, status == FileStatusNew ? "missing" : "unreadable");
  else good = ParseLanguage(reading, &draft, &file, &problem);

  if (good) {
    const char *base = reading->texts.data + draft.folder;
    draft.hasParser = IsFile(base, "parser.so");
    draft.hasTags = IsFile(base, "tags.scm");

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

    if (reading->draftCount == reading->draftCapacity) {
      reading->draftCapacity = reading->draftCapacity ? reading->draftCapacity * 2 : 16;
      reading->drafts = MemoryResize(reading->drafts, reading->draftCapacity, sizeof(Draft));
    }

    reading->drafts[reading->draftCount++] = draft;
  } else {
    AddProblem(reading, id, &problem);
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

  char **names = nullptr;
  u32 capacity = 0;
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
      .symbol = TextAt(texts, draft->symbol),
      .commentStart = commentStart,
      .commentEnd = commentStart && !commentEnd ? "" : commentEnd,
      .prose = draft->prose,
      .freshParse = draft->freshParse,
      .firstPatternWins = draft->firstPatternWins,
      .hasParser = draft->hasParser,
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

  Reading reading = {};
  u32 count;
  char **names = ListPackages(folder.data, &count);

  for (u32 index = 0; index < count; index++) {
    ReadPackage(&reading, folder.data, names[index]);
    free(names[index]);
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

const Lang *
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
