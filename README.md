# editor

A personal terminal text editor for Linux, written in C23.

It targets Wayland and the foot, alacritty and ghostty terminals, and requires the Kitty Keyboard Protocol. There is no fallback for legacy terminals.

## Build

```sh
./build.sh                         # optimised build, produces ./editor
./build.sh --debug                 # unoptimised build with address and undefined behaviour sanitizers
./build.sh --languages --install   # also builds the language packages and installs them and the word list
```

Requirements: gcc 15 or newer (C23), bash, and `wl-clipboard` at runtime for system clipboard support. Optionally, a language server for the languages that name one (`clangd` for C, see Language servers).

The build compiles the vendored tree-sitter runtime once into `build/vendor` and only recompiles it when it changes. The editor sources are rebuilt every time.

Languages are not built into the editor: each one is a package in `~/.config/editor/languages/<id>/` (or under `$XDG_CONFIG_HOME`), read when the editor starts. `--languages` builds a package for each vendored grammar into `build/languages` (compiling a grammar again only when its sources change), and `--install` copies them to the config folder, replacing packages of the same name and leaving any others alone. Until they are installed, nothing is highlighted, Ctrl-/ does not know any comment markers and there are no symbols. Run `./build.sh --languages --install` again after updating the vendored grammars or queries; an editor already running keeps the packages it started with.

The spell check word list is not built in either: it is `~/.config/editor/words.txt` (or under `$XDG_CONFIG_HOME`), one word per line, and spell check is off until it exists. `--install` copies `data/words.txt` there when there is no list yet, and never replaces one, so words you add are kept. Any list of one word per line works (such as `/usr/share/dict/words`), in any order.

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

## Features

