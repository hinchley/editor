# editor

A personal terminal text editor for Linux, written in C23.
It runs in the [foot](https://codeberg.org/dnkl/foot) terminal on Wayland, and nowhere else (see Foot).

## Quick start

```sh
./build.sh --languages --install   # build the editor, and install the language packages and word list
./editor .                         # load every file under the current folder and pick one
```

Ctrl-H lists every key and runs the one picked, Alt-H practises them, and Ctrl-Q quits.

## The keys used most

| Key                      | Action                                                    |
| ------------------------ | --------------------------------------------------------- |
| Ctrl-S / Ctrl-Shift-S    | save the buffer / save every buffer                       |
| Ctrl-Q                   | quit, offering to save                                    |
| Ctrl-Z / Ctrl-Y          | undo / redo                                               |
| Ctrl-C / Ctrl-X / Ctrl-V | copy / cut / paste (the line, with no selection)          |
| Ctrl-F                   | search as you type                                        |
| Alt-N / Alt-P            | next / previous match                                     |
| Ctrl-R                   | search and replace                                        |
| Ctrl-Shift-F             | search every loaded file                                  |
| Ctrl-P                   | pick a buffer                                             |
| Ctrl-O                   | open a file or folder                                     |
| Ctrl-E                   | select the word, then add a cursor at its next occurrence |
| Ctrl-/                   | comment or uncomment the lines                            |
| Ctrl-D / Ctrl-K          | duplicate / delete the lines                              |
| Alt-[ / Alt-]            | back / forward through jumps                              |
| Alt-D                    | go to the definition of the name at the cursor            |
| Ctrl-L                   | go to a line                                              |
| Esc                      | one cursor, then no selection, then no highlights         |
| Ctrl-H                   | every key, filterable                                     |

## Build

```sh
./build.sh                         # optimised build, produces ./editor
./build.sh --debug                 # unoptimised build with address and undefined behaviour sanitizers
./build.sh --languages --install   # also builds the language packages and installs them and the word list
```

Requirements: gcc 15 or newer (C23) and bash, and at runtime foot and `wl-clipboard` for the system clipboard.
Optionally, a language server for the languages that name one (`clangd` for C, see Language servers).

The build compiles the vendored tree-sitter runtime once into `build/vendor` and only recompiles it when it changes.
The editor sources are rebuilt every time.
It also writes `compile_commands.json`, the flags each source is compiled with, for clangd.

Languages are not built into the editor: each one is a package in `~/.config/editor/languages/<id>/` (or under `$XDG_CONFIG_HOME`), read when the editor starts.
`--languages` builds a package for each vendored grammar into `build/languages`, and `--install` copies them to the config folder, replacing packages of the same name and leaving any others alone.
Until they are installed, nothing is highlighted, Ctrl-/ knows no comment markers and there are no symbols.
Run `./build.sh --languages --install` again after updating the vendored grammars or queries; an editor already running keeps the packages it started with.

The spell check word list is `~/.config/editor/words.txt` (or under `$XDG_CONFIG_HOME`), one word per line, and spell check is off until it exists.
`--install` copies `data/words.txt` there when there is no list yet, and never replaces one, so words you add are kept.
Any list of one word per line works, such as `/usr/share/dict/words`.

## Usage

```sh
editor                       # new empty buffer
editor file                  # open file (created on first save if it does not exist)
editor file:120[:8]          # open file at line 120 (and column 8), as compilers print it
editor file1 path/to/file2   # load both and pick one
editor folder                # load every file under folder and pick one (Escape: empty buffer)
editor file1 folder1 ...     # any mix of files and folders
```

A `path:line[:column]` is split only when the whole name does not exist but the path does, so a file with colons in its name still opens as itself.

Folders are walked at any depth.
`.git` folders, links to folders and the temporary files a save cut short leaves behind (`.<name>.editor-XXXXXX`) are skipped, and links to files are followed.
A file reached twice is loaded once.
Loaded files are only read when first used.
More than 100,000 files is refused before the editor starts.

A `.editor` file in a folder leaves files out of that folder and everything below it, until a deeper `.editor` file replaces it.
One pattern per line, and `#` starts a comment:

```
foo.txt       # files with this name at any depth
*.c           # files ending in .c at any depth
bar/          # folders with this name (and everything in them) at any depth
/foo/foo.txt  # this path, relative to the .editor file
/*.c          # .c files next to the .editor file
/bar/         # this folder, relative to the .editor file
```

`*` is the only wildcard and stays within one name.
Files named on the command line are always loaded.
`.editor` files are read when loading (at startup and when Ctrl-O opens a folder), not when they change.

Files over 100 MB, files with a nul byte in their first 4 KB (other than pictures, see Images), and anything that is not a regular file are not opened; they stay in the buffer list, marked in the pickers.
A file is saved with the line ending it was loaded with (LF or CRLF); a file that mixes both is normalised to the more common one.
Saving is atomic, follows symbolic links and keeps the file's permissions, and the editor asks before overwriting a file that changed on disk since it was opened.

## Features

The binding tables below list every key; this section describes what they do.

### Moving and selecting

The arrows, Home (first non-blank, then line start), End, PageUp, PageDown, Ctrl-Left and Ctrl-Right (by word), and Ctrl-Home and Ctrl-End move, and Shift with any of them extends the selection.
Alt-Up and Alt-Down scroll a row, as the wheel does: the cursors stay put until they would reach the scroll margin, and are then carried along.

Ctrl-A selects everything.
Ctrl-M selects inside the closest brackets, again to include the brackets, and again for the next pair out.
Alt-M jumps to the matching bracket, and Ctrl-L goes to a line or `line:column`.
Alt-Right grows every selection to the enclosing syntax node (a word, then the expression, statement, block, function), and Alt-Left takes it back a step, as long as nothing else moved the cursors; without highlighting Alt-Right selects inside brackets.
Ctrl-Shift-Up and Ctrl-Shift-Down grow every selection by a line, and Alt-Shift-Down and Alt-Shift-Up shrink it by a line at the top or bottom, so after Ctrl-M inside a function they leave just the lines between the braces.
Esc collapses several cursors to one, then clears the selection, then stops highlighting matches.

### Mouse

A click puts the cursor on the text clicked, and on the other side of a split moves the focus there first.
Dragging selects; a double click selects a word and a triple click a line, and dragging after either grows the selection a word or a line at a time.
A click on the line numbers selects that line.
Dragging past the top or bottom of the view scrolls it while the pointer keeps moving.
Alt-click adds a cursor, or removes the one clicked; Alt-drag adds one with a selection; Ctrl-Alt-drag selects a column.
The wheel scrolls the side under the pointer by three rows, carrying the cursors along at the margin.
In a list, a click selects an item and a second click picks it.
Holding Shift makes foot's own selection instead.

### Editing

Enter keeps the indentation, adds a level after `{`, `[` or `(`, and opens a block between a pair; a `}`, `]` or `)` typed at the end of a line holding only blanks takes a level off it.
An opening bracket or quote typed over a selection puts the pair around it.
Tab inserts spaces to the next tab stop or indents the selected lines, Shift-Tab dedents, and Backspace in leading spaces deletes back to the previous tab stop.
Ctrl-Enter and Ctrl-Shift-Enter open a line below or above, wherever the cursor is on its line.

Line commands act on the line or every line the selection touches: Ctrl-D duplicates, Ctrl-K deletes, Ctrl-Up and Ctrl-Down move, Alt-J joins (each line break and the next line's leading blanks become one space), Ctrl-/ comments or uncomments (with the markers lined up), and Ctrl-T trims trailing whitespace (Ctrl-Shift-T every line).
Alt-Shift-U and Alt-Shift-L convert a selection to upper or lower case, with Unicode's simple mappings (`ß` stays `ß`).

Alt-O sorts the selected lines, asking how: `a` ascending, `d` descending, `u` unique, `s` shortest or `l` longest first (as shown on screen), and the capitals do the same ignoring case.

Alt-Shift-G aligns the Markdown table at the cursor, asking for the widest a column may grow (50 at first); wider body cells wrap onto lines added below their row.
It finds tables through the syntax tree, so it needs Markdown highlighting.

Ctrl-Z and Ctrl-Y undo and redo.
Consecutive typing, backspaces or deletes are one step until the cursor moves or another command runs, and a paste is one step.
Undoing back to the saved text clears `[+]`.

Ctrl-C, Ctrl-X and Ctrl-V use `wl-copy` and `wl-paste`.
With no selection, copy and cut take the whole line, and pasting that text again inserts it as a line above the cursor.
In a prompt, Ctrl-V pastes the clipboard's first line.
Without a Wayland session or `wl-paste`, the editor keeps its own clipboard and says so.

### Multiple cursors

Ctrl-E selects the word under the cursor, and each further press adds a cursor at the next occurrence (matching case, as a whole word when the selection is one).
Alt-E skips an occurrence, moving the newest cursor on, and Ctrl-Shift-E adds a cursor at every occurrence.
When the selection is a highlighted search match, the occurrences are the search's matches, so a regular expression search and Ctrl-Shift-E selects every match.
Ctrl-Alt-Up and Ctrl-Alt-Down add a cursor above the first or below the last.

Typing, the deletes, movement and selection act at every cursor, and each edit is one undo step.
Ctrl-D and Ctrl-Up/Down act on the lines of every cursor; Ctrl-K, joining, sorting, Ctrl-L and the search commands first collapse to the newest cursor.
A paste with as many lines as there are cursors gives each cursor one line.
Alt-= lines the cursors up in a column with spaces, so with a cursor before each field name of a struct the names line up; with several cursors on a line, each rank lines up in turn.
The status bar counts the cursors (`3 cursors (esc)`).

### Files and buffers

Ctrl-S saves, asking for a name when the buffer has none.
Alt-R reloads from disk, asking first if there are unsaved changes.
When the window regains focus or you switch to a buffer, a file another program changed is reloaded if its buffer has no unsaved changes, and noted otherwise.
Alt-; switches between LF and CRLF, and Alt-Shift-X copies the file's path.

Ctrl-N makes a new buffer and Ctrl-O opens a file or folder (Tab completes, `~` is the home folder).
A folder asks whether to add its files to the buffers or replace them.
Alt-Shift-R loads files added to the open folders since they were read.
Ctrl-PgDn and Ctrl-PgUp step through the open buffers and the files named on their own, Ctrl-Tab goes to the buffer used before, Ctrl-P picks any buffer (a faint `+` marks unsaved changes and `·` other open buffers), and Ctrl-U picks one with unsaved changes.
Alt-Q closes the buffer and Alt-Shift-Q a folder with all its buffers, asking `s` save, `d` discard or `c` cancel when there are unsaved changes.
Ctrl-Shift-S saves every buffer with changes, and Ctrl-Q offers to before quitting.

Buffers are named by folder: a file inside an open folder shows as the folder's name and its path inside it (`editor/src/app.c`), with parent folders added to tell apart two folders of the same name.
Any other file shows its path from the folder the editor was started in, or its whole path with `~`.

Alt-X asks `r` rename, `c` copy or `d` delete.
Rename moves the file on disk (copying it across file systems) and keeps unsaved changes; a new extension changes the language.
Copy writes the buffer's text to a new path and leaves the buffer alone.
Delete removes the file and closes its buffer.

Alt-I counts the lines, words, characters and bytes of the buffer or the selection.
Lines are counted as `wc -l` does, words as Ctrl-Right moves by (so `foo-bar` is two), and characters as the cursor steps.

### Recovery

Unsaved changes are copied to `~/.local/state/editor/recover` (under `$XDG_STATE_HOME` when it is set) once typing stops for 2 seconds, and at once when the window closes or the editor gets SIGHUP or SIGTERM.
A crash or SIGKILL loses at most the last 2 seconds of typing; a buffer over 16 MB is only copied when the window closes.
Each copy is named after the file's full path with `/` as `%` (or `unnamed-N`), then `~` and the editor's process id.
Saving, closing or discarding a buffer, or quitting, removes its copy, so the folder only holds lost work, and the next start says how many copies are there.
Open them with Ctrl-O, save what you want and delete the rest.

### Split view

Ctrl-Alt-Right splits the window into two views of the buffer, and Ctrl-B splits it with a new buffer; both need a window at least 80 columns wide.
Ctrl-J moves the focus, Ctrl-I swaps the sides and Ctrl-Alt-Left closes the focused view.
Each view has its own cursors and scrolling, even on the same buffer, and edits show in both at once.
Below 80 columns only the focused view is shown, and the split comes back when the window widens.

### Search and replace

Ctrl-F searches as you type, highlighting every match and selecting the first after the cursor.
Up and Down step between matches, Enter keeps the match and Esc puts the cursor back.
Case is smart: a pattern with an upper case letter matches case.
Ctrl-T asks for case always and Ctrl-R switches regular expressions on, in every search prompt, for the session; the prompt's label shows `[match case]`, `[regex]` and which match is selected of how many (`[3/17]`, in buffers up to 4 MB).
With a selection on one line, the prompt starts with it.
In an empty prompt, Up and Down bring back the last 20 searches.
Alt-N and Alt-P go to the next and previous match, and Ctrl-W to the next occurrence of the word at the cursor.

Ctrl-R asks for a pattern and a replacement, then for each match `y` yes, `n` no, `a` all the rest or `q` stop.
It goes round the file once from the cursor, or replaces only inside a selection of more than one line, and the whole run is one undo step.

Patterns are literal unless regular expressions are on.
`^` at the start and `$` at the end tie them to a line's ends, and `\t`, `\r`, `\\`, `\^` and `\$` stand for those characters; `\n` is a line break in a replacement, but a match never crosses lines.
Regular expressions find the leftmost match, preferring alternatives and repetitions as Perl does, and a match is never empty:

| Syntax                      | Matches                                                                                    |
| --------------------------- | ------------------------------------------------------------------------------------------ |
| `.`                         | any character (one code point)                                                             |
| `[abc]` `[a-z]` `[^...]`    | one character from the set, or not in it; `\d`, `\w` and `\s` work inside                  |
| `\d` `\w` `\s`              | a digit `0`-`9`; a word character (letter, digit, `_` or any non-ASCII character); a blank |
| `\D` `\W` `\S`              | anything else                                                                              |
| `^` `$`                     | the start and end of the line                                                              |
| `\b` `\B`                   | a word boundary, and anywhere else                                                         |
| `*` `+` `?`                 | 0 or more, 1 or more, 0 or 1 (as many as possible; add `?` for as few as possible)         |
| `{m}` `{m,}` `{,n}` `{m,n}` | m, at least m, at most n, m to n times (at most 1000; `?` after makes it lazy)             |
| `(...)` `(?:...)`           | a group, captured for the replacement, or not                                              |
| `a\|b`                      | either                                                                                     |
| `\t` `\r` `\n` `\f` `\v`    | control characters; `\` before any punctuation matches it literally                        |

Without case matching, sets match either case too.
There are no backreferences, lookarounds, POSIX classes, Unicode properties or `\x` and `\u` escapes.
Search time grows with the line length times the pattern's size, never exponentially; a pattern over 4096 instructions (such as `(a{1000}){5}`) is refused.
In the replacement, `\0` is the whole match and `\1` to `\9` the groups, so `(\w+)=(\w+)` replaced with `\2=\1` swaps the sides.

### Project search

Ctrl-Shift-F asks for a pattern (as Ctrl-F does), then which files to search: blank for every loaded file, or patterns such as `*.c *.h`, `src/` (a folder at any depth), `src/*.c` (a path from the start folder) and `!test*` to leave files out.
Open buffers are searched as they are in memory and the others read from disk without being opened.
Progress shows in the message bar, and Esc stops the search, keeping what it found.
Up to 10,000 matching lines are listed as `file:line`, with a preview beside them in a window at least 80 columns wide; Enter opens one with the match selected.
Alt-F shows the list again, finding the matches again in files edited since.

Ctrl-R in the list replaces on the lines the list shows, so filter it first to leave lines out.
Each line is matched again as it is now, and a file read from disk that has changed since is left alone.
Nothing is saved: each changed buffer undoes in one step, and Alt-Shift-Z lists the changed lines and can undo the replace in every file at once.

### Symbols and navigation

Alt-[ goes back to where the cursor was before a jump, and Alt-] forward again.
Jumps are Ctrl-L, Ctrl-Home and Ctrl-End, opening a search match, the symbol and language server commands, and going to a diagnostic; 32 places are kept across buffers.

Alt-S lists the symbols of the buffer (functions, prototypes, types, structs, enums and macros, or Markdown headings), starting on the one named at the cursor, with a preview.
Ctrl-] and Ctrl-[ go to the next or previous definition.
Without a language server, Alt-D goes to the definition of the name at the cursor in any loaded file (on a definition, to its prototype), Alt-Shift-D shows its signature, or that of the call the cursor is in, and Alt-Shift-S lists every symbol of the loaded files.
These use an index built the first time one is used, with progress in the message bar; open buffers are indexed as they are, so a function written but not saved is found.
CSS, HTML, JSON, SQL, XML and YAML have no symbols.

### Git changes

In a file inside a git repository, the cell before each line number marks how the buffer differs from the last commit: a green `▎` for added lines, a yellow `▎` for changed ones and a red `▔` after removed ones.
The marks follow your edits once you pause typing, and the status bar counts the lines (`+3 ~2 -1`).
Alt-Shift-] and Alt-Shift-[ go to the next and previous change.
Alt-G shows the change at the cursor in a panel on the right, with the old lines in red and the new in green.
Alt-Z restores the change at the cursor to the last commit.
Ctrl-G compares every loaded file with the last commit and lists every change, with a preview of the whole file as a diff (Shift with the arrows and pages scrolls it).

The editor reads the repository itself, so no `git` program is needed.
It compares with HEAD, not the staged files, and line numbers must be on (Alt-L) for the marks to show.

### Language servers

A language whose package names a server (the C package names `clangd`) starts it when the first file of that language opens.
The status bar shows it after the language, `C (clangd)`, with `starting`, `waiting` while a command waits for an answer, and `stopped`.
One server runs for each project root, the first folder above the file holding one of the package's `server-roots`.
Alt-V turns servers off and on again, and restarts stopped ones.
A server's messages go to `~/.local/state/editor/<language id>.log`.

With a server, Alt-D asks it for the definition (so local variables, fields and `#include` files work too), Alt-Shift-D shows the declaration, Alt-U lists every use, and Alt-A switches between a C header and its source (clangd only).
Alt-Shift-N renames the symbol at the cursor everywhere the server knows it is used.
Alt-Y applies the server's fix for a diagnostic on the line, and Ctrl-Shift-I formats the buffer.
Changes go to every file as one undo step each, nothing is saved, and Alt-Shift-Z lists them and undoes or redoes them in every file at once.
An answer that comes after the text or cursor changed is dropped, and one that has not come in 5 seconds is given up on.

Diagnostics show as they come: a curly underline (red for errors, yellow for warnings, blue for the rest), the line number in the same colour, and the worst message after the end of the line.
The status bar counts errors and warnings (`E2 W5`).
Ctrl-Shift-] and Ctrl-Shift-[ go to the next and previous one.
Alt-K shows every diagnostic on the cursor's line in full in a panel on the right, with what the server says of the symbol at the cursor once it rests.
Alt-Shift-K lists the diagnostics of every open buffer.

### Images

A PNG, JPEG, GIF or BMP file opens as a picture, drawn at its own size when it fits or shrunk to fit, keeping its shape.
The status bar shows its format and size and how much it is shrunk.
Alt-0 switches to its actual size, where the arrows, pages, Home, End and the wheel move the part shown.
Images are view only: Alt-R reads one again, Alt-X renames, copies or deletes it, and Alt-I describes it.
Transparent parts show over a dark checkerboard, and colours are dithered to 255 for the terminal.
Animated GIFs show their first frame; arithmetic coded, lossless, 12-bit and CMYK JPEGs, damaged files and images over 40 megapixels are not opened.

### Spell check

Alt-C highlights the words that are not in the word list, counts them (up to 10,000) and selects the next one.
Alt-N and Alt-P then step through unknown words.
Words in capitals only, words with digits or `_`, and links are skipped, and a capitalised word is accepted when its lowercase form is in the list.
In code only comments and strings are checked; in Markdown everything but code and link targets; other files in full.
Alt-Shift-C adds the word at the cursor to the list, or removes it when the list knows it, and saves the list at once.
Words in Latin, Greek and Cyrillic letters are checked.

### Highlighting and display

Bash, C, CSS, Go, HTML, JavaScript, JSON, Markdown, PHP, SQL, XML, YAML and Zig are coloured, with languages embedded in others (code blocks in Markdown, scripts in HTML, HTML in PHP).
A file without an extension is Bash when its `#!` line runs `sh` or `bash`.
Buffers over 16 MB, or that take more than a second to parse, are shown uncoloured, and the status bar says `(uncoloured)`.

Alt-W switches soft wrap (in previews too), Alt-L line numbers, Alt-. whitespace, Alt-\ the ruler and Alt-Shift-\ its column, and Alt-T indenting with spaces or tabs.
The status bar shows the name with `[+]` for unsaved changes and `[RO]` for a read-only file, then only what differs from the defaults, and `line:column` last.
The window title names the buffer.

### Pickers and learning

In any list, type to filter (any part of the text, ignoring case; `^` and `$` tie it to the start or end), move with the arrows, pages, Home and End, Enter picks and Esc cancels.
Ctrl-H lists every binding and runs the one picked.
Alt-H asks for the shortcuts held with Ctrl or Alt by what they do, in a random order, and gives the answer after three misses; Esc stops it.

## Key bindings

Ctrl-H lists every binding, filterable, and runs the one picked.
The tables below are generated from the same table in `src/keymap.c` by `tools/bindings.py`, so run it after changing a binding.

<!-- bindings start: generated by tools/bindings.py -->

**Movement**

| Key        | Action                                           |
| ---------- | ------------------------------------------------ |
| Up         | move up                                          |
| Down       | move down                                        |
| Left       | move left                                        |
| Right      | move right                                       |
| Home       | move to the first non-blank, then the line start |
| End        | move to the line end                             |
| PgUp       | move up a page                                   |
| PgDn       | move down a page                                 |
| Ctrl-Left  | move to the previous word                        |
| Ctrl-Right | move to the next word                            |
| Ctrl-Home  | move to the start of the buffer                  |
| Ctrl-End   | move to the end of the buffer                    |
| Alt-Up     | scroll up a line, as the wheel does              |
| Alt-Down   | scroll down a line, as the wheel does            |

**Selection**

| Key              | Action                                                           |
| ---------------- | ---------------------------------------------------------------- |
| Shift-Up         | extend the selection up                                          |
| Shift-Down       | extend the selection down                                        |
| Shift-Left       | extend the selection left                                        |
| Shift-Right      | extend the selection right                                       |
| Shift-Home       | extend the selection to the first non-blank, then the line start |
| Shift-End        | extend the selection to the line end                             |
| Shift-PgUp       | extend the selection up a page                                   |
| Shift-PgDn       | extend the selection down a page                                 |
| Ctrl-Shift-Left  | extend the selection to the previous word                        |
| Ctrl-Shift-Right | extend the selection to the next word                            |
| Ctrl-Shift-Home  | extend the selection to the start of the buffer                  |
| Ctrl-Shift-End   | extend the selection to the end of the buffer                    |
| Ctrl-A           | select the whole buffer                                          |
| Ctrl-M           | select inside the closest brackets (again to widen)              |
| Alt-Right        | expand the selection to the enclosing syntax node (or brackets)  |
| Alt-Left         | shrink the selection back a step                                 |
| Ctrl-Shift-Up    | grow the selection by a line above                               |
| Ctrl-Shift-Down  | grow the selection by a line below                               |
| Alt-Shift-Down   | shrink the selection by a line at the top                        |
| Alt-Shift-Up     | shrink the selection by a line at the bottom                     |
| Esc              | collapse to one cursor, clear the selection, or clear highlights |

**Editing**

| Key              | Action                                                         |
| ---------------- | -------------------------------------------------------------- |
| Enter            | insert a line break                                            |
| Ctrl-Enter       | open a new line below the line                                 |
| Ctrl-Shift-Enter | open a new line above the line                                 |
| Tab              | insert a tab (or indent the selected lines)                    |
| Shift-Tab        | dedent the line (or the selected lines)                        |
| Backspace        | delete the character before the cursor                         |
| Delete           | delete the character after the cursor                          |
| Ctrl-Backspace   | delete the word before the cursor                              |
| Ctrl-Delete      | delete the word after the cursor                               |
| Ctrl-Z           | undo                                                           |
| Ctrl-Y           | redo                                                           |
| Ctrl-C           | copy the selection (or the line)                               |
| Ctrl-X           | cut the selection (or the line)                                |
| Ctrl-V           | paste                                                          |
| Ctrl-Up          | move the line (or the selected lines) up                       |
| Ctrl-Down        | move the line (or the selected lines) down                     |
| Ctrl-D           | duplicate the line (or the selected lines)                     |
| Ctrl-K           | delete the line (or the selected lines)                        |
| Ctrl-/           | toggle comments on the line (or the selected lines)            |
| Alt-J            | join the selected lines (or the line and the next)             |
| Alt-O            | sort the selected lines                                        |
| Ctrl-T           | trim trailing whitespace from the line (or the selected lines) |
| Ctrl-Shift-T     | trim trailing whitespace from every line                       |
| Alt-Shift-U      | convert the selection to upper case                            |
| Alt-Shift-L      | convert the selection to lower case                            |
| Alt-Shift-G      | align the columns of the markdown table                        |
| Alt-C            | spell check the buffer                                         |
| Alt-Shift-C      | add the word at the cursor to the word list (or remove it)     |

**Multiple cursors**

| Key           | Action                                                                      |
| ------------- | --------------------------------------------------------------------------- |
| Ctrl-E        | select the word, then add a cursor at the next occurrence (or search match) |
| Alt-E         | move the newest cursor to the next occurrence, skipping this one            |
| Ctrl-Shift-E  | add a cursor at every occurrence                                            |
| Ctrl-Alt-Up   | add a cursor on the line above                                              |
| Ctrl-Alt-Down | add a cursor on the line below                                              |
| Alt-=         | align the cursors in a column by inserting spaces                           |

**Buffers and files**

| Key          | Action                                                          |
| ------------ | --------------------------------------------------------------- |
| Ctrl-N       | create a new empty buffer                                       |
| Ctrl-O       | open a file or a folder (asks whether to add or replace)        |
| Ctrl-S       | save the buffer (asks for a name if it has none)                |
| Ctrl-Shift-S | save every buffer with unsaved changes                          |
| Alt-R        | reload the buffer from disk (asks before discarding changes)    |
| Alt-Shift-R  | load files added to the folder since it was opened              |
| Alt-Q        | close the buffer (offers to save changes)                       |
| Alt-Shift-Q  | close a folder and its buffers (offers to save changes)         |
| Alt-X        | rename, copy or delete the buffer's file                        |
| Alt-Shift-X  | copy the path of the buffer's file                              |
| Alt-;        | toggle line endings between LF and CRLF                         |
| Alt-I        | count the lines, words, characters and bytes (of the selection) |
| Ctrl-PgDn    | switch to the next buffer                                       |
| Ctrl-PgUp    | switch to the previous buffer                                   |
| Ctrl-Tab     | switch to the buffer used before this one                       |
| Ctrl-P       | pick a buffer                                                   |
| Ctrl-U       | pick a buffer with unsaved changes                              |
| Ctrl-Q       | quit (offers to save changes)                                   |

**Split view**

| Key            | Action                                                    |
| -------------- | --------------------------------------------------------- |
| Ctrl-Alt-Right | split the window, showing this buffer in both views       |
| Ctrl-B         | split the window, with a new empty buffer in the new view |
| Ctrl-Alt-Left  | close this view                                           |
| Ctrl-J         | move focus to the other view                              |
| Ctrl-I         | swap the two views                                        |

**Search**

| Key          | Action                                              |
| ------------ | --------------------------------------------------- |
| Ctrl-F       | search the buffer                                   |
| Alt-N        | go to the next match                                |
| Alt-P        | go to the previous match                            |
| Ctrl-W       | go to the next occurrence of the word at the cursor |
| Ctrl-R       | search and replace in the buffer                    |
| Ctrl-Shift-F | search the loaded files                             |
| Alt-F        | reopen the list of matches in the loaded files      |
| Ctrl-L       | go to a line number                                 |

**Navigation**

| Key         | Action                                                 |
| ----------- | ------------------------------------------------------ |
| Alt-[       | go back to where the cursor was before a jump          |
| Alt-]       | go forward again after going back                      |
| Ctrl-[      | go to the previous definition                          |
| Ctrl-]      | go to the next definition                              |
| Alt-M       | go to the matching bracket                             |
| Alt-S       | pick a symbol in this buffer                           |
| Alt-Shift-S | pick a symbol in any loaded file                       |
| Alt-D       | go to the definition of the name at the cursor         |
| Alt-Shift-D | show the signature of the name (or call) at the cursor |

**Changes since the last git commit**

| Key         | Action                                                  |
| ----------- | ------------------------------------------------------- |
| Alt-Shift-] | go to the next change                                   |
| Alt-Shift-[ | go to the previous change                               |
| Alt-G       | show or hide the change at the cursor beside the buffer |
| Alt-Z       | revert the change at the cursor to the last commit      |
| Ctrl-G      | pick a change since the last commit                     |

**Diagnostics from language servers**

| Key          | Action                                                                             |
| ------------ | ---------------------------------------------------------------------------------- |
| Ctrl-Shift-] | go to the next diagnostic                                                          |
| Ctrl-Shift-[ | go to the previous diagnostic                                                      |
| Alt-K        | show or hide the diagnostics at the cursor beside the buffer                       |
| Alt-Shift-K  | pick a diagnostic in the open buffers                                              |
| Alt-U        | pick a use of the name at the cursor (language server)                             |
| Alt-A        | switch between a header and its source (clangd)                                    |
| Alt-Shift-N  | rename the name at the cursor everywhere (language server)                         |
| Alt-Y        | fix a diagnostic on the line (language server)                                     |
| Ctrl-Shift-I | format the buffer (language server)                                                |
| Alt-Shift-Z  | list the last change across files (ctrl+z and ctrl+y in the list undo and redo it) |

**Display and settings**

| Key         | Action                                                         |
| ----------- | -------------------------------------------------------------- |
| Alt-T       | toggle indenting with spaces or tabs                           |
| Alt-W       | toggle soft wrap                                               |
| Alt-L       | toggle line numbers                                            |
| Alt-.       | toggle showing whitespace                                      |
| Alt-\       | toggle the vertical ruler                                      |
| Alt-0       | switch an image between fitting the window and its actual size |
| Alt-V       | toggle language servers (or restart stopped ones)              |
| Alt-Shift-\ | set the ruler column (and show the ruler)                      |
| Ctrl-H      | show this list of shortcuts                                    |
| Alt-H       | practise the shortcuts (esc to stop)                           |

<!-- bindings end -->

## Settings

Settings are compile-time constants in `src/settings.h`: change one and rebuild.
Those marked with a key only set how the editor starts, and the key changes them for the session.

| Setting                                                | Default            | Key                |
| ------------------------------------------------------ | ------------------ | ------------------ |
| `tabWidth`                                             | 2 columns          |                    |
| `expandTabDefault`                                     | indent with spaces | Alt-T              |
| `softWrapDefault`                                      | off                | Alt-W              |
| `lineNumbersDefault`                                   | on                 | Alt-L              |
| `showWhitespaceDefault`                                | on                 | Alt-.              |
| `rulerDefault`, `rulerColumnDefault`                   | off, column 100    | Alt-\, Alt-Shift-\ |
| `trimTrailingWhitespace`                               | off (trim on save) |                    |
| `languageServersDefault`                               | on                 | Alt-V              |
| `scrollOffset`                                         | 4 rows and columns |                    |
| `wheelRows`                                            | 3                  |                    |
| `cursorStyle`, `cursorBlink`                           | a blinking line    |                    |
| `maxFileSize`                                          | 100 MB             |                    |
| `maxFileCount`                                         | 100,000 files      |                    |
| `maxHighlightSize`, `maxDiffSize`, `maxServerFileSize` | 16 MB              |                    |
| `recoverDelay`                                         | 2 s                |                    |

The rest of the file holds the other limits and timings, each with a comment.

## Files the editor reads and writes

- `~/.config/editor/languages/<id>/`: the language packages, read at startup.
- `~/.config/editor/words.txt`: the spell check word list, written by Alt-Shift-C.
- `~/.local/state/editor/<language id>.log`: each language server's error output, emptied when one starts.
- `~/.local/state/editor/recover/`: copies of unsaved buffers (see Recovery).
- `.editor` in any loaded folder: patterns of files to leave out.
- `.<name>.editor-XXXXXX` next to a file being saved, renamed over it at once.
- For clangd: `compile_commands.json`, `compile_flags.txt` or `.clangd` in the project (read by clangd), and its index in `.cache/clangd`.

`~/.config` and `~/.local/state` are `$XDG_CONFIG_HOME` and `$XDG_STATE_HOME` when they are set.

## Foot

The editor is written for foot alone.
It relies on foot's kitty keyboard protocol, so that keys like Ctrl-M, Ctrl-H, Ctrl-I, Ctrl-J, Ctrl-[ and Ctrl-Shift-S reach it as themselves, and its decoder only understands the sequences foot sends.
At startup it asks the terminal whether the protocol is there and exits with an error if not (as in the Linux console, or inside tmux).
Nothing in `foot.ini` needs changing.

Foot's default key bindings keep these keys for foot, so the editor never binds them: Ctrl-Shift-C and Ctrl-Shift-V (clipboard), Ctrl-Shift-R (search), Ctrl-Shift-N (new window), Ctrl-Shift-O (URLs), Ctrl-Shift-U (Unicode input), Ctrl-Shift-Z and Ctrl-Shift-X (prompt jumps), Ctrl-=, Ctrl-+, Ctrl-- and Ctrl-0 (font size), and Shift-Insert (primary paste).
That is why renaming a symbol is Alt-Shift-N rather than Ctrl-Shift-R, and aligning cursors Alt-= rather than Ctrl-=.
A binding you add to `foot.ini` takes its key from the editor too.

The window title names the current buffer, `editor: src/app.c`, with ` [+]` while it has unsaved changes, and foot gets its old title back when the editor quits.
Foot's Ctrl-Shift-N opens a new window in the folder of the current buffer's file, which the editor reports as it changes (OSC 7).
Images are drawn with foot's sixel graphics.
The editor turns on mouse reporting, so foot sends it clicks and the wheel; holding Shift still makes foot's own selection, and a desktop that moves windows with Alt-drag keeps Alt-drag from reaching the editor.

## Troubleshooting

- **"no kitty keyboard protocol (the editor runs in foot)"**: the editor was started outside foot, or inside tmux or screen.
- **Nothing is coloured, and Ctrl-/ does nothing**: the language packages are not installed.
  Run `./build.sh --languages --install`.
- **The status bar says `(uncoloured)`**: the buffer is over 16 MB, or its parse took more than a second of processor time.
  Highlighting stays off for that buffer until Alt-R reloads it.
- **A package is left out at startup**: its `language` file has a mistake, which the message names; a grammar that fails to load is named when a file of its language opens.
- **The status bar says `stopped` after the server's name**: the language server could not start or ended.
  Its log, `~/.local/state/editor/<language id>.log`, says why; Alt-V starts it again.
  Check that the program is on your `PATH`, and for clangd that the project has a `compile_commands.json`.
- **Alt-U finds uses only in open files**: clangd needs `compile_commands.json` to know every file of the project.
- **Copying says "the system clipboard is not available"**: install `wl-clipboard`; until then copy and paste use the editor's own clipboard.
- **Spell check says "no word list"**: put a list of words at the path it names, or run `./build.sh --install`.
- **A key does nothing**: foot, your `foot.ini` or the desktop may take it before the editor sees it (see Foot).
- **The window closed with unsaved changes**: open the copies in `~/.local/state/editor/recover` (see Recovery).

## Known limitations

- It runs only in foot.
- Files are not watched: a change by another program is noticed when the window regains focus, on switching to the buffer, or on saving.
- Regular expressions have no backreferences or lookaround, and a match never crosses a line.
- Git marks need a typical repository (no worktrees, submodules, reftable or bare repositories), compare with HEAD rather than the index, and do not apply LFS or other filters.
- Highlighting, git marks and language servers stop at 16 MB, and files over 100 MB are not opened.
- Undo history has no limit, lives in memory, and is cleared by a reload.
- Case conversion and folding use Unicode's simple one to one mappings, and spell check only knows Latin, Greek and Cyrillic letters.
- Brackets and quotes are not closed as you type.
- Settings are compile-time, and nothing (open buffers, search history) is kept between sessions.
- Language servers cannot run commands of their own (`workspace/applyEdit`), so fixes that need one are not offered.

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

`language` holds one `key = value` per line.
A line starting with `#` is a comment (there are no comments after a value, since `#` can be a comment marker):

- `name`: the display name, shown in the status bar.
  Required with `extensions`, `files` or `interpreters`, and left out for a grammar that is only embedded in others (`markdown-inline`).
- `extensions`: extensions without the dot, separated by spaces, matched ignoring case.
- `files`: whole file names for files without an extension, matching case (`PKGBUILD .bashrc`).
- `interpreters`: programs a `#!` first line may name, directly or through `env`, for files without an extension (`sh bash`).
- `comment`: what Ctrl-/ puts before a line (`//`).
  Left out, the language has no comments.
- `comment-end`: what it puts after a line, for block comments (`-->`).
- `inject`: the names other grammars embed it by, such as `bash shell`.
  The default is the id.
  Markdown fences also find a language by its extensions.
- `symbol`: the function of `parser.so` that gives the grammar.
  The default is `tree_sitter_` and the id with `-` as `_`.
- `prose`: `yes` to spell check all text outside code, as in Markdown, rather than only comments and strings.
- `fresh-parse`: `yes` to parse from scratch after every edit, for a grammar whose scanner gives different trees when parsing incrementally (Markdown's inline grammar).
- `first-pattern-wins`: `yes` when the highlight query puts its specific patterns first (Go's does): for captures of the same node, the earlier pattern wins rather than the later one.
- `server`: the language server to start for files of the language: the program (looked up in `PATH`) and its arguments, separated by blanks, with no quoting (`clangd --background-index`).
  It must speak the Language Server Protocol over its standard input and output.
- `server-roots`: names of files or folders that mark a project's root for the server, separated by blanks: the first folder above a file holding one of them is the root.
  The default is `.git`.
  Needs `server`.

Without `parser.so` a language still has its files and comment markers, just no colours or symbols.
A package whose `language` file is wrong is left out, and the problem is shown when the editor starts.
A `parser.so` that does not load (a missing entry point, or a tree-sitter ABI other than 13 to 15) or a query that does not compile is reported when a file of that language first opens, and that language is left uncoloured.
Packages are read only at startup, so a new or changed package takes effect the next time the editor starts.
Removing a language is deleting its folder.

To make a package from a grammar repository:

```sh
tools/language.sh ~/src/tree-sitter-lua lua
```

This compiles the repository's `src/parser.c` (and `src/scanner.c`, if it has one) into `parser.so`, copies its `queries/highlights.scm` and `queries/injections.scm`, and writes a `language` file to correct unless the package already has one.
For a repository holding several grammars, name the folder of the one wanted (`tree-sitter-php/php`).
Repositories that do not commit `src/parser.c` need `tree-sitter generate` first.
Then check that the query's capture names map to theme groups (see ARCHITECTURE.md, "Colouring lines"), that any predicates it uses are among those the editor supports (an unknown one is ignored and lets its pattern match everything), and which pattern it expects to win when two capture the same node (most put general patterns first, the default).
For symbols, write `tags.scm` to the contract in ARCHITECTURE.md ("Symbols"): adapt upstream's `tags.scm` if it has one, so that `@definition.<kind>` captures the whole declaration.

A `parser.so` runs inside the editor with your privileges, so only install grammars you would run as a program.
Packages are only ever read from the config folder, never from a project being edited.
The same goes for a `server`: it is a program the editor runs, so a project can never choose one.

To add a language to the default packages instead, put its generated `src/parser.c` and `src/scanner.c` in `vendor/grammars/<id>/` with the grammar's `src/tree_sitter/` headers and any other files its scanner includes (when the scanner includes a header by a relative path outside `src/`, as PHP's and XML's do, keep upstream's layout), its queries in `vendor/queries/<id>/`, and the editor's own files (`language`, and `tags.scm` when it has symbols) in `languages/<id>/`.
`./build.sh --languages` builds every folder in `vendor/grammars`.

### Language servers

A server only needs `server` (and, when `.git` does not mark the projects' roots, `server-roots`) in the package's `language` file; the rest comes from the server itself.
How the server builds the project is its own configuration, kept with the project and shared with other tools, not the editor's.
For `clangd`, that is one of:

- `compile_commands.json`, written by the build system (CMake's `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, or `bear -- make`), which gives every file its exact flags.
- `compile_flags.txt`, one flag per line, used for every file: `-std=c23`, `-Iinclude`, `-Wall`.
- `.clangd`, YAML that adds or removes flags (`CompileFlags: Add: [-std=c23, -Iinclude]`) and says which diagnostics to show (`Diagnostics: Suppress: [unused-parameter]`), with `If: PathMatch:` blocks for parts of the tree.
  This repository's own `.clangd` is an example.

Only `compile_commands.json` lists every file of the project, which is what clangd's background index (`--background-index`) goes through.
Without it, clangd only knows the open files and what they include, so Alt-U and renames find uses only there, while diagnostics, Alt-D and Alt-Shift-D work the same.
`build.sh` writes this repository's `compile_commands.json`.
The index is kept in `.cache/clangd` at the root, so leave `.cache/` out with `.editor` and `.gitignore`.

## Adding a key binding

1. Add a `Command…` value to `enum Command` in `src/keymap.h`.
2. Add a row to `keymapBindings` in `src/keymap.c`, under the group comment it belongs to: the key, its modifiers, the command and the help text Ctrl-H shows.
   Avoid the keys foot keeps (see Foot).
3. Run the command in `src/app.c`: in the dispatch function for its kind (`MoveCommand`, `EditCommand`, `NavigationCommand`, `BufferCommand` or `SearchCommand`), or in `RunCommand`'s own switch.
4. If it only looks at the buffer or works on other buffers, add it to `ViewOnly` so it also runs on an image.
5. Run `tools/bindings.py` to update the tables in this file.

Ctrl-H and learning mode read the same table, so they pick the binding up without more work.
To add a setting, add a `static constexpr` with a comment to `src/settings.h`, and name it with a `Default` suffix when a key toggles it at runtime.
