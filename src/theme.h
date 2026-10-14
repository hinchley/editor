#pragma once

// highlight groups and their colours.
// syntax groups colour text by tree-sitter capture, and the remaining groups colour parts
// of the user interface.
#include "screen.h"

enum ThemeGroup {
  ThemeNone,
  ThemeKeyword,
  ThemeString,
  ThemeComment,
  ThemeNumber,
  ThemeType,
  ThemeFunction,
  ThemeConstant,
  ThemeVariable,
  ThemeBuiltin,
  ThemeOperator,
  ThemePunctuation,
  ThemeProperty,
  ThemeTag,
  ThemeAttribute,
  ThemeConstructor,
  ThemeEscape,
  ThemeLabel,
  ThemeNamespace,

  // markup (markdown): headings, code, emphasis, strong emphasis, urls and link text.
  ThemeTitle,
  ThemeLiteral,
  ThemeEmphasis,
  ThemeStrong,
  ThemeUri,
  ThemeReference,

  ThemeGutter,
  ThemeWhitespace,
  ThemeControl,
  ThemeRuler,
  ThemeStatus,
  ThemeStatusDim,
  ThemeMessage,
  ThemeSelection,
  ThemeSelectionWhitespace,
  ThemeMatch,
  ThemeMatchSelected,
  ThemeMisspelt,
  ThemeCursor,
  ThemeGroupCount
};

// highlighting keeps a group in six bits.
static_assert(ThemeGroupCount <= 64);

// the style a group draws with.
Style
ThemeStyle(enum ThemeGroup group);

// the group's name, as used in capture names.
const char *
ThemeName(enum ThemeGroup group);

// finds the syntax group a tree-sitter capture name is coloured with.
// a capture uses the group named by its first component ("string.special" is a string),
// except that any "*.builtin" capture is a builtin and markup captures ("text.title") use
// their second component.
// returns false for a name with no group, which colours nothing ("none" is a group, and
// clears the colour).
bool
ThemeOfCapture(const char *name, enum ThemeGroup *group);
