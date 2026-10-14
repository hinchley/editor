# editor

A personal terminal text editor for Linux, written in C23.

It targets Wayland and the foot, alacritty and ghostty terminals, and requires the Kitty Keyboard Protocol. There is no fallback for legacy terminals.

> Status: early development (milestone M10, project search). The editor loads files and folders, switches between buffers, edits with any number of cursors, undo and the clipboard, shows two views side by side, highlights syntax, searches a buffer or the whole project, replaces, spell checks and saves. Polish and performance come in the last milestone.

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
editor file1 folder1 ...     # any mix of files and folders
```

Folders are walked at any depth. `.git` folders and links to folders are skipped, and links to files are followed. A file reached twice (named again, inside a named folder, or through a link) is loaded once. Loaded files are only read when first used. More than 100,000 files is refused before the editor starts.

A `.editor` file in a folder leaves files out of that folder and everything below it, until a deeper `.editor` file replaces it. One pattern per line, and `#` starts a comment:

```
foo.txt       # files with this name at any depth
*.c           # files ending in .c at any depth
bar/          # folders with this name (and everything in them) at any depth
/foo/foo.txt  # this path, relative to the .editor file
/*.c          # .c files next to the .editor file
/bar/         # this folder, relative to the .editor file
```

`*` is the only wildcard and stays within one name. Files named on the command line are always loaded. `.editor` files are read when loading (at startup and when Ctrl-O opens a folder), not when they change.

Files over 100 MB, files with a nul byte in their first 4 KB, and anything that is not a regular file are not opened. They stay in the buffer list, marked in the pickers. A file is saved with the line ending it was loaded with (LF or CRLF); a file that mixes both is normalised to the more common one. Saving is atomic, follows symbolic links and keeps the file's permissions. If the file changed on disk since it was opened, the editor asks before overwriting it.

Working so far:

