#include "search.h"

#include <ctype.h>
#include <string.h>

#include "app.h"
#include "move.h"
#include "prompt.h"
#include "settings.h"
#include "spell.h"
#include "utf8.h"

// which search prompt is open.
enum SearchPrompt {
  SearchPromptNone,
  SearchPromptFind,
  SearchPromptPattern,
  SearchPromptReplacement,
  SearchPromptProject,
  SearchPromptProjectReplacement
};

// what escape puts back after incremental find.
typedef struct SearchSaved {
  CursorSet cursors;
  ViewRow top;
  u64 leftColumn;
  enum MatchKind marks;
  StrBuf source;
  PatternOptions options;
} SearchSaved;

// a replace in progress.
// it runs from where it started to the end of the buffer, then wraps round to stop, which
// moves with the replacements made before it; or within a selection of several lines, from
// its start to stop at its end, without wrapping.
typedef struct Replacing {
  StrBuf text;
  Range match;
  Position next;
  Position stop;
  bool wrapped;
  bool within;
  u64 asked;
  u64 decided;
  u64 replaced;
} Replacing;

// the last search, and whether a new one always matches case (ctrl+t) and is a regular
// expression (ctrl+r), for the whole session.
static Pattern last;
static bool caseSensitive;
static bool regex;
static StrBuf lastReplacement;

// patterns or replacements given to the prompts, newest first.
typedef struct History {
  StrBuf entries[searchHistorySize];
  u32 count;
} History;

static History patterns;
static History replacements;

// the history entry the open prompt was last filled with (from 1), or 0.
static u32 recalled;

// the text of the open prompt, compiled to find out whether it is a valid pattern.
static Pattern typed;

static enum SearchPrompt prompt;
static SearchProjectDone projectDone;
static SearchReplacementDone replacementDone;
static SearchSaved saved;
static Replacing replacing;

// the word alt+shift+c asks about, and whether it is to be added to the word list.
static StrBuf spellWord;
static bool spellAdd;

// where incremental find starts, and the match it shows.
static Position origin;
static bool found;
static bool wrapped;
static Range current;

// puts text first in history, moving it there when it is already in it, and dropping the
// oldest entry when there is no room.
static void
HistoryAdd(History *history, const char *text, u64 length)
{
  if (!length) return;

  u32 index = 0;
  while (index < history->count) {
    const StrBuf *entry = &history->entries[index];
    if (entry->length == length && memcmp(entry->data, text, length) == 0) break;
    index++;
  }

  if (index == history->count) {
    if (history->count < searchHistorySize) history->count++;
    index = history->count - 1;
  }

  // the entry's buffer is reused for the new first one.
  StrBuf first = history->entries[index];
  memmove(&history->entries[1], &history->entries[0], index * sizeof(StrBuf));
  StrBufClear(&first);
  StrBufAppend(&first, text, length);
  history->entries[0] = first;
}

static Document *
Current(void)
{
  return WorkspaceDocument(workspace);
}

MatchSet
SearchMarks(const Document *document)
{
  Highlight *scope = document->marks == MatchKindSpell ? document->highlight : nullptr;
  return (MatchSet){document->marks, &last, scope};
}

// whether a search for source matches case: always after ctrl+t, and otherwise when it has
// an upper case letter (smart case).
// the character after a backslash is an escape (\S in a regular expression), not a letter
// typed.
static bool
MatchesCase(const char *source, u64 length)
{
  if (caseSensitive) return true;

  for (u64 at = 0; at < length;) {
    if (source[at] == '\\') {
      at += 2;
      continue;
    }

    u32 codepoint;
    at += Utf8Decode(source + at, length - at, &codepoint);
    if (Utf8Lower(codepoint) != codepoint) return true;
  }

  return false;
}

static void
SetPattern(const char *source, u64 length)
{
  PatternOptions options = {.caseSensitive = MatchesCase(source, length), .regex = regex};
  PatternSet(&last, source, length, options);
}

