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

  // gutter marks for lines added, modified or removed since the last commit, and the old
  // and new lines in the change panel.
  ThemeChangeAdded,
  ThemeChangeModified,
  ThemeChangeRemoved,
  ThemeDiffOld,
  ThemeDiffNew,

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

  // diagnostics from a language server: their marks and hints, by severity (information
  // and hints share one).
  ThemeError,
  ThemeWarning,
  ThemeInfo,
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
// except that any "*.builtin" capture is a builtin, markup captures ("text.title") use
// their second component, and a few names other groups go by ("boolean" is a constant).
// returns false for a name with no group, which colours nothing ("none" is a group, and
// clears the colour).
bool
ThemeOfCapture(const char *name, enum ThemeGroup *group);
