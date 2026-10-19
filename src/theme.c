#include "theme.h"

#include <string.h>

typedef struct ThemeEntry {
  const char *name;
  Style style;
} ThemeEntry;

// foreground colours are 256 colour palette indexes.
static const ThemeEntry themeEntries[ThemeGroupCount] = {
  [ThemeNone] = {"none", {colorDefault, colorDefault, 0, colorDefault}},
  [ThemeKeyword] = {"keyword", {170, colorDefault, 0, colorDefault}},
  [ThemeString] = {"string", {114, colorDefault, 0, colorDefault}},
  [ThemeComment] = {"comment", {245, colorDefault, 0, colorDefault}},
  [ThemeNumber] = {"number", {215, colorDefault, 0, colorDefault}},
  [ThemeType] = {"type", {180, colorDefault, 0, colorDefault}},
  [ThemeFunction] = {"function", {75, colorDefault, 0, colorDefault}},
  [ThemeConstant] = {"constant", {215, colorDefault, 0, colorDefault}},
  [ThemeVariable] = {"variable", {colorDefault, colorDefault, 0, colorDefault}},
  [ThemeBuiltin] = {"builtin", {204, colorDefault, 0, colorDefault}},
  [ThemeOperator] = {"operator", {250, colorDefault, 0, colorDefault}},
  [ThemePunctuation] = {"punctuation", {245, colorDefault, 0, colorDefault}},
  [ThemeProperty] = {"property", {75, colorDefault, 0, colorDefault}},
  [ThemeTag] = {"tag", {204, colorDefault, 0, colorDefault}},
  [ThemeAttribute] = {"attribute", {215, colorDefault, 0, colorDefault}},
  [ThemeConstructor] = {"constructor", {180, colorDefault, 0, colorDefault}},
  [ThemeEscape] = {"escape", {215, colorDefault, 0, colorDefault}},
  [ThemeLabel] = {"label", {204, colorDefault, 0, colorDefault}},
  [ThemeNamespace] = {"namespace", {180, colorDefault, 0, colorDefault}},
  [ThemeTitle] = {"title", {75, colorDefault, StyleAttributeBold, colorDefault}},
  [ThemeLiteral] = {"literal", {114, colorDefault, 0, colorDefault}},
  [ThemeEmphasis] = {"emphasis", {colorDefault, colorDefault, StyleAttributeItalic, colorDefault}},
  [ThemeStrong] = {"strong", {colorDefault, colorDefault, StyleAttributeBold, colorDefault}},
  [ThemeUri] = {"uri", {75, colorDefault, StyleAttributeUnderline, colorDefault}},
  [ThemeReference] = {"reference", {180, colorDefault, 0, colorDefault}},
  [ThemeGutter] = {"linenr", {240, colorDefault, 0, colorDefault}},
  [ThemeChangeAdded] = {"changeadded", {71, colorDefault, 0, colorDefault}},
  [ThemeChangeModified] = {"changemodified", {179, colorDefault, 0, colorDefault}},
  [ThemeChangeRemoved] = {"changeremoved", {167, colorDefault, 0, colorDefault}},
  [ThemeDiffOld] = {"diffold", {colorDefault, 52, 0, colorDefault}},
  [ThemeDiffNew] = {"diffnew", {colorDefault, 22, 0, colorDefault}},
  [ThemeWhitespace] = {"whitespace", {238, colorDefault, 0, colorDefault}},
  [ThemeControl] = {"control", {204, colorDefault, 0, colorDefault}},
  [ThemeRuler] = {"ruler", {238, colorDefault, 0, colorDefault}},
  [ThemeStatus] = {"status", {colorDefault, colorDefault, StyleAttributeReverse, colorDefault}},
  [ThemeStatusDim] = {"statusdim", {245, 236, 0, colorDefault}},
  [ThemeMessage] = {"message", {colorDefault, colorDefault, 0, colorDefault}},

  // selected text keeps its own colour on this background.
  [ThemeSelection] = {"selection", {colorDefault, 238, 0, colorDefault}},
  [ThemeSelectionWhitespace] = {"selectionwhitespace", {244, 238, 0, colorDefault}},

  // search matches and unknown words, and the one that is selected.
  [ThemeMatch] = {"match", {235, 180, 0, colorDefault}},
  [ThemeMatchSelected] = {"matchselected", {235, 214, 0, colorDefault}},
  [ThemeMisspelt] = {"misspelt", {colorDefault, 52, 0, colorDefault}},

  // the colour of a diagnostic's line number, hint and the curly underline of its text.
  [ThemeError] = {"diagnosticerror", {167, colorDefault, 0, colorDefault}},
  [ThemeWarning] = {"diagnosticwarning", {179, colorDefault, 0, colorDefault}},
  [ThemeInfo] = {"diagnosticinfo", {75, colorDefault, 0, colorDefault}},

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
