#include "theme.h"

#include <string.h>

typedef struct ThemeEntry {
  const char *name;
  Style style;
} ThemeEntry;

// foreground colours are 256 colour palette indexes.
static const ThemeEntry themeEntries[ThemeGroupCount] = {
  [ThemeNone] = {"none", {colorDefault, colorDefault, 0}},
  [ThemeKeyword] = {"keyword", {170, colorDefault, 0}},
  [ThemeString] = {"string", {114, colorDefault, 0}},
  [ThemeComment] = {"comment", {245, colorDefault, 0}},
  [ThemeNumber] = {"number", {215, colorDefault, 0}},
  [ThemeType] = {"type", {180, colorDefault, 0}},
  [ThemeFunction] = {"function", {75, colorDefault, 0}},
  [ThemeConstant] = {"constant", {215, colorDefault, 0}},
  [ThemeVariable] = {"variable", {colorDefault, colorDefault, 0}},
  [ThemeBuiltin] = {"builtin", {204, colorDefault, 0}},
  [ThemeOperator] = {"operator", {250, colorDefault, 0}},
  [ThemePunctuation] = {"punctuation", {245, colorDefault, 0}},
  [ThemeProperty] = {"property", {75, colorDefault, 0}},
  [ThemeTag] = {"tag", {204, colorDefault, 0}},
  [ThemeAttribute] = {"attribute", {215, colorDefault, 0}},
  [ThemeConstructor] = {"constructor", {180, colorDefault, 0}},
  [ThemeEscape] = {"escape", {215, colorDefault, 0}},
  [ThemeLabel] = {"label", {204, colorDefault, 0}},
  [ThemeNamespace] = {"namespace", {180, colorDefault, 0}},
  [ThemeTitle] = {"title", {75, colorDefault, StyleAttributeBold}},
  [ThemeLiteral] = {"literal", {114, colorDefault, 0}},
  [ThemeEmphasis] = {"emphasis", {colorDefault, colorDefault, StyleAttributeItalic}},
  [ThemeStrong] = {"strong", {colorDefault, colorDefault, StyleAttributeBold}},
  [ThemeUri] = {"uri", {75, colorDefault, StyleAttributeUnderline}},
  [ThemeReference] = {"reference", {180, colorDefault, 0}},
  [ThemeGutter] = {"linenr", {240, colorDefault, 0}},
  [ThemeChangeAdded] = {"changeadded", {71, colorDefault, 0}},
  [ThemeChangeModified] = {"changemodified", {179, colorDefault, 0}},
  [ThemeChangeRemoved] = {"changeremoved", {167, colorDefault, 0}},
  [ThemeDiffOld] = {"diffold", {colorDefault, 52, 0}},
  [ThemeDiffNew] = {"diffnew", {colorDefault, 22, 0}},
  [ThemeWhitespace] = {"whitespace", {238, colorDefault, 0}},
  [ThemeControl] = {"control", {204, colorDefault, 0}},
  [ThemeRuler] = {"ruler", {238, colorDefault, 0}},
  [ThemeStatus] = {"status", {colorDefault, colorDefault, StyleAttributeReverse}},
  [ThemeStatusDim] = {"statusdim", {245, 236, 0}},
  [ThemeMessage] = {"message", {colorDefault, colorDefault, 0}},

  // selected text keeps its own colour on this background.
  [ThemeSelection] = {"selection", {colorDefault, 238, 0}},
  [ThemeSelectionWhitespace] = {"selectionwhitespace", {244, 238, 0}},

  // search matches and unknown words, and the one that is selected.
  [ThemeMatch] = {"match", {235, 180, 0}},
  [ThemeMatchSelected] = {"matchselected", {235, 214, 0}},
  [ThemeMisspelt] = {"misspelt", {colorDefault, 52, 0}},

  // every cursor while there are several, as the terminal shows only one.
  [ThemeCursor] = {"cursor", {colorDefault, colorDefault, StyleAttributeReverse}}
};

Style
ThemeStyle(enum ThemeGroup group)
{
  return themeEntries[group].style;
}

const char *
ThemeName(enum ThemeGroup group)
{
  return themeEntries[group].name;
}

// first components some grammars' queries use for what the theme colours as another group.
static const struct {
  const char *name;
  enum ThemeGroup group;
} captureAliases[] = {
  {"boolean", ThemeConstant},
  {"character", ThemeString},
  {"conditional", ThemeKeyword},
  {"float", ThemeNumber},
  {"module", ThemeNamespace},
  {"storageclass", ThemeKeyword}
};

bool
ThemeOfCapture(const char *name, enum ThemeGroup *group)
{
  const char *last = strrchr(name, '.');

  if (strcmp(last ? last + 1 : name, "builtin") == 0) {
    *group = ThemeBuiltin;
    return true;
  }

  if (strncmp(name, "text.", 5) == 0) name += 5;
  u64 length = strcspn(name, ".");

  for (u32 index = ThemeNone; index < ThemeGutter; index++) {
    const char *candidate = themeEntries[index].name;

    if (strlen(candidate) == length && strncmp(candidate, name, length) == 0) {
      *group = (enum ThemeGroup)index;
      return true;
    }
  }

  for (u32 index = 0; index < sizeof captureAliases / sizeof captureAliases[0]; index++) {
    const char *alias = captureAliases[index].name;

    if (strlen(alias) == length && strncmp(alias, name, length) == 0) {
      *group = captureAliases[index].group;
      return true;
    }
  }

  return false;
}
