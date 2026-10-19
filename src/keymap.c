#include "keymap.h"

static constexpr u8 shift = KeyModifierShift;
static constexpr u8 alt = KeyModifierAlt;
static constexpr u8 control = KeyModifierControl;

const Binding keymapBindings[] = {
  // movement.
  {{KeyUp, 0}, CommandMoveUp, "move up"},
  {{KeyDown, 0}, CommandMoveDown, "move down"},
  {{KeyLeft, 0}, CommandMoveLeft, "move left"},
  {{KeyRight, 0}, CommandMoveRight, "move right"},
  {{KeyHome, 0}, CommandMoveLineStart, "move to the first non-blank, then the line start"},
  {{KeyEnd, 0}, CommandMoveLineEnd, "move to the line end"},
  {{KeyPageUp, 0}, CommandMovePageUp, "move up a page"},
  {{KeyPageDown, 0}, CommandMovePageDown, "move down a page"},
  {{KeyLeft, control}, CommandMoveWordLeft, "move to the previous word"},
  {{KeyRight, control}, CommandMoveWordRight, "move to the next word"},
  {{KeyHome, control}, CommandMoveFileStart, "move to the start of the buffer"},
  {{KeyEnd, control}, CommandMoveFileEnd, "move to the end of the buffer"},
  {{KeyUp, alt}, CommandScrollUp, "scroll up a line (the cursor stays put)"},
  {{KeyDown, alt}, CommandScrollDown, "scroll down a line (the cursor stays put)"},

  // selection.
  {{KeyUp, shift}, CommandSelectUp, "extend the selection up"},
  {{KeyDown, shift}, CommandSelectDown, "extend the selection down"},
  {{KeyLeft, shift}, CommandSelectLeft, "extend the selection left"},
  {{KeyRight, shift}, CommandSelectRight, "extend the selection right"},
  {
    {KeyHome, shift},
    CommandSelectLineStart,
    "extend the selection to the first non-blank, then the line start"
  },
  {{KeyEnd, shift}, CommandSelectLineEnd, "extend the selection to the line end"},
  {{KeyPageUp, shift}, CommandSelectPageUp, "extend the selection up a page"},
  {{KeyPageDown, shift}, CommandSelectPageDown, "extend the selection down a page"},
  {{KeyLeft, control | shift}, CommandSelectWordLeft, "extend the selection to the previous word"},
  {{KeyRight, control | shift}, CommandSelectWordRight, "extend the selection to the next word"},
  {
    {KeyHome, control | shift},
    CommandSelectFileStart,
    "extend the selection to the start of the buffer"
  },
  {
    {KeyEnd, control | shift},
    CommandSelectFileEnd,
    "extend the selection to the end of the buffer"
  },
  {{'a', control}, CommandSelectAll, "select the whole buffer"},
  {{'m', control}, CommandSelectBrackets, "select inside the closest brackets (again to widen)"},
  {
    {KeyRight, alt},
    CommandExpandSelection,
    "expand the selection to the enclosing syntax node (or brackets)"
  },
  {{KeyLeft, alt}, CommandShrinkSelection, "shrink the selection back a step"},
  {{KeyUp, control | shift}, CommandSelectionTopUp, "grow the selection by a line above"},
  {{KeyDown, control | shift}, CommandSelectionBottomDown, "grow the selection by a line below"},
  {{KeyDown, alt | shift}, CommandSelectionTopDown, "shrink the selection by a line at the top"},
  {{KeyUp, alt | shift}, CommandSelectionBottomUp, "shrink the selection by a line at the bottom"},
  {
    {KeyEscape, 0},
    CommandCancel,
    "collapse to one cursor, clear the selection, or clear highlights"
  },

  // editing.
  {{KeyEnter, 0}, CommandNewline, "insert a line break"},
  {{KeyTab, 0}, CommandIndent, "insert a tab (or indent the selected lines)"},
  {{KeyTab, shift}, CommandDedent, "dedent the line (or the selected lines)"},
  {{KeyBackspace, 0}, CommandDeleteBackward, "delete the character before the cursor"},
  {{KeyDelete, 0}, CommandDeleteForward, "delete the character after the cursor"},
  {{KeyBackspace, control}, CommandDeleteWordBackward, "delete the word before the cursor"},
  {{'z', control}, CommandUndo, "undo"},
  {{'y', control}, CommandRedo, "redo"},
  {{'c', control}, CommandCopy, "copy the selection (or the line)"},
  {{'x', control}, CommandCut, "cut the selection (or the line)"},
  {{'v', control}, CommandPaste, "paste"},
  {{KeyUp, control}, CommandMoveLinesUp, "move the line (or the selected lines) up"},
  {{KeyDown, control}, CommandMoveLinesDown, "move the line (or the selected lines) down"},
  {{'d', control}, CommandDuplicateLine, "duplicate the line (or the selected lines)"},
  {{'k', control}, CommandDeleteLine, "delete the line (or the selected lines)"},
  {{'/', control}, CommandToggleComment, "toggle comments on the line (or the selected lines)"},
  {{'j', alt}, CommandJoinLines, "join the selected lines (or the line and the next)"},
  {{'o', alt}, CommandSortLines, "sort the selected lines"},
  {
    {'t', control},
    CommandTrimLines,
    "trim trailing whitespace from the line (or the selected lines)"
  },
  {{'t', control | shift}, CommandTrimWhitespace, "trim trailing whitespace from every line"},
  {{'u', alt | shift}, CommandUpperCase, "convert the selection to upper case"},
  {{'l', alt | shift}, CommandLowerCase, "convert the selection to lower case"},
  {{'c', alt}, CommandSpellCheck, "spell check the buffer"},
  {
    {'c', alt | shift},
    CommandSpellWord,
    "add the word at the cursor to the word list (or remove it)"
  },

  // multiple cursors.
  {
    {'e', control},
    CommandAddCursorWord,
    "select the word, then add a cursor at the next occurrence (or search match)"
  },
  {
    {'e', alt},
    CommandSkipCursorWord,
    "move the newest cursor to the next occurrence, skipping this one"
  },
  {{'e', control | shift}, CommandAddCursorAll, "add a cursor at every occurrence"},
  {{KeyUp, control | alt}, CommandAddCursorAbove, "add a cursor on the line above"},
  {{KeyDown, control | alt}, CommandAddCursorBelow, "add a cursor on the line below"},

  // buffers and files.
  {{'n', control}, CommandNewBuffer, "create a new empty buffer"},
  {
    {'o', control},
    CommandOpen,
    "open a file (in a new buffer) or a folder (replacing every buffer)"
  },
  {{'s', control}, CommandSave, "save the buffer (asks for a name if it has none)"},
  {{'s', control | shift}, CommandSaveAll, "save every buffer with unsaved changes"},
  {{'r', alt}, CommandReload, "reload the buffer from disk (asks before discarding changes)"},
  {{'r', alt | shift}, CommandLoadNewFiles, "load files added to the folder since it was opened"},
  {{'q', alt}, CommandCloseBuffer, "close the buffer (offers to save changes)"},
  {{'x', alt}, CommandFileMenu, "rename, copy or delete the buffer's file"},
  {{'x', alt | shift}, CommandCopyPath, "copy the path of the buffer's file"},
  {{';', alt}, CommandToggleLineEndings, "toggle line endings between LF and CRLF"},
  {
    {'i', alt},
    CommandStatistics,
    "count the lines, words, characters and bytes (of the selection)"
  },
  {{KeyPageDown, control}, CommandNextBuffer, "switch to the next buffer"},
  {{KeyPageUp, control}, CommandPreviousBuffer, "switch to the previous buffer"},
  {{KeyTab, control}, CommandLastBuffer, "switch to the buffer used before this one"},
  {{'p', control}, CommandPickBuffer, "pick a buffer"},
  {{'u', control}, CommandPickUnsaved, "pick a buffer with unsaved changes"},
  {{'q', control}, CommandQuit, "quit (offers to save changes)"},

  // split view.
  {
    {KeyRight, control | alt},
    CommandSplitSame,
    "split the window, showing this buffer in both views"
  },
  {{'b', control}, CommandSplitNew, "split the window, with a new empty buffer in the new view"},
  {{KeyLeft, control | alt}, CommandCloseView, "close this view"},
  {{'j', control}, CommandFocusOtherView, "move focus to the other view"},
  {{'i', control}, CommandSwapViews, "swap the two views"},

  // search.
  {{'f', control}, CommandFind, "search the buffer"},
  {{'n', alt}, CommandFindNext, "go to the next match"},
  {{'p', alt}, CommandFindPrevious, "go to the previous match"},
  {{'w', control}, CommandJumpWord, "go to the next occurrence of the word at the cursor"},
  {{'r', control}, CommandReplace, "search and replace in the buffer"},
  {{'f', alt | shift}, CommandProjectSearch, "search the loaded files"},
  {{'f', alt}, CommandProjectResults, "reopen the list of matches in the loaded files"},
  {{'l', control}, CommandGoToLine, "go to a line number"},

  // navigation.
  {{'[', alt}, CommandJumpBack, "go back to where the cursor was before a jump"},
  {{']', alt}, CommandJumpForward, "go forward again after going back"},
  {{'[', control}, CommandPreviousSymbol, "go to the previous definition"},
  {{']', control}, CommandNextSymbol, "go to the next definition"},
  {{'m', alt}, CommandJumpBracket, "go to the matching bracket"},
  {{'s', alt}, CommandFileSymbols, "pick a symbol in this buffer"},
  {{'s', alt | shift}, CommandProjectSymbols, "pick a symbol in any loaded file"},
  {{'d', alt}, CommandGoToDefinition, "go to the definition of the name at the cursor"},
  {
    {'d', alt | shift},
    CommandShowSignature,
    "show the signature of the name (or call) at the cursor"
  },

  // changes since the last git commit.
  {{']', alt | shift}, CommandNextChange, "go to the next change"},
  {{'[', alt | shift}, CommandPreviousChange, "go to the previous change"},
  {{'g', alt}, CommandChangePanel, "show or hide the change at the cursor beside the buffer"},
  {{'z', alt}, CommandRevertChange, "revert the change at the cursor to the last commit"},
  {{'g', control}, CommandPickChanged, "pick a change since the last commit"},

  // display and settings.
  {{'t', alt}, CommandToggleExpandTab, "toggle indenting with spaces or tabs"},
  {{'w', alt}, CommandToggleSoftWrap, "toggle soft wrap"},
  {{'l', alt}, CommandToggleLineNumbers, "toggle line numbers"},
  {{'.', alt}, CommandToggleWhitespace, "toggle showing whitespace"},
  {{'\\', alt}, CommandToggleRuler, "toggle the vertical ruler"},
  {{'v', alt}, CommandToggleServers, "toggle language servers (or restart stopped ones)"},
  {{'\\', alt | shift}, CommandSetRulerColumn, "set the ruler column (and show the ruler)"},
  {{'h', control}, CommandHelp, "show this list of shortcuts"},
  {{'h', alt}, CommandLearn, "practise the shortcuts (esc to stop)"}
};

const u32 keymapBindingCount = sizeof keymapBindings / sizeof keymapBindings[0];

const Binding *
KeymapFind(Key key)
{
  for (u32 index = 0; index < keymapBindingCount; index++) {
    if (KeyEqual(keymapBindings[index].key, key)) return &keymapBindings[index];
  }

  return nullptr;
}