- Movement: the arrows, Home (first non-blank, then line start), End, PageUp, PageDown, Ctrl-Left and Ctrl-Right (by word), Ctrl-Home and Ctrl-End, Alt-Up and Alt-Down (scroll without moving the cursor). Shift with any of them extends the selection. Ctrl-A selects everything, Ctrl-M selects inside the closest brackets (again to include the brackets, again for the next pair out), Alt-M jumps to the matching bracket, Ctrl-L goes to a line or `line:column`. Alt-Right grows every selection to the enclosing syntax node (a word, then the expression, statement, block, function and so on), and Alt-Left takes it back a step at a time, as long as nothing else moved the cursors in between; without syntax highlighting Alt-Right selects inside brackets as Ctrl-M does. Ctrl-Shift-Up and Ctrl-Shift-Down grow every selection by a line above or below, and Alt-Shift-Down and Alt-Shift-Up shrink it by a line at the top or bottom. The top of a selection moves between line starts and the bottom between line ends, so after Ctrl-M inside a function, Alt-Shift-Down and Alt-Shift-Up leave just the lines between the braces. Esc collapses several cursors to one, then clears the selection, then stops highlighting search matches or unknown words.
- Editing: typing, Enter (keeps the indentation, adds a level after `{`, `[` or `(`, and opens a block between a pair), Tab (spaces up to the next tab stop, or indents every selected line), Shift-Tab (dedents the line or the selected lines), Backspace (deletes back to the previous tab stop in leading spaces), Delete, Ctrl-Backspace (previous word), Ctrl-D (duplicate lines), Ctrl-K (delete lines), Ctrl-Up and Ctrl-Down (move lines), Alt-J (join the selected lines, or the line and the next; each line break and the next line's leading blanks become one space, or nothing when either side is empty, the line ends in a blank or the next line starts with a closing bracket), Ctrl-T (trim trailing whitespace from the lines the cursors and selections touch), Ctrl-Shift-T (trim it from every line).
- Case: Alt-Shift-U converts every selection to upper case, and Alt-Shift-L to lower case. They need a selection: without one they say so and change nothing. They use Unicode's simple one to one mappings, so `ß` stays `ß` (not `SS`), capital sigma always becomes `σ` (never the final `ς`), and `I` becomes `i` in every language. Selections stay selected and cursors stay on the same characters, and each conversion is one undo step.
- Sort: Alt-O sorts the lines the selection touches and selects them. It needs a selection: without one it says so and changes nothing (Ctrl-A first sorts the whole buffer). It asks how (`sort: (a)sc (d)esc (u)nique (s)hortest (l)ongest; capital ignores case`): `a` ascending (as `LC_ALL=C sort`), `d` descending (as `sort -r`), `u` unique, ascending with one of each duplicate line kept (as `sort -u`), `s` shortest first or `l` longest first. Length is the width on screen: a tab counts up to its tab stop and a wide character as two columns. Lines of the same length stay in ascending order. The capital letters `A`, `D`, `U`, `S` and `L` do the same ignoring case: they compare simply case folded characters, lines that are then equal go in byte order (reversed by `D`), and `U` keeps the first in byte order of lines that differ only in case. It is one undo step. Cursors of the other view of a split inside the sorted lines move to their start.
- Comments: Ctrl-/ comments out every line touched by a cursor or selection, or uncomments them when they are all commented already. The markers line up at the smallest indentation of each block, and blank lines are left alone. The markers come from the language's package: in the default packages C, Go, JavaScript, PHP and Zig use `//`, Bash and YAML `#`, SQL `--`, CSS `/* */`, and HTML, Markdown and XML `<!-- -->`. JSON has no comments.
- Multiple cursors: Ctrl-E selects the word under the cursor, and each further press adds a cursor at the next occurrence of the selection (matching case, and as a whole word when the selection is one), going round the end of the file. Alt-E skips an occurrence: it moves the newest cursor on to the next one instead of adding a cursor. Ctrl-Shift-E adds a cursor at every occurrence at once. When the selection is a highlighted search match, the occurrences are the search's matches, so after Ctrl-F with the regular expression `Document\b` and Enter, Ctrl-E steps through the matches and Ctrl-Shift-E selects them all; after Alt-C they are the unknown words. Ctrl-Alt-Up and Ctrl-Alt-Down add a cursor on the row above the first cursor or below the last one. Typing, Enter, Tab, Shift-Tab, the deletes, movement and Shift selection act at every cursor, and each edit is one undo step. Cursors that meet merge. Ctrl-D, Ctrl-K, moving lines, joining, sorting, Ctrl-L, Ctrl-M, Alt-M and the search commands first collapse to the newest cursor. With more than one cursor, every cursor is drawn as a block and the status bar shows how many there are.
- Undo and redo: Ctrl-Z and Ctrl-Y. Consecutive typing, backspaces or deletes are one step until the cursor moves or another command runs. A paste is one step. Undoing back to the saved text clears `[+]`.
- Clipboard: Ctrl-C, Ctrl-X and Ctrl-V use `wl-copy` and `wl-paste`, and fall back to an in-editor clipboard without a Wayland session. With no selection, copy and cut take the whole line, and pasting that text again inserts it as a line above the cursor. With several cursors, copy joins the selections (or the cursors' lines) with line breaks, and a paste with as many lines as there are cursors gives each cursor one line. Pasted `\r\n` becomes `\n`.
- Files: Ctrl-S saves (asking for a name when the buffer has none, and before overwriting an existing file or one changed on disk), Alt-R reloads from disk (asking first if there are unsaved changes), Alt-Shift-R loads the files added to the open folder since it was read (by another program; files already found once, including closed ones, are not added again, and nothing else changes), Alt-; switches between LF and CRLF, Alt-Shift-X copies the file's path (as shown in the status bar), and Ctrl-Q quits (offering to save unsaved changes). Switching to a buffer whose file another program changed since it was loaded or saved says so in the message bar (Alt-R reloads it).
- File menu: Alt-X asks for `r` rename, `c` copy or `d` delete. Rename and copy ask for the new path, starting from the current one (Tab completes, `~` is the home folder), and ask before replacing a file that exists. Rename moves the file on disk (copying it, with its permissions, when the new path is on another file system) and keeps unsaved changes unsaved; a new extension changes the language. Copy writes the buffer's text as it is (with its line ending, untrimmed) to the new path and leaves the buffer, its name and `[+]` alone. Delete asks first, then removes the file and closes its buffer. Unnamed buffers can be copied but not renamed or deleted.
- Navigation: Alt-[ goes back to where the cursor was before a jump, and Alt-] forward again. Jumps are Ctrl-L, Ctrl-Home and Ctrl-End, opening a project search match, and the symbol commands below; 32 places are kept, across buffers, and a place in a closed buffer is skipped. Jumping after going back drops the places ahead, as undo does. Alt-S lists the symbols of the file in order (functions, prototypes, types, structs, enums and macros in C, the equivalents in Go, JavaScript, PHP and Zig, Bash functions and Markdown headings), with the symbol named by the word at the cursor selected (so on a call, Enter goes to the function's definition rather than its prototype), or else the one the cursor is in. Each symbol's line number follows it, or in a window at least 80 columns wide a preview beside the list shows the lines around the selected one with its name picked out. Typing filters them, and Enter goes to its name. Ctrl-] and Ctrl-[ go to the start of the next or previous definition. Languages without symbols (CSS, HTML, JSON, SQL, XML, YAML), and files too large to highlight, say so.
- Project symbols: Alt-D goes to the definition of the name at the cursor, in any loaded file. With one definition it goes straight there; with several (such as `static` functions of the same name in several files) it lists them with a preview, the current file's first. On a definition, Alt-D goes to its prototype instead, and a prototype only counts as a definition when there is none. Alt-Shift-D shows the signature of the name at the cursor in the message bar, or of the function whose call the cursor is inside (the name before the innermost `(` left open on the line), preferring one in the current file, then a prototype; long signatures are shortened to fit. Alt-Shift-S lists every symbol of the loaded files by name, with `file:line` beside it, or in a window at least 80 columns wide a preview headed by `file:line`, starting on the name at the cursor. Every preview (these lists, Alt-S, project search and Ctrl-G) names its file and line on its first row. When there are several languages, only the current buffer's count, and Markdown headings are listed by Alt-Shift-S but are never a definition. These use an index of every loaded file's symbols, built the first time one of them is used: progress shows in the message bar, and Esc stops it, keeping what was indexed, so the next use carries on from there. Open buffers are indexed from their text, so a function written but not saved is found. A name that is not found first indexes again the files that changed on disk. Opening a folder with Ctrl-O drops the index.
- Git changes: in a file inside a git repository, the cell before each line number marks how the buffer differs from the file in the last commit: a green `▎` for added lines, a yellow `▎` for changed ones and a red `▔` on the line after lines that were removed (`▁` on the last line for lines removed from the end). The marks follow your edits, and cover changes made by other programs before the file was opened. The status bar counts the lines added, changed and removed (`+3 ~2 -1`). Alt-Shift-] and Alt-Shift-[ go to the next and previous change, going round the end of the file. Alt-G shows a panel on the right half of the window with the change at the cursor: a few lines around it, the old lines in red and the new ones in green; it follows the cursor, takes the place of a split while it shows, and needs a window at least 80 columns wide. Alt-Z restores the change at the cursor (where its mark is) to the last commit, leaving the file's other changes alone, as one undo step. Ctrl-G compares every loaded file with the last commit (open buffers as they are in memory, the others as they are on disk, with progress in the message bar and Esc to stop), lists every change as `file:line`, as project search lists its matches, and sums up the lines they change. Without a preview each change notes its counts (`+1 ~2 -0`). Beside the list, the selected change's file is shown whole with its changes in place, starting a few lines above the selected one: the old lines on red and the new ones on green, each with a bar in the colour of the gutter marks (yellow for changed lines, green for added ones and red for removed ones), under a header with the change's counts and which of the file's changes it is. The preview scrolls on its own with Shift-Up, Shift-Down, Shift-PgUp and Shift-PgDn; Enter opens the file at the selected change. A commit made by another program is noticed when you switch to a buffer, or use Alt-G or Ctrl-G. The comparison is with the last commit (HEAD), not the staged files. The editor reads the repository itself (no `git` program is needed) and only typical ones: a work tree with a `.git` folder, with SHA-1 or SHA-256 objects, loose or packed. Worktrees and submodules (where `.git` is a file), reftable refs and bare repositories show no marks, nor do files the last commit does not have, symbolic links, or buffers over 16 MB. Line numbers must be on (Alt-L) for the marks to show.
- Language servers: a language whose package names a server (the default C package names `clangd`) starts it the first time a file of that language opens, with no command to run. The status bar shows it after the language, `C (clangd)`, with `starting` while it gets ready and `stopped` when it could not start or ended. One server runs for each project root: the first folder above the file holding one of the package's `server-roots` (for C: `compile_commands.json`, `compile_flags.txt`, `.clangd` or `.git`), or else the folder the editor was started in. The server is told about every open file of its language and every edit, collected and sent once per keystroke or paste however many cursors made it, so it never waits for the editor and the editor never waits for it. Unnamed buffers and buffers over 16 MB are not shown to a server. Alt-V turns servers off (stopping them) and on again for the session, and when one has stopped it starts them again. A server's own messages go to `~/.local/state/editor/<language id>.log` (under `$XDG_STATE_HOME` when it is set), which is emptied each time one starts. Opening a folder with Ctrl-O stops them, and they start again as its files open. Quitting asks the servers to exit and waits at most half a second for them.
- Statistics: Alt-I shows the lines, words, characters and bytes of the buffer, or with a selection "selection: N of M" for each. Lines are counted as in the status bar. A word is a run of letters, digits, `_` and non-ASCII characters, the same words that Ctrl-Left and Ctrl-Right move by, so `foo-bar` is two words (unlike `wc -w`). A character is what the cursor steps over (a grapheme cluster, so an emoji sequence is one), and a line break is one character. Bytes count line breaks as the file is saved, so two each with CRLF.
- Display: Alt-W (soft wrap, which also works in a picker and wraps the previews beside it and the Alt-G panel), Alt-L (line numbers), Alt-. (whitespace), Alt-\ (ruler), Alt-Shift-\ (ruler column), Alt-T (spaces or tabs for indentation). Soft wrap breaks rows a column short of the edge, so the cursor at the end of a line always fits on its last row and a line that ends exactly at the edge is not followed by an empty row. Previews have no cursor, so they wrap at the edge.
- Buffers: Ctrl-N (new unnamed buffer), Ctrl-O (open a file, or a folder, which replaces every buffer after offering to save; Tab completes the path, and again steps through the choices; `~` stands for the home folder, here and when saving as), Ctrl-PgDn and Ctrl-PgUp (next and previous open buffer), Ctrl-Tab (the buffer used before this one), Ctrl-P (pick any buffer; a faint `+` before a name marks unsaved changes and `·` any other open buffer, one already read), Ctrl-U (pick a buffer with unsaved changes), Ctrl-Shift-S (save every buffer with unsaved changes, asking for names for unnamed ones), Alt-Q (close the buffer, removing it from the list; with unsaved changes it asks `y` save, `n` discard or `c` cancel, then switches to the buffer used before it, and closing the last buffer leaves a new unnamed one). Closing a buffer shown on both sides of a split, or on the other side, shows the most recently used buffer there instead. Ctrl-Q offers to save every unsaved buffer, and does not quit if a save fails.
- Split view: Ctrl-Alt-Right splits the window into two views of the current buffer, and Ctrl-B splits it with a new unnamed buffer in the new view. Both need a window at least 80 columns wide that is not split already. The new view is on the right and takes focus. Ctrl-J moves focus to the other view, Ctrl-I swaps the two sides, and Ctrl-Alt-Left closes the focused view (its buffer stays loaded). Every other command acts on the focused view, which has the full status bar and the cursor; the other view's status bar is dimmed and shows only its name. Each view has its own cursors and scrolling, even on the same buffer, and edits in one show in the other at once. Display toggles apply to both. Below 80 columns only the focused view is shown, and the split comes back when the window widens. Opening a folder with Ctrl-O ends the split.
- Search: Ctrl-F searches as you type, highlighting every match and selecting the first one after the cursor. Up and Down (or Ctrl-P and Ctrl-N) step between matches, Ctrl-T switches case matching on or off (off at first, and kept for the session), Ctrl-R switches regular expressions on or off (the same way, in every search prompt), Enter keeps the match and Esc puts the cursor and view back. With a selection on one line, the prompt starts with it (escaped to match literally), as do the Ctrl-R and Alt-Shift-F prompts, and the first match Ctrl-F selects is the selection itself. In an empty search prompt, Up or Down brings back the last search. Alt-N and Alt-P select the next and previous match of the last search, wrapping round the ends of the file. Ctrl-W searches for the whole word under the cursor (matching case, and always literally) and selects its next occurrence. The prompt's label shows `[match case]` and `[regex]` while they are on, and `bad pattern` while the typed regular expression is not valid (Enter then says why).
- Replace: Ctrl-R asks for the text to find (Ctrl-T and Ctrl-R work in that prompt as in Ctrl-F) and its replacement, then for each match whether to replace it: `y` yes, `n` no, `a` this and all the rest, `q` (or Esc) stop. It starts at the cursor and goes round the file once. The whole run is one undo step.
- Project search: Alt-Shift-F asks for a pattern (as Ctrl-F does: Ctrl-T matches case, and Up or Down in the empty prompt brings back the last search), then which files to search. Leave that blank for every loaded file, or give space-separated patterns: `*.c *.h` (by name), `src/` (a folder, at any depth), `src/*.c` (a path from the folder the editor was started in), and `!` to leave files out (`*.c !test*`). Only `!` patterns search everything else. The prompt starts with the last scope, so Enter repeats it. Files outside the scope are not read. Open buffers are searched as they are in memory, and the others are read from disk without being opened. Binary and too-large files are skipped. Progress shows in the message bar, and Esc stops the search, keeping what it found. The matching lines are listed as `file:line`, with a preview of the lines around the selected one when the window is at least 80 columns wide (narrower, each line's text follows it in the list). At most 10,000 lines are listed. Type to filter the list, and Enter opens the selected line, selecting the match and highlighting the pattern in that buffer. Alt-F shows the list again, starting after the line opened last, and Esc in the list clears it. Before showing it, Alt-F finds the matches again in every listed file that is open and has been edited, so lines that no longer match drop out and line numbers follow inserted or deleted lines; files that had no match are not searched again (Alt-Shift-F does that). Lines in buffers closed since the search are left out, and the list's title counts them.
- Project replace: Ctrl-R in the list of project search matches replaces the pattern on the lines the list shows, so filter it first to leave lines out. It asks for the replacement as Ctrl-R does (Up or Down in the empty prompt brings back the last one), then "replace in N lines of M files? (y/n)". Every match on those lines is replaced, matching each line again as it is now, so a line edited since the search is safe. Files are opened but not saved: each changed buffer undoes in one step, Ctrl-U lists them and Ctrl-Shift-S saves them. Esc or `n` goes back to the list; after a replace the list is cleared.
- Patterns: search text is literal unless regular expressions are on. `^` at the start ties it to the start of a line and `$` at the end to the end of one. `\t`, `\r`, `\\`, `\^` and `\$` stand for a tab, a carriage return, a backslash, `^` and `$`, in both the pattern and the replacement. `\n` is a line break in a replacement, but a match never crosses lines, so a pattern holding it never matches.
- Regular expressions (Ctrl-R in a search prompt) match within one line and find the leftmost match, preferring alternatives and repetitions in the order Perl does. A match is never empty, so `x*` finds runs of `x` and nothing between them. Ctrl-W and Ctrl-E always search literally.

  | Syntax | Matches |
  |---|---|
  | `.` | any character (one code point) |
  | `[abc]` `[a-z]` `[^...]` | one character from the set, or not in it; `\d`, `\w` and `\s` work inside |
  | `\d` `\w` `\s` | a digit `0`-`9`; a word character (letter, digit, `_` or any non-ASCII character, as Ctrl-W uses); a blank (space, tab, `\r`, `\f`, `\v`) |
  | `\D` `\W` `\S` | anything else |
  | `^` `$` | the start and end of the line |
  | `\b` `\B` | a word boundary, and anywhere else |
  | `*` `+` `?` | 0 or more, 1 or more, 0 or 1 (as many as possible; add `?` for as few as possible) |
  | `{m}` `{m,}` `{m,n}` | m, at least m, m to n times (at most 1000; `?` after makes it lazy) |
  | `(...)` `(?:...)` | a group, captured for the replacement, or not |
  | `a\|b` | either |
  | `\t` `\r` `\n` `\f` `\v` | control characters; `\` before any punctuation matches it literally |

  Without case matching, letters match either case, and so do sets: `[A-Z]` also matches `a`. There are no backreferences, lookarounds, POSIX classes or Unicode properties. Search time grows with the line length times the pattern's size, never exponentially; a pattern too large (over 4096 steps, such as `(a{1000}){5}`) is refused. In the replacement, `\0` is the whole match and `\1` to `\9` are the groups (empty when a group took no part), and `\t`, `\r`, `\n` and `\\` work as in a literal replacement, so `(\w+)=(\w+)` replaced with `\2=\1` swaps the two sides.
- Spell check: Alt-C highlights the words that are not in the word list (`~/.config/editor/words.txt`, see Build), counts them and selects the next one. Alt-N and Alt-P then step through unknown words instead of search matches, and the highlights follow your edits. Words in capitals only, words with digits or `_`, and URLs and email addresses are skipped. A capitalised word is accepted when its lowercase form is in the list. In the programming and data languages (everything highlighted except Markdown) only comments and strings are checked. In Markdown everything is checked except code (fenced and indented blocks and code spans) and link targets. Other files are checked in full. Alt-Shift-C asks whether to add the word at the cursor (or the unknown word Alt-C selected) to the list, or, when the list knows it, whether to remove it (for a capitalised word known through its lowercase form, the lowercase entry). The list is saved straight away, and read again when another program changes it.
- Syntax highlighting: Bash (`.sh`, `.bash`, and `.bashrc`, `.bash_profile`, `.profile` and `PKGBUILD` by name), C (`.c`, `.h`), CSS, Go, HTML (`.html`, `.htm`), JavaScript (`.js`, `.mjs`, `.cjs`), JSON, Markdown (`.md`, `.markdown`), PHP, SQL, XML (`.xml`, `.xsd`, `.xsl`, `.svg`), YAML (`.yaml`, `.yml`) and Zig (`.zig`, `.zon`) are coloured, and the status bar names the language. A file without an extension whose first line is `#!` running `sh` or `bash` (directly or through `env`) is Bash; this is checked when the file opens. Markdown colours headings, emphasis, links and code, and a fenced code block in any of these languages (named by the language or its extension, as in ```` ```js ````, or `shell`). HTML colours its `<script>` and `<style>` contents, and PHP the HTML around its `<?php ?>` blocks (each piece of HTML on its own, so a closing tag whose opening tag is in another piece is not coloured as a tag). Buffers over 16 MB, or that take more than a second to parse, are shown uncoloured, and the status bar says so.
- Pickers and Ctrl-H: type to filter (any part of the text, ignoring case; `^` at the start and `$` at the end tie it to the start or end, as in `^src/` or `.c$`), Up, Down, PgUp, PgDn, Home and End to move, Enter to pick, Esc to cancel. Buffers are listed most recently used first. Ctrl-H lists every binding and runs the one picked.
- Learning mode: Alt-H shows what a shortcut does in the message bar and waits for you to press it: `✓` and the next one when you are right, or `✗` and what the key you pressed does when you are not, and after three misses the answer. Only shortcuts held with Ctrl or Alt are asked, each once in a random order before any comes up again. No key runs its command while it is on; Esc stops it and says how many you got right.

## Key bindings

Ctrl-H lists every binding, filterable, and runs the one picked. The tables below are generated from the same table in `src/keymap.c` by `tools/bindings.py`, so run it after changing a binding.

<!-- bindings start: generated by tools/bindings.py -->

**Movement**

| Key | Action |
|---|---|
| Up | move up |
| Down | move down |
| Left | move left |
| Right | move right |
| Home | move to the first non-blank, then the line start |
| End | move to the line end |
| PgUp | move up a page |
| PgDn | move down a page |
| Ctrl-Left | move to the previous word |
| Ctrl-Right | move to the next word |
| Ctrl-Home | move to the start of the buffer |
| Ctrl-End | move to the end of the buffer |
| Alt-Up | scroll up a line (the cursor stays put) |
| Alt-Down | scroll down a line (the cursor stays put) |

**Selection**

| Key | Action |
|---|---|
| Shift-Up | extend the selection up |
| Shift-Down | extend the selection down |
| Shift-Left | extend the selection left |
| Shift-Right | extend the selection right |
| Shift-Home | extend the selection to the first non-blank, then the line start |
| Shift-End | extend the selection to the line end |
| Shift-PgUp | extend the selection up a page |
| Shift-PgDn | extend the selection down a page |
| Ctrl-Shift-Left | extend the selection to the previous word |
| Ctrl-Shift-Right | extend the selection to the next word |
| Ctrl-Shift-Home | extend the selection to the start of the buffer |
| Ctrl-Shift-End | extend the selection to the end of the buffer |
| Ctrl-A | select the whole buffer |
| Ctrl-M | select inside the closest brackets (again to widen) |
| Alt-Right | expand the selection to the enclosing syntax node (or brackets) |
| Alt-Left | shrink the selection back a step |
| Ctrl-Shift-Up | grow the selection by a line above |
| Ctrl-Shift-Down | grow the selection by a line below |
| Alt-Shift-Down | shrink the selection by a line at the top |
| Alt-Shift-Up | shrink the selection by a line at the bottom |
| Esc | collapse to one cursor, clear the selection, or clear highlights |

**Editing**

| Key | Action |
|---|---|
| Enter | insert a line break |
| Tab | insert a tab (or indent the selected lines) |
| Shift-Tab | dedent the line (or the selected lines) |
| Backspace | delete the character before the cursor |
| Delete | delete the character after the cursor |
| Ctrl-Backspace | delete the word before the cursor |
| Ctrl-Z | undo |
| Ctrl-Y | redo |
| Ctrl-C | copy the selection (or the line) |
| Ctrl-X | cut the selection (or the line) |
| Ctrl-V | paste |
| Ctrl-Up | move the line (or the selected lines) up |
| Ctrl-Down | move the line (or the selected lines) down |
| Ctrl-D | duplicate the line (or the selected lines) |
| Ctrl-K | delete the line (or the selected lines) |
| Ctrl-/ | toggle comments on the line (or the selected lines) |
| Alt-J | join the selected lines (or the line and the next) |
| Alt-O | sort the selected lines |
| Ctrl-T | trim trailing whitespace from the line (or the selected lines) |
| Ctrl-Shift-T | trim trailing whitespace from every line |
| Alt-Shift-U | convert the selection to upper case |
| Alt-Shift-L | convert the selection to lower case |
| Alt-C | spell check the buffer |
| Alt-Shift-C | add the word at the cursor to the word list (or remove it) |

**Multiple cursors**

| Key | Action |
|---|---|
| Ctrl-E | select the word, then add a cursor at the next occurrence (or search match) |
| Alt-E | move the newest cursor to the next occurrence, skipping this one |
| Ctrl-Shift-E | add a cursor at every occurrence |
| Ctrl-Alt-Up | add a cursor on the line above |
| Ctrl-Alt-Down | add a cursor on the line below |

**Buffers and files**

| Key | Action |
|---|---|
| Ctrl-N | create a new empty buffer |
| Ctrl-O | open a file (in a new buffer) or a folder (replacing every buffer) |
| Ctrl-S | save the buffer (asks for a name if it has none) |
| Ctrl-Shift-S | save every buffer with unsaved changes |
| Alt-R | reload the buffer from disk (asks before discarding changes) |
| Alt-Shift-R | load files added to the folder since it was opened |
| Alt-Q | close the buffer (offers to save changes) |
| Alt-X | rename, copy or delete the buffer's file |
| Alt-Shift-X | copy the path of the buffer's file |
| Alt-; | toggle line endings between LF and CRLF |
| Alt-I | count the lines, words, characters and bytes (of the selection) |
| Ctrl-PgDn | switch to the next buffer |
| Ctrl-PgUp | switch to the previous buffer |
| Ctrl-Tab | switch to the buffer used before this one |
| Ctrl-P | pick a buffer |
| Ctrl-U | pick a buffer with unsaved changes |
| Ctrl-Q | quit (offers to save changes) |

**Split view**

| Key | Action |
|---|---|
| Ctrl-Alt-Right | split the window, showing this buffer in both views |
| Ctrl-B | split the window, with a new empty buffer in the new view |
| Ctrl-Alt-Left | close this view |
| Ctrl-J | move focus to the other view |
| Ctrl-I | swap the two views |

**Search**

| Key | Action |
|---|---|
| Ctrl-F | search the buffer |
| Alt-N | go to the next match |
| Alt-P | go to the previous match |
| Ctrl-W | go to the next occurrence of the word at the cursor |
| Ctrl-R | search and replace in the buffer |
| Alt-Shift-F | search the loaded files |
| Alt-F | reopen the list of matches in the loaded files |
| Ctrl-L | go to a line number |

**Navigation**

| Key | Action |
|---|---|
| Alt-[ | go back to where the cursor was before a jump |
| Alt-] | go forward again after going back |
| Ctrl-[ | go to the previous definition |
| Ctrl-] | go to the next definition |
| Alt-M | go to the matching bracket |
| Alt-S | pick a symbol in this buffer |
| Alt-Shift-S | pick a symbol in any loaded file |
| Alt-D | go to the definition of the name at the cursor |
| Alt-Shift-D | show the signature of the name (or call) at the cursor |

**Changes since the last git commit**

| Key | Action |
|---|---|
| Alt-Shift-] | go to the next change |
| Alt-Shift-[ | go to the previous change |
| Alt-G | show or hide the change at the cursor beside the buffer |
| Alt-Z | revert the change at the cursor to the last commit |
| Ctrl-G | pick a change since the last commit |

**Display and settings**

| Key | Action |
|---|---|
| Alt-T | toggle indenting with spaces or tabs |
| Alt-W | toggle soft wrap |
| Alt-L | toggle line numbers |
| Alt-. | toggle showing whitespace |
| Alt-\ | toggle the vertical ruler |
| Alt-V | toggle language servers (or restart stopped ones) |
| Alt-Shift-\ | set the ruler column (and show the ruler) |
| Ctrl-H | show this list of shortcuts |
| Alt-H | practise the shortcuts (esc to stop) |

<!-- bindings end -->

## Terminals

The editor needs the kitty keyboard protocol, so that keys like Ctrl-M, Ctrl-H, Ctrl-I, Ctrl-J, Ctrl-[ and Ctrl-Shift-S reach it as themselves. At startup it asks the terminal whether the protocol is there and exits with an error if it is not (as in the Linux console, or inside tmux). It works in foot, alacritty and ghostty with their default configurations, except for the keys below.

As mentioned above, project search is **Alt-Shift-F** rather than Ctrl-Shift-F, because alacritty and ghostty use Ctrl-Shift-F for their own search and never pass it on. Alt-Shift-F pairs with Alt-F, which reopens the results.

### Ghostty

Ghostty's default key bindings take Shift-Home, Shift-End, Shift-PgUp, Shift-PgDn (scrolling), Ctrl-Tab (next tab), Ctrl-Shift-T (new tab) and Ctrl-Shift-E (new split), so the editor never sees them: selection extension to the line start, line end and by page, switching to the buffer used before, trimming every line and adding a cursor at every occurrence. Foot and alacritty pass them through. To use the editor in ghostty, add this to `~/.config/ghostty/config.ghostty`:

```
keybind = shift+home=unbind
keybind = shift+end=unbind
keybind = shift+page_up=unbind
keybind = shift+page_down=unbind
keybind = ctrl+tab=unbind
keybind = ctrl+shift+t=unbind
keybind = ctrl+shift+e=unbind
```

If changing the ghostty config is not an option, rebind them in `src/keymap.c` instead. Suggested free keys: Alt-Shift-A, Alt-Shift-B, Alt-Shift-O and Alt-Shift-P for the four selection keys, Ctrl-Space for Ctrl-Tab, Alt-Shift-T for Ctrl-Shift-T and Alt-Shift-E for Ctrl-Shift-E. Ctrl-Shift-Up and Ctrl-Shift-Down do reach the editor in ghostty. Ghostty also binds Ctrl-PgUp, Ctrl-PgDn, Ctrl-Shift-Left, Ctrl-Shift-Right and the Ctrl-Alt arrows to its tabs and splits. They are not unbound above because ghostty tabs and splits are not used; if ghostty takes one of them, unbind it the same way.

## Adding a language

A language is a folder in `~/.config/editor/languages/` (or `$XDG_CONFIG_HOME/editor/languages/`), named by its id:

```
languages/go/
  language         # what the language is (below); the only file required
  parser.so        # the tree-sitter grammar, for highlighting and symbols
  highlights.scm   # required with parser.so
  injections.scm   # optional: languages embedded in this one
  tags.scm         # optional: symbols (Alt-S, Ctrl-], Ctrl-[, Alt-D, Alt-Shift-D, Alt-Shift-S)
```

`language` holds one `key = value` per line. A line starting with `#` is a comment (there are no comments after a value, since `#` can be a comment marker):

| Key | Value |
| --- | --- |
| `name` | The display name, shown in the status bar. Required with `extensions`, `files` or `interpreters`, and left out for a grammar that is only embedded in others (`markdown-inline`). |
| `extensions` | Extensions without the dot, separated by spaces, matched ignoring case. |
| `files` | Whole file names for files without an extension, matching case (`PKGBUILD .bashrc`). |
| `interpreters` | Programs a `#!` first line may name, directly or through `env`, for files without an extension (`sh bash`). |
| `comment` | What Ctrl-/ puts before a line (`//`). Left out, the language has no comments. |
| `comment-end` | What it puts after a line, for block comments (`-->`). |
| `inject` | The names other grammars embed it by, such as `bash shell`. The default is the id. Markdown fences also find a language by its extensions. |
| `symbol` | The function of `parser.so` that gives the grammar. The default is `tree_sitter_` and the id with `-` as `_`. |
| `prose` | `yes` to spell check all text outside code, as in Markdown, rather than only comments and strings. |
| `fresh-parse` | `yes` to parse from scratch after every edit, for a grammar whose scanner gives different trees when parsing incrementally (Markdown's inline grammar). |
| `first-pattern-wins` | `yes` when the highlight query puts its specific patterns first (Go's does): for captures of the same node, the earlier pattern wins rather than the later one. |
| `server` | The language server to start for files of the language: the program (looked up in `PATH`) and its arguments, separated by blanks, with no quoting (`clangd --background-index`). It must speak the Language Server Protocol over its standard input and output. |
| `server-roots` | Names of files or folders that mark a project's root for the server, separated by blanks: the first folder above a file holding one of them is the root. The default is `.git`. Needs `server`. |

Without `parser.so` a language still has its files and comment markers, just no colours or symbols. A package whose `language` file is wrong is left out, and the problem is shown when the editor starts. A `parser.so` that does not load (a missing entry point, or a tree-sitter ABI other than 13 to 15) or a query that does not compile is reported when a file of that language first opens, and that language is left uncoloured. Packages are read only at startup, so a new or changed package takes effect the next time the editor starts. Removing a language is deleting its folder.

To make a package from a grammar repository:

```sh
tools/language.sh ~/src/tree-sitter-lua lua
```

This compiles the repository's `src/parser.c` (and `src/scanner.c`, if it has one) into `parser.so`, copies its `queries/highlights.scm` and `queries/injections.scm`, and writes a `language` file to correct unless the package already has one. For a repository holding several grammars, name the folder of the one wanted (`tree-sitter-php/php`). Repositories that do not commit `src/parser.c` need `tree-sitter generate` first. Then check that the query's capture names map to theme groups (see ARCHITECTURE.md, "Colouring lines"), that any predicates it uses are among those the editor supports (an unknown one is ignored and lets its pattern match everything), and which pattern it expects to win when two capture the same node (most put general patterns first, the default). For symbols, write `tags.scm` to the contract in ARCHITECTURE.md ("Symbols"): adapt upstream's `tags.scm` if it has one, so that `@definition.<kind>` captures the whole declaration.

A `parser.so` runs inside the editor with your privileges, so only install grammars you would run as a program. Packages are only ever read from the config folder, never from a project being edited. The same goes for a `server`: it is a program the editor runs, so a project can never choose one.

### Language servers

A server only needs `server` (and, when `.git` does not mark the projects' roots, `server-roots`) in the package's `language` file; the rest comes from the server itself. How the server builds the project is its own configuration, kept with the project and shared with other tools, not the editor's. For `clangd`, that is one of:

- `compile_commands.json`, written by the build system (CMake's `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, or `bear -- make`), which gives every file its exact flags.
- `compile_flags.txt`, one flag per line, used for every file: `-std=c23`, `-Iinclude`, `-Wall`.
- `.clangd`, YAML that adds or removes flags (`CompileFlags: Add: [-std=c23, -Iinclude]`) and says which diagnostics to show (`Diagnostics: Suppress: [unused-parameter]`), with `If: PathMatch:` blocks for parts of the tree. This repository's own `.clangd` is an example.

`--background-index` makes clangd index the whole project in the background, kept in `.cache/clangd` at the root, so leave `.cache/` out with `.editor` and `.gitignore`.

To add a language to the default packages instead, put its generated `src/parser.c` and `src/scanner.c` in `vendor/grammars/<id>/` with the grammar's `src/tree_sitter/` headers and any other files its scanner includes (when the scanner includes a header by a relative path outside `src/`, as PHP's and XML's do, keep upstream's layout), its queries in `vendor/queries/<id>/`, and the editor's own files (`language`, and `tags.scm` when it has symbols) in `languages/<id>/`. `./build.sh --languages` builds every folder in `vendor/grammars`.