// sets source to a pattern matching the primary selection literally, as a regular expression
// when regex is on, and returns false when nothing is selected or the selection crosses a
// line.
static bool
SelectedSource(StrBuf *source)
{
  Document *document = Current();
  Range selection = CursorRange(CursorSetPrimary(&document->view->cursors));
  bool empty = PositionCompare(selection.start, selection.end) == 0;
  if (empty || selection.start.line != selection.end.line) return false;

  const StrBuf *line = BufferLine(&document->buffer, selection.start.line);
  const char *text = line->data + selection.start.byte;
  u64 length = selection.end.byte - selection.start.byte;
  if (!regex) {
    PatternEscape(text, length, source);
    return true;
  }

  for (u64 at = 0; at < length; at++) {
    if (ispunct((u8)text[at])) StrBufAppendByte(source, '\\');
    StrBufAppendByte(source, text[at]);
  }

  return true;
}

// appends which of the search's matches in the buffer match is, of how many ("3/17"), and
// returns false, counting nothing, when the buffer or the count is too large to count
// quickly.
static bool
AppendPlace(Range match, StrBuf *text)
{
  const Buffer *buffer = &Current()->buffer;
  u64 lineCount = BufferLineCount(buffer);
  u64 size = 0;

  for (u64 line = 0; line < lineCount; line++) {
    size += BufferLine(buffer, line)->length + 1;
    if (size > maxMatchCountSize) return false;
  }

  MatchSet set = {.kind = MatchKindSearch, .pattern = &last};
  u64 count = 0;
  u64 place = 0;

  for (u64 line = 0; line < lineCount; line++) {
    PatternMatch hit = {};

    while (MatchInLine(&set, buffer, line, hit.end, UINT64_MAX, &hit)) {
      if (++count > maxMatchCount) return false;
      if (PositionCompare((Position){line, hit.start}, match.start) <= 0) place = count;
    }
  }

  if (!place) return false;

  StrBufAppendFormat(text, "%lu/%lu", place, count);
  return true;
}

// says in the message which match match is, when they can be counted.
static bool
ReportPlace(Range match)
{
  StrBufAppendString(message, "match ");
  if (AppendPlace(match, message)) return true;

  StrBufTruncate(message, message->length - strlen("match "));
  return false;
}

static void
Select(Range range)
{
  MoveSelect(Current()->view, range.start, range.end);
}

static void
ReportNoMatch(const MatchSet *set)
{
  if (set->kind == MatchKindSpell) {
    StrBufAppendString(message, "no unknown words");
  } else if (last.error.length) {
    StrBufAppendFormat(message, "bad pattern: %s", last.error.data);
  } else if (last.impossible && last.text.length) {
    StrBufAppendString(message, "a match cannot cross a line break");
  } else {
    StrBufAppendFormat(message, "no match for %s", StrBufText(&last.source));
  }
}

// true when the text of the open prompt is a regular expression that does not compile.
static bool
BadPattern(void)
{
  const StrBuf *text = PromptText();
  if (!regex || !text->length) return false;

  PatternOptions options = {
    .caseSensitive = MatchesCase(text->data, text->length),
    .regex = true
  };
  PatternSet(&typed, text->data, text->length, options);
  return typed.error.length;
}

// labels the open prompt with what it does and, in brackets, the state of the search.
static void
Label(void)
{
  const char *notes[4];
  u32 count = 0;
  bool find = prompt == SearchPromptFind;
  bool bad = BadPattern();
  StrBuf place = {};
  const StrBuf *text = PromptText();
  if (MatchesCase(StrBufText(text), text->length)) notes[count++] = "match case";
  if (regex) notes[count++] = bad ? "regex, bad pattern" : "regex";
  if (find && text->length && !found && !bad) notes[count++] = "no match";
  if (find && found && AppendPlace(current, &place)) notes[count++] = place.data;
  if (find && found && wrapped) notes[count++] = "wrapped";

  StrBuf label = {};
  if (find) StrBufAppendString(&label, "find");
  else if (prompt == SearchPromptProject) StrBufAppendString(&label, "search files");
  else StrBufAppendString(&label, "replace");

  for (u32 index = 0; index < count; index++) {
    StrBufAppendString(&label, index ? ", " : " [");
    StrBufAppendString(&label, notes[index]);
  }

  StrBufAppendString(&label, count ? "]: " : ": ");
  PromptSetLabel(label.data);
  StrBufFree(&label);
  StrBufFree(&place);
}

