#include "search.h"

#include "move.h"
#include "prompt.h"
#include "utf8.h"

// which search prompt is open.
enum SearchPrompt {
  SearchPromptNone,
  SearchPromptFind,
  SearchPromptPattern,
  SearchPromptReplacement
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
// moves with the replacements made before it.
typedef struct Replacing {
  StrBuf text;
  Range match;
  Position next;
  Position stop;
  bool wrapped;
  u64 asked;
  u64 decided;
  u64 replaced;
} Replacing;

static Workspace *workspace;
static StrBuf *message;

// the last search, and whether a new one matches case (ctrl+t), for the whole session.
static Pattern last;
static bool caseSensitive;
static StrBuf lastReplacement;

static enum SearchPrompt prompt;
static SearchSaved saved;
static Replacing replacing;

// where incremental find starts, and the match it shows.
static Position origin;
static bool found;
static bool wrapped;
static Range current;

void
SearchInit(Workspace *newWorkspace, StrBuf *newMessage)
{
  workspace = newWorkspace;
  message = newMessage;
}

static Document *
Current(void)
{
  return WorkspaceDocument(workspace);
}

static const char *
TextOf(const StrBuf *text)
{
  return text->data ? text->data : "";
}

MatchSet
SearchMarks(const Document *document)
{
  return (MatchSet){document->marks, &last};
}

static void
SetPattern(const char *source, u64 length)
{
  PatternSet(&last, source, length, (PatternOptions){.caseSensitive = caseSensitive});
}

static void
Select(Range range)
{
  MoveSelect(&Current()->view, range.start, range.end);
}

static void
ReportNoMatch(const MatchSet *set)
{
  if (set->kind == MatchKindSpell) {
    StrBufAppendString(message, "no unknown words");
  } else if (last.impossible && last.text.length) {
    StrBufAppendString(message, "a match cannot cross a line break");
  } else {
    StrBufAppendFormat(message, "no match for %s", TextOf(&last.source));
  }
}

// labels the open prompt with a verb and, in brackets, the state of the search.
static void
Label(const char *verb)
{
  const char *notes[3];
  u32 count = 0;
  bool typed = PromptText()->length;
  if (caseSensitive) notes[count++] = "match case";
  if (prompt == SearchPromptFind && typed && !found) notes[count++] = "no match";
  if (prompt == SearchPromptFind && found && wrapped) notes[count++] = "wrapped";

  StrBuf label = {};
  StrBufAppendString(&label, verb);

  for (u32 index = 0; index < count; index++) {
    StrBufAppendString(&label, index ? ", " : " [");
    StrBufAppendString(&label, notes[index]);
  }

  StrBufAppendString(&label, count ? "]: " : ": ");
  PromptSetLabel(label.data);
  StrBufFree(&label);
}

static void
RestoreView(void)
{
  View *view = &Current()->view;
  CursorSetCopy(&view->cursors, &saved.cursors);
  view->top = saved.top;
  view->leftColumn = saved.leftColumn;
}

// shows the first match from where find started, or the view as it was without one.
static void
ShowFirst(void)
{
  MatchSet set = {MatchKindSearch, &last};
  found = MatchFind(&set, &Current()->buffer, origin, false, &current, &wrapped);
  if (found) Select(current);
  else RestoreView();
  Label("find");
}

static void
FindChanged([[maybe_unused]] void *context, const char *text, u64 length)
{
  SetPattern(text, length);
  ShowFirst();
}

static void
FindStep(bool backward)
{
  if (!found) return;

  MatchSet set = {MatchKindSearch, &last};
  Position from = backward ? current.start : current.end;
  MatchFind(&set, &Current()->buffer, from, backward, &current, &wrapped);
  Select(current);
  Label("find");
}

static void
FindDone([[maybe_unused]] void *context, const char *answer, u64 length)
{
  prompt = SearchPromptNone;

  // escape, or enter with nothing typed, puts everything back.
  if (!answer || !length) {
    RestoreView();
    Current()->marks = saved.marks;
    PatternSet(&last, TextOf(&saved.source), saved.source.length, saved.options);
    return;
  }

  MatchSet set = SearchMarks(Current());
  if (!found) ReportNoMatch(&set);
}

void
SearchFind(void)
{
  Document *document = Current();
  View *view = &document->view;
  CursorSetCopy(&saved.cursors, &view->cursors);
  saved.top = view->top;
  saved.leftColumn = view->leftColumn;
  saved.marks = document->marks;
  StrBufClear(&saved.source);
  StrBufAppend(&saved.source, TextOf(&last.source), last.source.length);
  saved.options = last.options;

  CursorSetCollapse(&view->cursors);
  origin = CursorSetPrimary(&view->cursors)->head;
  found = false;
  wrapped = false;
  document->marks = MatchKindSearch;
  SetPattern("", 0);

  prompt = SearchPromptFind;
  PromptAsk("find: ", "", FindDone, nullptr);
  PromptHandle(FindChanged, nullptr);
  Label("find");
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

  View *view = &document->view;
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
  if (around && same) StrBufAppendString(message, "this is the only match");
  else if (around) StrBufAppendString(message, wrappedTo);
}

static bool
IsWordAt(const StrBuf *line, u64 at)
{
  if (at >= line->length) return false;

  u32 codepoint;
  Utf8Decode(line->data + at, line->length - at, &codepoint);
  return Utf8IsWord(codepoint);
}

// the word at or just before the cursor.
static bool
WordAround(const StrBuf *line, u64 at, u64 *start, u64 *end)
{
  *start = at;
  *end = at;

  while (*start > 0 && IsWordAt(line, Utf8PreviousStart(line->data, *start))) {
    *start = Utf8PreviousStart(line->data, *start);
  }

  while (IsWordAt(line, *end)) {
    u32 codepoint;
    *end += Utf8Decode(line->data + *end, line->length - *end, &codepoint);
  }

  return *end > *start;
}

void
SearchWord(void)
{
  Document *document = Current();
  View *view = &document->view;
  CursorSetCollapse(&view->cursors);
  Position head = CursorSetPrimary(&view->cursors)->head;
  const StrBuf *line = BufferLine(&document->buffer, head.line);
  u64 start;
  u64 end;

  if (!WordAround(line, head.byte, &start, &end)) {
    StrBufAppendString(message, "no word under the cursor");
    return;
  }

  PatternOptions options = {.caseSensitive = true, .wholeWord = true};
  PatternSet(&last, line->data + start, end - start, options);
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
  Document *document = Current();
  document->marks = MatchKindSpell;
  MatchSet set = SearchMarks(document);
  u64 count = MatchCount(&set, &document->buffer);

  if (!count) {
    document->marks = MatchKindNone;
    ReportNoMatch(&set);
    return;
  }

  View *view = &document->view;
  CursorSetCollapse(&view->cursors);
  Position from = CursorSetPrimary(&view->cursors)->head;
  Range match;
  bool around;
  MatchFind(&set, &document->buffer, from, false, &match, &around);
  Select(match);
  StrBufAppendFormat(message, "%lu unknown word%s", count, count == 1 ? "" : "s");
}

// the next match of the replace, or false once it has come round to where it started.
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
      if (!PatternFind(&last, text->data, text->length, from, &hit)) continue;

