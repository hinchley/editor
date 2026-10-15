#pragma once

// the editor's key bindings.
// this table is the single source of truth for both dispatch and the alt+h help list.
// keys used inside prompts and pickers are handled there and are not listed here.
#include "key.h"
#include "types.h"

enum Command {
  CommandMoveUp,
  CommandMoveDown,
  CommandMoveLeft,
  CommandMoveRight,
  CommandMoveLineStart,
  CommandMoveLineEnd,
  CommandMovePageUp,
  CommandMovePageDown,
  CommandMoveWordLeft,
  CommandMoveWordRight,
  CommandMoveFileStart,
  CommandMoveFileEnd,
  CommandScrollUp,
  CommandScrollDown,
  CommandSelectUp,
  CommandSelectDown,
  CommandSelectLeft,
  CommandSelectRight,
  CommandSelectLineStart,
  CommandSelectLineEnd,
  CommandSelectPageUp,
  CommandSelectPageDown,
  CommandSelectWordLeft,
  CommandSelectWordRight,
  CommandSelectFileStart,
  CommandSelectFileEnd,
  CommandSelectAll,
  CommandSelectBrackets,
  CommandExpandSelection,
  CommandShrinkSelection,
  CommandCancel,
  CommandNewline,
  CommandIndent,
  CommandDedent,
  CommandDeleteBackward,
  CommandDeleteForward,
  CommandDeleteWordBackward,
  CommandUndo,
  CommandRedo,
  CommandCopy,
  CommandCut,
  CommandPaste,
  CommandMoveLinesUp,
  CommandMoveLinesDown,
  CommandDuplicateLine,
  CommandDeleteLine,
  CommandJumpBracket,
  CommandJumpWord,
  CommandToggleComment,
  CommandSortLines,
  CommandJoinLines,
  CommandTrimWhitespace,
  CommandSpellCheck,
  CommandAddCursorWord,
  CommandAddCursorAbove,
  CommandAddCursorBelow,
  CommandNewBuffer,
  CommandOpen,
  CommandReload,
  CommandToggleLineEndings,
  CommandCopyPath,
  CommandStatistics,
  CommandNextBuffer,
  CommandPreviousBuffer,
  CommandLastBuffer,
  CommandPickBuffer,
  CommandPickUnsaved,
  CommandCloseBuffer,
  CommandFileMenu,
  CommandSplitSame,
  CommandSplitNew,
  CommandFocusOtherView,
  CommandSwapViews,
  CommandCloseView,
  CommandFind,
  CommandFindNext,
  CommandFindPrevious,
  CommandProjectSearch,
  CommandProjectResults,
  CommandGoToLine,
  CommandJumpBack,
  CommandJumpForward,
  CommandFileSymbols,
  CommandNextSymbol,
  CommandPreviousSymbol,
  CommandGoToDefinition,
  CommandShowSignature,
  CommandProjectSymbols,
  CommandReplace,
  CommandToggleExpandTab,
  CommandToggleSoftWrap,
  CommandToggleLineNumbers,
  CommandToggleWhitespace,
  CommandToggleRuler,
  CommandSetRulerColumn,
  CommandSave,
  CommandSaveAll,
  CommandQuit,
  CommandHelp
};

typedef struct Binding {
  Key key;
  enum Command command;
  const char *help;
} Binding;

extern const Binding keymapBindings[];
extern const u32 keymapBindingCount;

// the binding for a key, or nullptr when the key is unbound.
const Binding *
KeymapFind(Key key);