static void
RestoreView(void)
{
  View *view = Current()->view;
  CursorSetCopy(&view->cursors, &saved.cursors);
  view->top = saved.top;
  view->leftColumn = saved.leftColumn;
}

// shows the first match from where find started, or the view as it was without one.
static void
ShowFirst(void)
{
  MatchSet set = {.kind = MatchKindSearch, .pattern = &last};
  found = MatchFind(&set, &Current()->buffer, origin, false, &current, &wrapped);
  if (found) Select(current);
  else RestoreView();
  Label();
}

// relabels the replace and project search prompts as their text changes.
static void
Relabel(
  [[maybe_unused]] void *context,
  [[maybe_unused]] const char *text,
  [[maybe_unused]] u64 length
)
{
  Label();
}

// true when the last pattern found nothing and text only adds to it, so text cannot match
// either: it is literal text (no whole word, no "$" at the end, the same case matching)
// that every match of text would hold a match of.
// the last byte of the old pattern and the first added one are both ascii, which keeps
// a grapheme break between them, and the old one is not a backslash, which would start
// an escape.
static bool
CannotMatch(const char *text, u64 length)
{
  const StrBuf *old = &last.source;
  bool literal = !regex && !last.options.regex && !last.options.wholeWord;
  if (found || !literal || last.lineEnd || !last.text.length) return false;
  bool matchesCase = MatchesCase(text, length);
  if (last.options.caseSensitive != matchesCase || length <= old->length) return false;

  u8 before = (u8)old->data[old->length - 1];
  u8 after = (u8)text[old->length];
  if (before >= 0x80 || after >= 0x80 || before == '\\') return false;
  return memcmp(text, old->data, old->length) == 0;
}

static void
FindChanged([[maybe_unused]] void *context, const char *text, u64 length)
{
  // the buffer cannot change while find is open, so the search can be skipped.
  bool hopeless = CannotMatch(text, length);
  SetPattern(text, length);
  if (hopeless) Label();
  else ShowFirst();
}

static void
FindStep(bool backward)
{
  if (!found) return;

  MatchSet set = {.kind = MatchKindSearch, .pattern = &last};
  Position from = backward ? current.start : current.end;
  MatchFind(&set, &Current()->buffer, from, backward, &current, &wrapped);
  Select(current);
  Label();
}

static void
FindDone([[maybe_unused]] void *context, const char *answer, u64 length)
{
  prompt = SearchPromptNone;

  // escape, or enter with nothing typed, puts everything back.
  if (!answer || !length) {
    RestoreView();
    Current()->marks = saved.marks;
    PatternSet(&last, StrBufText(&saved.source), saved.source.length, saved.options);
    return;
  }

  HistoryAdd(&patterns, answer, length);

  if (!found) {
    MatchSet set = SearchMarks(Current());
    ReportNoMatch(&set);
    return;
  }

  if (ReportPlace(current)) StrBufAppendString(message, " (alt+n and alt+p step)");
}

