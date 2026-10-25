#pragma once

// spell checking against the word list in the config folder (editor/words.txt), one word
// per line in any order, or the built-in list (builtin.h) while there is none there.
// a word is a run of letters with apostrophes allowed inside it.
// runs holding digits or "_", words in capitals only, and anything inside a url or an
// email address are never checked.
// a word is known when it is in the list as written or, when capitalised, in lowercase.
#include "strbuf.h"
#include "types.h"

// reads the word list when it has not been read or changed on disk since, and tells
// whether there is one; problems (such as there being none) are appended to message.
bool
SpellReady(StrBuf *message);

// finds the first unknown word in a line that starts at or after from; none before
// SpellReady has read a list.
bool
SpellFind(const char *text, u64 length, u64 from, u64 *start, u64 *end);

// finds the word spell check would look at around at (holding it or ending there).
bool
SpellWordAt(const char *text, u64 length, u64 at, u64 *start, u64 *end);

// gives the entry of the list that makes the word from start to end known (as written, or
// in lowercase when capitalised) and returns true, or gives the word as it would be added
// and returns false.
bool
SpellEntry(const char *text, u64 start, u64 end, StrBuf *entry);

// adds a word to the list or removes it, and saves the list; false after a problem, which
// is appended to message, and then the list is as it was.
bool
SpellChange(const char *word, u64 length, bool add, StrBuf *message);
