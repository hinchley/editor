#pragma once

// highlight groups and their colours.
// syntax groups colour text by tree-sitter capture (wired up with highlighting), and the
// remaining groups colour parts of the user interface.
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
  ThemeGutter,
  ThemeWhitespace,
  ThemeControl,
  ThemeRuler,
  ThemeStatus,
  ThemeStatusDim,
  ThemeMessage,
  ThemeGroupCount
};

// the style a group draws with.
Style
ThemeStyle(enum ThemeGroup group);

// the group's name, as used in capture names.
const char *
ThemeName(enum ThemeGroup group);