void
SearchFind(void)
{
  Document *document = Current();
  View *view = document->view;
  CursorSetCopy(&saved.cursors, &view->cursors);
  saved.top = view->top;
  saved.leftColumn = view->leftColumn;
  saved.marks = document->marks;
  StrBufClear(&saved.source);
  StrBufAppend(&saved.source, StrBufText(&last.source), last.source.length);
  saved.options = last.options;

  // a selection is the first thing found, from where it starts.
  StrBuf selected = {};
  bool hasSelection = SelectedSource(&selected);
  CursorSetCollapse(&view->cursors);
  Cursor *primary = CursorSetPrimary(&view->cursors);
  origin = hasSelection ? CursorRange(primary).start : primary->head;
  found = false;
  wrapped = false;
  document->marks = MatchKindSearch;
  SetPattern("", 0);

  prompt = SearchPromptFind;
  recalled = 0;
  PromptAsk("find: ", "", FindDone, nullptr);
  PromptHandle(FindChanged, nullptr);
  if (hasSelection) PromptSetText(selected.data, selected.length);
  else Label();
  StrBufFree(&selected);
}

void
SearchStep(bool backward)
{
  Document *document = Current();

  if (document->marks != MatchKindSpell && !last.source.length) {
    StrBufAppendString(message, "nothing searched for yet");
    return;
  }

  if (document->marks != MatchKindSpell) document->marks = MatchKindSearch;

  View *view = document->view;
  CursorSetCollapse(&view->cursors);
  Range selection = CursorRange(CursorSetPrimary(&view->cursors));
  Position from = backward ? selection.start : selection.end;
  MatchSet set = SearchMarks(document);
  Range match;
  bool around;

  if (!MatchFind(&set, &document->buffer, from, backward, &match, &around)) {
    ReportNoMatch(&set);
    return;
  }

  bool same = PositionCompare(match.start, selection.start) == 0 &&
    PositionCompare(match.end, selection.end) == 0;

  const char *wrappedTo = backward ? "wrapped to the end" : "wrapped to the start";
  Select(match);

  if (around && same) {
    StrBufAppendString(message, "this is the only match");
    return;
  }

  bool placed = set.kind == MatchKindSearch && ReportPlace(match);
  if (around) StrBufAppendFormat(message, "%s%s", placed ? ", " : "", wrappedTo);
}

void
SearchWord(void)
{
  Document *document = Current();
  View *view = document->view;
  CursorSetCollapse(&view->cursors);
  Position head = CursorSetPrimary(&view->cursors)->head;
  const StrBuf *line = BufferLine(&document->buffer, head.line);
  u64 start;
  u64 end;

  if (!MoveWordAround(line, head.byte, &start, &end)) {
    StrBufAppendString(message, "no word under the cursor");
    return;
  }

  PatternOptions options = {.caseSensitive = true, .wholeWord = true};
  PatternSet(&last, line->data + start, end - start, options);
  HistoryAdd(&patterns, line->data + start, end - start);
  document->marks = MatchKindSearch;

  MatchSet set = SearchMarks(document);
  Range match;
  bool around;
  Position after = {head.line, end};
  if (!MatchFind(&set, &document->buffer, after, false, &match, &around)) return;

  bool same = match.start.line == head.line && match.start.byte == start;
  if (around && same) StrBufAppendString(message, "no other occurrence");
  else if (around) StrBufAppendString(message, "wrapped to the start");
  Select(match);
}

void
SearchSpell(void)
{
  if (!SpellReady(message)) return;

  Document *document = Current();
  document->marks = MatchKindSpell;
  MatchSet set = SearchMarks(document);
  View *view = document->view;
  Position from = CursorSetPrimary(&view->cursors)->head;
  Range match;
  bool around;

  if (!MatchFind(&set, &document->buffer, from, false, &match, &around)) {
    document->marks = MatchKindNone;
    ReportNoMatch(&set);
    return;
  }

  CursorSetCollapse(&view->cursors);
  Select(match);

  // counting stops at maxSpellCount, as counting every word of a large file takes seconds.
  u64 count = MatchCount(&set, &document->buffer, maxSpellCount + 1);
  if (count > maxSpellCount) {
    StrBufAppendFormat(message, "over %lu unknown words", maxSpellCount);
  } else {
    StrBufAppendFormat(message, "%lu unknown word%s", count, StrBufPlural(count));
  }
}

