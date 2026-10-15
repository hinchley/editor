#include "lang.h"

#include <string.h>
#include <strings.h>

static const Lang langTable[] = {
  {"Bash", {"sh", "bash"}, {".bashrc", ".bash_profile", ".profile", "PKGBUILD"}, "#", ""},
  {"C", {"c", "h"}, {}, "//", ""},
  {"CSS", {"css"}, {}, "/*", "*/"},
  {"Go", {"go"}, {}, "//", ""},
  {"HTML", {"html", "htm"}, {}, "<!--", "-->"},
  {"JavaScript", {"js", "mjs", "cjs"}, {}, "//", ""},
  {"JSON", {"json"}, {}, nullptr, nullptr},
  {"Markdown", {"md", "markdown"}, {}, "<!--", "-->"},
  {"PHP", {"php"}, {}, "//", ""},
  {"SQL", {"sql"}, {}, "--", ""},
  {"XML", {"xml", "xsd", "xsl", "svg"}, {}, "<!--", "-->"},
  {"YAML", {"yaml", "yml"}, {}, "#", ""},
  {"Zig", {"zig", "zon"}, {}, "//", ""}
};

static constexpr u32 langCount = sizeof langTable / sizeof langTable[0];

static const Lang *
LangOfName(const char *name)
{
  for (u32 index = 0; index < langCount; index++) {
    const char *const *names = langTable[index].names;

    for (u32 at = 0; names[at]; at++) {
      if (strcmp(names[at], name) == 0) return &langTable[index];
    }
  }

  return nullptr;
}

const Lang *
LangOfExtension(const char *extension, u64 length)
{
  for (u32 index = 0; index < langCount; index++) {
    const char *const *extensions = langTable[index].extensions;

    for (u32 at = 0; extensions[at]; at++) {
      const char *candidate = extensions[at];
      if (strlen(candidate) != length) continue;
      if (strncasecmp(candidate, extension, length) == 0) return &langTable[index];
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

  if (!RunsProgram(line, start, at, "sh") && !RunsProgram(line, start, at, "bash")) {
    return nullptr;
  }

  for (u32 index = 0; index < langCount; index++) {
    if (strcmp(langTable[index].name, "Bash") == 0) return &langTable[index];
  }

  return nullptr;
}
