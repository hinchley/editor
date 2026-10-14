#include "lang.h"

#include <string.h>
#include <strings.h>

static const Lang langTable[] = {
  {"C", {"c", "h", nullptr}, "//", ""},
  {"HTML", {"html", "htm", nullptr}, "<!--", "-->"},
  {"CSS", {"css", nullptr}, "/*", "*/"},
  {"JSON", {"json", nullptr}, nullptr, nullptr},
  {"JavaScript", {"js", "mjs", "cjs", nullptr}, "//", ""},
  {"Markdown", {"md", "markdown", nullptr}, "<!--", "-->"}
};

static constexpr u32 langCount = sizeof langTable / sizeof langTable[0];

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
LangOfPath(const char *path)
{
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;

  // a leading dot starts a hidden name, not an extension.
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name) return nullptr;

  return LangOfExtension(dot + 1, strlen(dot + 1));
}