static void
SpellWordAnswered(
  [[maybe_unused]] void *context,
  const char *answer,
  [[maybe_unused]] u64 length
)
{
  if (!answer || *answer != 'y') return;
  if (!SpellChange(spellWord.data, spellWord.length, spellAdd, message)) return;

  const char *done = spellAdd ? "added \"%s\" to" : "removed \"%s\" from";
  StrBufAppendFormat(message, done, spellWord.data);
  StrBufAppendString(message, " the word list");
}

void
SearchSpellWord(void)
{
  if (!SpellReady(message)) return;

  Document *document = Current();
  Position head = CursorSetPrimary(&document->view->cursors)->head;
  const StrBuf *line = BufferLine(&document->buffer, head.line);
  u64 start;
  u64 end;

  if (!SpellWordAt(StrBufText(line), line->length, head.byte, &start, &end)) {
    StrBufAppendString(message, "no word to check at the cursor");
    return;
  }

  spellAdd = !SpellEntry(line->data, start, end, &spellWord);
  StrBuf question = {};
  StrBufAppendFormat(
    &question,
    spellAdd ? "add \"%s\" to" : "remove \"%s\" from",
    spellWord.data
  );
  StrBufAppendString(&question, " the word list? (y/n)");
  PromptChoose(question.data, "yn", SpellWordAnswered, nullptr);
  StrBufFree(&question);
}

// the next match of the replace, or false once it has come round to its stop.
static bool
NextMatch(Range *match)
{
  const Buffer *buffer = &Current()->buffer;

  for (;;) {
    u64 lastLine = replacing.wrapped ? replacing.stop.line : BufferLineCount(buffer) - 1;

    for (u64 line = replacing.next.line; line <= lastLine; line++) {
      const StrBuf *text = BufferLine(buffer, line);
      u64 from = line == replacing.next.line ? replacing.next.byte : 0;
      PatternMatch hit;
      if (!PatternFind(&last, text->data, text->length, from, UINT64_MAX, &hit)) continue;

      // within a selection, a match must end inside it too.
      u64 edge = replacing.within ? hit.end : hit.start + 1;
      bool stopLine = replacing.wrapped && line == replacing.stop.line;
      if (stopLine && edge > replacing.stop.byte) return false;

      *match = (Range){{line, hit.start}, {line, hit.end}};
      return true;
    }

    if (replacing.wrapped) return false;

    replacing.wrapped = true;
    replacing.next = (Position){0, 0};
  }
}

// replaces a range, keeping the stop on the same text, and continues after it.
static void
Apply(Range range, const char *text, u64 length)
{
  Position end = BufferReplace(&Current()->buffer, range, text, length);
  BufferChange change = {.start = range.start, .oldEnd = range.end, .newEnd = end};
  replacing.stop = BufferAdjust(replacing.stop, &change);
  replacing.next = end;
}

// replaces match and the matches after it on the same line (up to the stop) in one change.
static void
ReplaceLine(Range match, StrBuf *rebuilt)
{
  u64 lineIndex = match.start.line;
  const StrBuf *line = BufferLine(&Current()->buffer, lineIndex);
  bool stopHere = replacing.wrapped && lineIndex == replacing.stop.line;
  u64 limit = stopHere ? replacing.stop.byte : UINT64_MAX;
  PatternMatch first = {match.start.byte, match.end.byte};
  u64 count = 0;
  const StrBuf *with = &replacing.text;
  u64 end =
    PatternReplaceLine(&last, line->data, line->length, first, limit, with, rebuilt, &count);

  replacing.replaced += count;
  replacing.decided += count;
  Range range = {match.start, {lineIndex, end}};
  Apply(range, rebuilt->data ? rebuilt->data : "", rebuilt->length);
}

static void
ReplaceRest(Range match)
{
  StrBuf rebuilt = {};

  do {
    ReplaceLine(match, &rebuilt);
  } while (NextMatch(&match));

  StrBufFree(&rebuilt);

  // the cursor goes after the last replacement.
  Select((Range){replacing.next, replacing.next});
}

