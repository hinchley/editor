#pragma once

// spell checking against the word list compiled in from data/words.txt.
// a word is a run of letters with apostrophes allowed inside it.
// runs holding digits or "_", words in capitals only, and anything inside a url or an
// email address are never checked.
// a word is known when it is in the list as written or, when capitalised, in lowercase.
#include "types.h"

// finds the first unknown word in a line that starts at or after from.
bool
SpellFind(const char *text, u64 length, u64 from, u64 *start, u64 *end);
