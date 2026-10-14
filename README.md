# editor

A personal terminal text editor for Linux, written in C23.

It targets Wayland and the foot, alacritty and ghostty terminals, and requires the Kitty Keyboard Protocol. There is no fallback for legacy terminals.

> Status: early development (milestone M4, editing, undo, prompts and clipboard). The editor edits one file (or an unnamed buffer) with undo, the clipboard, saving and reloading. Only one file argument is accepted for now; buffer lists, search, multiple cursors, split view and highlighting come in later milestones.

## Build

```sh
./build.sh           # optimised build, produces ./editor
./build.sh --debug   # unoptimised build with address and undefined behaviour sanitizers
```

Requirements: gcc 15 or newer (for C23 `#embed`), bash, and `wl-clipboard` at runtime for system clipboard support.

The build compiles the vendored tree-sitter sources once into `build/vendor` and only recompiles them when they change. The editor sources are rebuilt every time. The build fails if `data/words.txt` is not sorted in byte order.

## Usage

```sh
editor                       # new empty buffer
editor file                  # open file (created on first save if it does not exist)
editor file1 path/to/file2   # load both and pick one
editor folder                # load every file under folder and pick one
```

Files over 100 MB, files with a nul byte in their first 4 KB, and anything that is not a regular file are not opened. A file is saved with the line ending it was loaded with (LF or CRLF); a file that mixes both is normalised to the more common one. Saving is atomic, follows symbolic links and keeps the file's permissions. If the file changed on disk since it was opened, the editor asks before overwriting it.

Working so far:

- Movement: the arrows, Home (first non-blank, then line start), End, PageUp, PageDown, Ctrl-Left and Ctrl-Right (by word), Ctrl-Home and Ctrl-End, Ctrl-Up and Ctrl-Down (scroll without moving the cursor). Shift with any of them extends the selection. Ctrl-A selects everything, Ctrl-M selects inside the closest brackets (again to include the brackets, again for the next pair out), Alt-M jumps to the matching bracket, Ctrl-L goes to a line or `line:column`, and Esc clears the selection.
- Editing: typing, Enter (keeps the indentation, adds a level after `{`, `[` or `(`, and opens a block between a pair), Tab (spaces up to the next tab stop, or indents every selected line), Shift-Tab (dedents the line or the selected lines), Backspace (deletes back to the previous tab stop in leading spaces), Delete, Ctrl-Backspace (previous word), Ctrl-D (duplicate lines), Ctrl-K (delete lines), Ctrl-Shift-Up and Ctrl-Shift-Down (move lines).
- Undo and redo: Ctrl-Z and Ctrl-Y. Consecutive typing, backspaces or deletes are one step until the cursor moves or another command runs. A paste is one step. Undoing back to the saved text clears `[+]`.
- Clipboard: Ctrl-C, Ctrl-X and Ctrl-V use `wl-copy` and `wl-paste`, and fall back to an in-editor clipboard without a Wayland session. With no selection, copy and cut take the whole line, and pasting that text again inserts it as a line above the cursor. Pasted `\r\n` becomes `\n`.
- Files: Ctrl-S saves (asking for a name when the buffer has none, and before overwriting an existing file or one changed on disk), Alt-R reloads from disk (asking first if there are unsaved changes), Alt-E switches between LF and CRLF, and Ctrl-Q quits (offering to save unsaved changes).
- Display: Alt-W (soft wrap), Alt-L (line numbers), Alt-. (whitespace), Alt-G (ruler), Ctrl-G (ruler column), Alt-T (spaces or tabs for indentation).

The other bindings report that they are not available yet.

Key bindings will be documented here in full as features land. They follow the spec, with one change: project search is Alt-Shift-F rather than Ctrl-Shift-F, because alacritty and ghostty use Ctrl-Shift-F for their own search.

### Ghostty

Ghostty's default key bindings take Shift-Home, Shift-End, Shift-PgUp, Shift-PgDn (scrolling) and Ctrl-Tab (next tab), so the editor never sees them. Foot and alacritty pass them through. To use the editor in ghostty, add this to `~/.config/ghostty/config.ghostty`:

```
keybind = shift+home=unbind
keybind = shift+end=unbind
keybind = shift+page_up=unbind
keybind = shift+page_down=unbind
keybind = ctrl+tab=unbind
```

If changing the ghostty config is not an option, rebind them in `src/keymap.c` instead. Suggested free keys: Alt-Left, Alt-Right, Alt-Up and Alt-Down for the four selection keys, and Ctrl-Space for Ctrl-Tab.