      bool pastStop = replacing.wrapped && line == replacing.stop.line;
      if (pastStop && hit.start >= replacing.stop.byte) return false;

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
  const char *replacement = replacing.text.data;
  u64 replacementLength = replacing.text.length;

  PatternMatch hit = {match.start.byte, match.end.byte};
  u64 copied = hit.start;
  StrBufClear(rebuilt);

  do {
    StrBufAppend(rebuilt, line->data + copied, hit.start - copied);
    StrBufAppend(rebuilt, replacement ? replacement : "", replacementLength);
    copied = hit.end;
    replacing.replaced++;
    replacing.decided++;
  } while (PatternFind(&last, line->data, line->length, copied, &hit) && hit.start < limit);

  Range range = {match.start, {lineIndex, copied}};
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
  UndoEnd(&document->undo, &document->view.cursors);

  if (!replacing.asked) {
    MatchSet set = SearchMarks(document);
    ReportNoMatch(&set);
    return;
  }

  u64 count = replacing.decided;
  const char *plural = count == 1 ? "" : "es";
  u64 replaced = replacing.replaced;
  StrBufAppendFormat(message, "replaced %lu of %lu match%s", replaced, count, plural);
}

static void Ask(void);

static void
Chosen([[maybe_unused]] void *context, const char *answer, [[maybe_unused]] u64 length)
{
  char choice = answer ? *answer : 'q';
  Range match = replacing.match;
  const char *text = replacing.text.data ? replacing.text.data : "";

  switch (choice) {
  case 'y':
    Apply(match, text, replacing.text.length);
    replacing.replaced++;
    replacing.decided++;
    break;
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

  StrBufClear(&lastReplacement);
  StrBufAppend(&lastReplacement, answer, length);
  StrBufClear(&replacing.text);
  PatternUnescape(answer, length, &replacing.text);

  // every replacement until the run ends is one undo step.
  Document *document = Current();
  View *view = &document->view;
  UndoBreak(&document->undo);
  UndoBegin(&document->undo, UndoKindOther, &view->cursors);

  Position start = CursorRange(CursorSetPrimary(&view->cursors)).start;
  replacing.next = start;
  replacing.stop = start;
  replacing.wrapped = false;
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
  Current()->marks = MatchKindSearch;

  if (last.impossible) {
    MatchSet set = SearchMarks(Current());
    ReportNoMatch(&set);
    return;
  }

  StrBuf label = {};
  StrBufAppendFormat(&label, "replace %s with: ", TextOf(&last.source));
  prompt = SearchPromptReplacement;
  PromptAsk(label.data, "", ReplacementAnswered, nullptr);
  StrBufFree(&label);
}

void
SearchReplace(void)
{
  CursorSetCollapse(&Current()->view.cursors);
  prompt = SearchPromptPattern;
  PromptAsk("replace: ", "", PatternAnswered, nullptr);
  Label("replace");
}

bool
SearchActive(void)
{
  return prompt != SearchPromptNone && PromptActive();
}

// fills an empty prompt with the last pattern or replacement.
static void
Recall(void)
{
  const StrBuf *text = &last.source;
  if (prompt == SearchPromptFind) text = &saved.source;
  if (prompt == SearchPromptReplacement) text = &lastReplacement;
  if (text->length) PromptSetText(text->data, text->length);
}

static void
ToggleCase(void)
{
  caseSensitive = !caseSensitive;

  if (prompt == SearchPromptFind) {
    const StrBuf *text = PromptText();
    SetPattern(text->data ? text->data : "", text->length);
    ShowFirst();
    return;
  }

  Label("replace");
}

void
SearchKey(Key key)
{
  bool plain = key.modifiers == 0;
  bool control = key.modifiers == KeyModifierControl;
  bool up = (plain && key.code == KeyUp) || (control && key.code == 'p');
  bool down = (plain && key.code == KeyDown) || (control && key.code == 'n');

  if ((up || down) && !PromptText()->length) {
    Recall();
    return;
  }

  if (up || down) {
    if (prompt == SearchPromptFind) FindStep(up);
    return;
  }

  if (control && key.code == 't' && prompt != SearchPromptReplacement) {
    ToggleCase();
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
