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
  {{KeyHome, 0}, CommandMoveLineStart, "move to first non-blank, then line start"},
  {{KeyEnd, 0}, CommandMoveLineEnd, "move to line end"},
  {{KeyPageUp, 0}, CommandMovePageUp, "move up one page"},
  {{KeyPageDown, 0}, CommandMovePageDown, "move down one page"},
  {{KeyLeft, control}, CommandMoveWordLeft, "move to previous word"},
  {{KeyRight, control}, CommandMoveWordRight, "move to next word"},
  {{KeyHome, control}, CommandMoveFileStart, "move to start of file"},
  {{KeyEnd, control}, CommandMoveFileEnd, "move to end of file"},
  {{KeyUp, control}, CommandScrollUp, "scroll the view up one line (cursor stays put)"},
  {{KeyDown, control}, CommandScrollDown, "scroll the view down one line (cursor stays put)"},

  // selection.
  {{KeyUp, shift}, CommandSelectUp, "extend selection up"},
  {{KeyDown, shift}, CommandSelectDown, "extend selection down"},
  {{KeyLeft, shift}, CommandSelectLeft, "extend selection left"},
  {{KeyRight, shift}, CommandSelectRight, "extend selection right"},
  {
    {KeyHome, shift},
    CommandSelectLineStart,
    "extend selection to first non-blank, then line start"
  },
  {{KeyEnd, shift}, CommandSelectLineEnd, "extend selection to line end"},
  {{KeyPageUp, shift}, CommandSelectPageUp, "extend selection up one page"},
  {{KeyPageDown, shift}, CommandSelectPageDown, "extend selection down one page"},
  {{KeyLeft, control | shift}, CommandSelectWordLeft, "extend selection to previous word"},
  {{KeyRight, control | shift}, CommandSelectWordRight, "extend selection to next word"},
  {{KeyHome, control | shift}, CommandSelectFileStart, "extend selection to start of file"},
  {{KeyEnd, control | shift}, CommandSelectFileEnd, "extend selection to end of file"},
  {{'a', control}, CommandSelectAll, "select the whole file"},
  {
    {'m', control},
    CommandSelectBrackets,
    "extend selection to all text inside closest brackets"
  },
  {
    {KeyUp, alt},
    CommandExpandSelection,
    "extend selection to the enclosing syntax node (or brackets)"
  },
  {{KeyDown, alt}, CommandShrinkSelection, "shrink selection back to before alt+up"},
  {{KeyUp, alt | shift}, CommandSelectionTopUp, "grow selection by a line above"},
  {{KeyDown, alt | shift}, CommandSelectionBottomDown, "grow selection by a line below"},
  {
    {KeyDown, control | alt | shift},
    CommandSelectionTopDown,
    "shrink selection by a line at the top"
  },
  {
    {KeyUp, control | alt | shift},
    CommandSelectionBottomUp,
    "shrink selection by a line at the bottom"
  },
  {{KeyEscape, 0}, CommandCancel, "clear the selection, or collapse to a single cursor"},

  // editing.
  {{KeyEnter, 0}, CommandNewline, "insert a newline"},
  {{KeyTab, 0}, CommandIndent, "insert a tab, or indent the selected lines"},
  {{KeyTab, shift}, CommandDedent, "dedent the current line or the selected lines"},
  {{KeyBackspace, 0}, CommandDeleteBackward, "delete the character before the cursor"},
  {{KeyDelete, 0}, CommandDeleteForward, "delete the character after the cursor"},
  {{KeyBackspace, control}, CommandDeleteWordBackward, "delete the word before the cursor"},
  {{'z', control}, CommandUndo, "undo"},
  {{'y', control}, CommandRedo, "redo"},
  {{'c', control}, CommandCopy, "copy the selection (or the line)"},
  {{'x', control}, CommandCut, "cut the selection (or the line)"},
  {{'v', control}, CommandPaste, "paste the clipboard"},
  {
    {KeyUp, control | shift},
    CommandMoveLinesUp,
    "move the current line (or selected lines) up"
  },
  {
    {KeyDown, control | shift},
    CommandMoveLinesDown,
    "move the current line (or selected lines) down"
  },
  {{'d', control}, CommandDuplicateLine, "duplicate the current line"},
  {{'k', control}, CommandDeleteLine, "delete the current line"},
  {{'m', alt}, CommandJumpBracket, "jump to matching bracket"},
  {
    {'w', control},
    CommandJumpWord,
    "jump to the next occurrence of the word under the cursor"
  },
  {{'/', control}, CommandToggleComment, "toggle line comments"},
  {{'q', alt}, CommandSortLines, "sort the selected lines (or every line)"},
  {{'j', alt}, CommandJoinLines, "join the selected lines (or this line and the next)"},
  {{'t', alt | shift}, CommandTrimWhitespace, "trim trailing whitespace from every line"},
  {
    {'u', alt | shift},
    CommandUpperCase,
    "convert the selection (or the whole file) to upper case"
  },
  {
    {'l', alt | shift},
    CommandLowerCase,
    "convert the selection (or the whole file) to lower case"
  },
  {{'s', alt}, CommandSpellCheck, "spell check the current document"},

  // multiple cursors.
  {
    {'e', control},
    CommandAddCursorWord,
    "select the word, then add a cursor at each next occurrence (or search match)"
  },
  {
    {'e', control | shift},
    CommandSkipCursorWord,
    "move the newest cursor on to the next occurrence, skipping this one"
  },
  {{'e', control | alt}, CommandAddCursorAll, "add a cursor at every occurrence"},
  {{KeyUp, control | alt}, CommandAddCursorAbove, "add a cursor on the line above"},
  {{KeyDown, control | alt}, CommandAddCursorBelow, "add a cursor on the line below"},

  // buffers and files.
  {{'n', control}, CommandNewBuffer, "create a new empty buffer"},
  {{'o', control}, CommandOpen, "open a file (new buffer) or folder (replaces all)"},
  {{'r', alt}, CommandReload, "reload the file from disk (discards changes)"},
  {
    {'r', alt | shift},
    CommandLoadNewFiles,
    "load files added to the open folder since it was read"
  },
  {{'e', alt}, CommandToggleLineEndings, "toggle line endings between LF and CRLF"},
  {{'c', alt | shift}, CommandCopyPath, "copy the file's path to the clipboard"},
  {
    {'i', alt},
    CommandStatistics,
    "count the lines, words, characters and bytes (of the selection)"
  },
  {{KeyPageDown, control}, CommandNextBuffer, "switch to the next buffer"},
  {{KeyPageUp, control}, CommandPreviousBuffer, "switch to the previous buffer"},
  {{KeyTab, control}, CommandLastBuffer, "switch to the last-used buffer"},
  {{'p', control}, CommandPickBuffer, "pick a buffer from a filterable list"},
  {{'u', control}, CommandPickUnsaved, "pick from buffers with unsaved changes"},
  {{'c', alt}, CommandCloseBuffer, "close the buffer (offers to save changes)"},
  {{'x', alt}, CommandFileMenu, "rename, copy or delete the file"},
  {{'s', control}, CommandSave, "save the file (prompts for a name if unnamed)"},
  {{'s', control | shift}, CommandSaveAll, "save every buffer with unsaved changes"},
  {{'q', control}, CommandQuit, "quit the editor"},

  // split view.
  {
    {KeyRight, control | alt},
    CommandSplitSame,
    "split the window, showing this buffer in both views"
  },
  {{'b', control}, CommandSplitNew, "split the window, opening a new empty buffer"},
  {{'j', control}, CommandFocusOtherView, "move focus to the other view"},
  {{'t', control}, CommandSwapViews, "swap the two views"},
  {{'h', control}, CommandCloseView, "close this view of the split"},

  // search.
  {{'f', control}, CommandFind, "incremental search"},
  {{'n', alt}, CommandFindNext, "jump to the next match"},
  {{'p', alt}, CommandFindPrevious, "jump to the previous match"},
  {{'f', alt | shift}, CommandProjectSearch, "search the loaded files and list the matches"},
  {{'f', alt}, CommandProjectResults, "reopen the list of project search matches"},
  {{'r', control}, CommandReplace, "search and replace"},
  {{'l', control}, CommandGoToLine, "go to a line number"},

  // navigation.
  {{KeyLeft, alt}, CommandJumpBack, "go back to where the cursor was before a jump"},
  {{KeyRight, alt}, CommandJumpForward, "go forward again after going back"},
  {{'o', alt}, CommandFileSymbols, "pick a symbol of this file from a filterable list"},
  {{']', alt}, CommandNextSymbol, "go to the next definition"},
  {{'[', alt}, CommandPreviousSymbol, "go to the previous definition"},
  {{'d', alt}, CommandGoToDefinition, "go to the definition of the name at the cursor"},
  {{'k', alt}, CommandShowSignature, "show the signature of the name or call at the cursor"},
  {{'o', alt | shift}, CommandProjectSymbols, "pick a symbol of the loaded files"},

  // display and settings.
  {{'t', alt}, CommandToggleExpandTab, "toggle expand tab to spaces"},
  {{'w', alt}, CommandToggleSoftWrap, "toggle soft wrap"},
  {{'l', alt}, CommandToggleLineNumbers, "toggle line numbers"},
  {{'.', alt}, CommandToggleWhitespace, "toggle whitespace display"},
  {{'g', alt}, CommandToggleRuler, "toggle the vertical ruler"},
  {
    {'g', control},
    CommandSetRulerColumn,
    "set the vertical ruler column (and show the ruler)"
  },
  {{'h', alt}, CommandHelp, "show this list of shortcuts"}
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
