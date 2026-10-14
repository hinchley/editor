#include "theme.h"

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
  [ThemeGutter] = {"linenr", {240, colorDefault, 0}},
  [ThemeWhitespace] = {"whitespace", {238, colorDefault, 0}},
  [ThemeControl] = {"control", {204, colorDefault, 0}},
  [ThemeRuler] = {"ruler", {238, colorDefault, 0}},
  [ThemeStatus] = {"status", {colorDefault, colorDefault, StyleAttributeReverse}},
  [ThemeStatusDim] = {"statusdim", {245, 236, 0}},
  [ThemeMessage] = {"message", {colorDefault, colorDefault, 0}},

  // selected text keeps its own colour on this background.
  [ThemeSelection] = {"selection", {colorDefault, 238, 0}},
  [ThemeSelectionWhitespace] = {"selectionwhitespace", {244, 238, 0}}
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
