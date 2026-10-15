#pragma once

// the search commands: incremental find, stepping through matches, finding the word under
// the cursor, search and replace, spell check, and the prompts of project search and
// replace.
// the last search pattern is shared by every buffer, and each document chooses what it
// highlights: matches of that pattern, unknown words, or nothing.
#include "document.h"
#include "key.h"
#include "match.h"
#include "strbuf.h"
#include "workspace.h"

// the commands act on the workspace's current document and report to message.
void
SearchInit(Workspace *workspace, StrBuf *message);

// incremental find: each change to the typed text shows the first match from the cursor.
// up and down (or ctrl+p and ctrl+n) step between matches, and ctrl+t toggles matching
// case.
// enter keeps the match, and escape puts the cursors, scroll position and highlights back
// as they were.
void
SearchFind(void);

// selects the next match (or the previous one, with backward) of the last search, or the
// next unknown word when the document shows unknown words.
void
SearchStep(bool backward);

// searches for the whole word under the cursor, matching case, and selects its next
// occurrence.
void
SearchWord(void);

// asks for a pattern and its replacement, then for each match whether to replace it:
// (y)es, (n)o, (a)ll the rest or (q)uit.
// the whole run is one undo step.
void
SearchReplace(void);

// highlights the unknown words and selects the next one.
void
SearchSpell(void);

// receives the pattern of a project search, which has become the last search.
typedef void (*SearchProjectDone)(const Pattern *pattern);

// asks for the pattern of a project search, as find does, and hands it to done unless
// the prompt is cancelled or left empty.
void
SearchProject(SearchProjectDone done);

// receives the replacement text with its escapes replaced, or nullptr when the prompt was
// cancelled.
typedef void (*SearchReplacementDone)(const StrBuf *replacement);

// asks what to replace pattern with, as replace does (up or down recalls the last
// replacement), and hands the answer to done.
void
SearchReplacement(const Pattern *pattern, SearchReplacementDone done);

// makes pattern the last search and highlights its matches in document.
void
SearchShow(Document *document, const Pattern *pattern);

// true while a find, replace or project search or replace prompt is open, which takes keys before
// the prompt does.
bool
SearchActive(void);

void
SearchKey(Key key);

// stops highlighting matches in the current document.
// returns false when nothing was highlighted.
bool
SearchClear(void);

// what a document highlights.
MatchSet
SearchMarks(const Document *document);