- Movement: the arrows, Home (first non-blank, then line start), End, PageUp, PageDown, Ctrl-Left and Ctrl-Right (by word), Ctrl-Home and Ctrl-End, Ctrl-Up and Ctrl-Down (scroll without moving the cursor). Shift with any of them extends the selection. Ctrl-A selects everything, Ctrl-M selects inside the closest brackets (again to include the brackets, again for the next pair out), Alt-M jumps to the matching bracket, Ctrl-L goes to a line or `line:column`. Esc collapses several cursors to one, then clears the selection, then stops highlighting search matches or unknown words.
- Editing: typing, Enter (keeps the indentation, adds a level after `{`, `[` or `(`, and opens a block between a pair), Tab (spaces up to the next tab stop, or indents every selected line), Shift-Tab (dedents the line or the selected lines), Backspace (deletes back to the previous tab stop in leading spaces), Delete, Ctrl-Backspace (previous word), Ctrl-D (duplicate lines), Ctrl-K (delete lines), Ctrl-Shift-Up and Ctrl-Shift-Down (move lines).
- Comments: Ctrl-/ comments out every line touched by a cursor or selection, or uncomments them when they are all commented already. The markers line up at the smallest indentation of each block, and blank lines are left alone. C and JavaScript use `//`, CSS `/* */`, and HTML and Markdown `<!-- -->`. JSON has no comments.
- Multiple cursors: Ctrl-E selects the word under the cursor, and each further press adds a cursor at the next occurrence of the selection (matching case, and as a whole word when the selection is one), going round the end of the file. Ctrl-Alt-Up and Ctrl-Alt-Down add a cursor on the row above the first cursor or below the last one. Typing, Enter, Tab, Shift-Tab, the deletes, movement and Shift selection act at every cursor, and each edit is one undo step. Cursors that meet merge. Ctrl-D, Ctrl-K, moving lines, Ctrl-L, Ctrl-M, Alt-M and the search commands first collapse to the newest cursor. With more than one cursor, every cursor is drawn as a block and the status bar shows how many there are.
- Undo and redo: Ctrl-Z and Ctrl-Y. Consecutive typing, backspaces or deletes are one step until the cursor moves or another command runs. A paste is one step. Undoing back to the saved text clears `[+]`.
- Clipboard: Ctrl-C, Ctrl-X and Ctrl-V use `wl-copy` and `wl-paste`, and fall back to an in-editor clipboard without a Wayland session. With no selection, copy and cut take the whole line, and pasting that text again inserts it as a line above the cursor. With several cursors, copy joins the selections (or the cursors' lines) with line breaks, and a paste with as many lines as there are cursors gives each cursor one line. Pasted `\r\n` becomes `\n`.
- Files: Ctrl-S saves (asking for a name when the buffer has none, and before overwriting an existing file or one changed on disk), Alt-R reloads from disk (asking first if there are unsaved changes), Alt-E switches between LF and CRLF, and Ctrl-Q quits (offering to save unsaved changes).
- Display: Alt-W (soft wrap), Alt-L (line numbers), Alt-. (whitespace), Alt-G (ruler), Ctrl-G (ruler column), Alt-T (spaces or tabs for indentation).
- Buffers: Ctrl-N (new unnamed buffer), Ctrl-O (open a file, or a folder, which replaces every buffer after offering to save; Tab completes the path, and again steps through the choices; `~` stands for the home folder, here and when saving as), Ctrl-PgDn and Ctrl-PgUp (next and previous open buffer), Ctrl-Tab (the buffer used before this one), Ctrl-P (pick any buffer), Ctrl-U (pick a buffer with unsaved changes), Ctrl-Shift-S (save every buffer with unsaved changes, asking for names for unnamed ones). Ctrl-Q offers to save every unsaved buffer, and does not quit if a save fails.
- Split view: Ctrl-Alt-Right splits the window into two views of the current buffer, and Ctrl-B splits it with a new unnamed buffer in the new view. Both need a window at least 80 columns wide that is not split already. The new view is on the right and takes focus. Ctrl-J moves focus to the other view, Ctrl-T swaps the two sides, and Ctrl-H closes the focused view (its buffer stays loaded). Every other command acts on the focused view, which has the full status bar and the cursor; the other view's status bar is dimmed and shows only its name. Each view has its own cursors and scrolling, even on the same buffer, and edits in one show in the other at once. Display toggles apply to both. Below 80 columns only the focused view is shown, and the split comes back when the window widens. Opening a folder with Ctrl-O ends the split.
- Search: Ctrl-F searches as you type, highlighting every match and selecting the first one after the cursor. Up and Down (or Ctrl-P and Ctrl-N) step between matches, Ctrl-T switches case matching on or off (off at first, and kept for the session), Enter keeps the match and Esc puts the cursor and view back. In an empty search prompt, Up or Down brings back the last search. Alt-N and Alt-P select the next and previous match of the last search, wrapping round the ends of the file. Ctrl-W searches for the whole word under the cursor (matching case) and selects its next occurrence.
- Replace: Ctrl-R asks for the text to find and its replacement, then for each match whether to replace it: `y` yes, `n` no, `a` this and all the rest, `q` (or Esc) stop. It starts at the cursor and goes round the file once. The whole run is one undo step.
- Project search: Alt-Shift-F asks for a pattern (as Ctrl-F does: Ctrl-T matches case, and Up or Down in the empty prompt brings back the last search), then which files to search. Leave that blank for every loaded file, or give space-separated patterns: `*.c *.h` (by name), `src/` (a folder, at any depth), `src/*.c` (a path from the folder the editor was started in), and `!` to leave files out (`*.c !test*`). Only `!` patterns search everything else. The prompt starts with the last scope, so Enter repeats it. Files outside the scope are not read. Open buffers are searched as they are in memory, and the others are read from disk without being opened. Binary and too-large files are skipped. Progress shows in the message bar, and Esc stops the search, keeping what it found. The matching lines are listed as `file:line`, with a preview of the lines around the selected one when the window is at least 80 columns wide (narrower, each line's text follows it in the list). At most 10,000 lines are listed. Type to filter the list, and Enter opens the selected line, selecting the match and highlighting the pattern in that buffer. Alt-F shows the list again, starting after the line opened last, and Esc in the list clears it.
- Patterns: search text is literal. `^` at the start ties it to the start of a line and `$` at the end to the end of one. `\t`, `\r`, `\\`, `\^` and `\$` stand for a tab, a carriage return, a backslash, `^` and `$`, in both the pattern and the replacement. `\n` is a line break in a replacement, but a match never crosses lines, so a pattern holding it never matches.
- Spell check: Alt-S highlights the words that are not in the word list, counts them and selects the next one. Alt-N and Alt-P then step through unknown words instead of search matches, and the highlights follow your edits. Words in capitals only, words with digits or `_`, and URLs and email addresses are skipped. A capitalised word is accepted when its lowercase form is in the list. In C, CSS, HTML, JavaScript and JSON only comments and strings are checked. In Markdown everything is checked except code (fenced and indented blocks and code spans) and link targets. Other files are checked in full.
- Syntax highlighting: C (`.c`, `.h`), CSS, HTML (`.html`, `.htm`), JavaScript (`.js`, `.mjs`, `.cjs`), JSON and Markdown (`.md`, `.markdown`) are coloured, and the status bar names the language. Markdown colours headings, emphasis, links and code, and a fenced code block in any of these languages (named by the language or its extension, as in ```` ```js ````). HTML colours its `<script>` and `<style>` contents. Buffers over 16 MB, or that take more than a second to parse, are shown uncoloured, and the status bar says so.
- Pickers and Alt-H: type to filter (any part of the text, ignoring case; `^` at the start and `$` at the end tie it to the start or end, as in `^src/` or `.c$`), Up, Down, PgUp, PgDn, Home and End to move, Enter to pick, Esc to cancel. Buffers are listed most recently used first. Alt-H lists every binding and runs the one picked.

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

## Adding a language

1. Get the grammar's generated `src/parser.c` (and `src/scanner.c` if it has one) from the grammar repository, or run `tree-sitter generate` in it, and put them in `vendor/grammars/<lang>/` with the grammar's `src/tree_sitter/` headers. Copy its `queries/highlights.scm` (and `queries/injections.scm`, if it embeds other languages) to `vendor/queries/<lang>/`. `build.sh` compiles every folder under `vendor/grammars/`.
2. In `src/highlight.c`, declare its `tree_sitter_<lang>()` entry point beside the others, add a `u8` array that `#embed`s each query file, and add a row to `grammars[]`: the display name, the injection id (the lowercase name other grammars inject it by, such as `c`), the entry point and the queries. Set `prose` for a language whose text should be spell checked outside code (like Markdown).
3. In `src/lang.c`, add a row to `langTable`: the display name (the same as in its `grammars[]` row), its extensions, and its comment start and end (end empty for line comments, start `nullptr` for a language without comments).

A Markdown code block can name the new language by its injection id or by any of its extensions straight away.