static void
FinishReplace(void)
{
  Document *document = Current();
  UndoEnd(&document->undo, &document->view->cursors);

  if (!replacing.asked) {
    MatchSet set = SearchMarks(document);
    ReportNoMatch(&set);
    return;
  }

  u64 count = replacing.decided;
  const char *plural = count == 1 ? "" : "es";
  u64 replaced = replacing.replaced;
  StrBufAppendFormat(message, "replaced %lu of %lu match%s", replaced, count, plural);
  if (replacing.within) StrBufAppendString(message, " in the selection");
}

static void
Ask(void);

static void
Chosen([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  char choice = answer ? *answer : 'q';
  Range match = replacing.match;

  switch (choice) {
    case 'y': {
      const StrBuf *line = BufferLine(&Current()->buffer, match.start.line);
      PatternMatch hit = {match.start.byte, match.end.byte};
      StrBuf with = {};
      PatternExpand(&last, StrBufText(line), line->length, hit, &replacing.text, &with);
      Apply(match, StrBufText(&with), with.length);
      StrBufFree(&with);
      replacing.replaced++;
      replacing.decided++;
      break;
    }
    case 'n':
      replacing.next = match.end;
      replacing.decided++;
      break;
    case 'a':
      ReplaceRest(match);
      FinishReplace();
      return;
    default:
      FinishReplace();
      return;
  }

  Ask();
}

// selects the next match and asks what to do with it, or finishes.
static void
Ask(void)
{
  if (!NextMatch(&replacing.match)) {
    FinishReplace();
    return;
  }

  replacing.asked++;
  Select(replacing.match);
  PromptChoose("replace? (y)es, (n)o, (a)ll, (q)uit", "ynaq", Chosen, nullptr);
}

static void
ReplacementAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  prompt = SearchPromptNone;
  if (!answer) return;

  HistoryAdd(&replacements, answer, length);
  StrBufClear(&replacing.text);
  StrBufAppend(&replacing.text, answer, length);

  // every replacement until the run ends is one undo step.
  Document *document = Current();
  View *view = document->view;
  UndoBreak(&document->undo);
  UndoBegin(&document->undo, UndoKindOther, &view->cursors);

  // a selection of several lines is already the whole run, so it starts wrapped.
  Range selected = CursorRange(CursorSetPrimary(&view->cursors));
  replacing.within = selected.start.line != selected.end.line;
  replacing.next = selected.start;
  replacing.stop = replacing.within ? selected.end : selected.start;
  replacing.wrapped = replacing.within;
  replacing.asked = 0;
  replacing.decided = 0;
  replacing.replaced = 0;
  Ask();
}

static void
PatternAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  prompt = SearchPromptNone;
  if (!answer || !length) return;

  SetPattern(answer, length);
  HistoryAdd(&patterns, answer, length);
  Current()->marks = MatchKindSearch;

  if (last.impossible) {
    MatchSet set = SearchMarks(Current());
    ReportNoMatch(&set);
    return;
  }

  StrBuf label = {};
  StrBufAppendFormat(&label, "replace %s with: ", StrBufText(&last.source));
  prompt = SearchPromptReplacement;
  recalled = 0;
  PromptAsk(label.data, "", ReplacementAnswered, nullptr);
  StrBufFree(&label);
}

void
SearchReplace(void)
{
  StrBuf selected = {};
  SelectedSource(&selected);
  CursorSetCollapse(&Current()->view->cursors);
  prompt = SearchPromptPattern;
  recalled = 0;
  PromptAsk("replace: ", StrBufText(&selected), PatternAnswered, nullptr);
  PromptHandle(Relabel, nullptr);
  Label();
  StrBufFree(&selected);
}

static void
ProjectAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  prompt = SearchPromptNone;
  if (!answer || !length) return;

  SetPattern(answer, length);
  HistoryAdd(&patterns, answer, length);
  projectDone(&last);
}

void
SearchProject(SearchProjectDone done)
{
  StrBuf selected = {};
  SelectedSource(&selected);
  projectDone = done;
  prompt = SearchPromptProject;
  recalled = 0;
  PromptAsk("search files: ", StrBufText(&selected), ProjectAnswered, nullptr);
  PromptHandle(Relabel, nullptr);
  Label();
  StrBufFree(&selected);
}

static void
ProjectReplacementAnswered([[maybe_unused]] void *context, const char *answer, u64 length)
{
  prompt = SearchPromptNone;

  if (!answer) {
    replacementDone(nullptr);
    return;
  }

  StrBufClear(&lastReplacement);
  StrBufAppend(&lastReplacement, answer, length);
  HistoryAdd(&replacements, answer, length);
  replacementDone(&lastReplacement);
}

void
SearchReplacement(const Pattern *pattern, SearchReplacementDone done)
{
  replacementDone = done;
  StrBuf label = {};
  StrBufAppendFormat(&label, "replace %s with: ", StrBufText(&pattern->source));
  prompt = SearchPromptProjectReplacement;
  recalled = 0;
  PromptAsk(label.data, "", ProjectReplacementAnswered, nullptr);
  StrBufFree(&label);
}

void
SearchShow(Document *document, const Pattern *pattern)
{
  const StrBuf *source = &pattern->source;
  PatternSet(&last, StrBufText(source), source->length, pattern->options);
  document->marks = MatchKindSearch;
}

bool
SearchActive(void)
{
  return prompt != SearchPromptNone && PromptActive();
}

// fills an empty prompt with the newest pattern or replacement, or one holding an entry it
// was filled with, unchanged, with the one before it (older) or after it, emptying it after
// the newest; false, leaving the prompt alone, when it holds anything else.
static bool
Recall(bool older)
{
  bool replacement = prompt == SearchPromptReplacement ||
    prompt == SearchPromptProjectReplacement;
  const History *history = replacement ? &replacements : &patterns;
  const StrBuf *text = PromptText();

  if (text->length) {
    const StrBuf *entry = recalled ? &history->entries[recalled - 1] : nullptr;
    bool same = entry && entry->length == text->length &&
      memcmp(entry->data, text->data, text->length) == 0;
    if (!same) return false;
  }

  u32 next = !text->length ? 1 : older ? recalled + 1 : recalled - 1;
  if (next > history->count) return true;

  recalled = next;
  const StrBuf *entry = next ? &history->entries[next - 1] : nullptr;
  if (entry) PromptSetText(entry->data, entry->length);
  else PromptSetText("", 0);
  return true;
}

// switches case matching or regular expressions, searching again in find.
static void
Toggle(bool *option)
{
  *option = !*option;

  if (prompt == SearchPromptFind) {
    const StrBuf *text = PromptText();
    SetPattern(StrBufText(text), text->length);
    ShowFirst();
    return;
  }

  Label();
}

void
SearchKey(Key key)
{
  bool plain = key.modifiers == 0;
  bool control = key.modifiers == KeyModifierControl;
  bool up = (plain && key.code == KeyUp) || (control && key.code == 'p');
  bool down = (plain && key.code == KeyDown) || (control && key.code == 'n');

  if ((up || down) && Recall(up)) return;

  if (up || down) {
    if (prompt == SearchPromptFind) FindStep(up);
    return;
  }

  bool replacement = prompt == SearchPromptReplacement ||
    prompt == SearchPromptProjectReplacement;

  if (control && key.code == 't' && !replacement) {
    Toggle(&caseSensitive);
    return;
  }

  if (control && key.code == 'r' && !replacement) {
    Toggle(&regex);
    return;
  }

  PromptKey(key);
}

bool
SearchClear(void)
{
  Document *document = Current();
  if (document->marks == MatchKindNone) return false;

  document->marks = MatchKindNone;
  return true;
}
