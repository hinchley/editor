#include "query.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "regex.h"

enum QueryTest {
  QueryTestEq,
  QueryTestMatch,
  QueryTestAnyOf
};

struct QueryPredicate {
  enum QueryTest test;
  bool negate;
  u32 capture;

  // #eq? compares with another capture, or with the first string.
  bool withCapture;
  u32 otherCapture;
  const char **strings;
  u32 stringCount;

  // #match? only.
  Regex regex;
};

// the steps of one predicate: an operator name followed by its arguments.
typedef struct Steps {
  const TSQuery *query;
  const TSQueryPredicateStep *steps;
  u32 count;
} Steps;

static const char *
StepString(const Steps *steps, u32 index)
{
  const TSQueryPredicateStep *step = &steps->steps[index];
  if (step->type != TSQueryPredicateStepTypeString) return nullptr;

  u32 length;
  return ts_query_string_value_for_id(steps->query, step->value_id, &length);
}

static bool
StepIsCapture(const Steps *steps, u32 index)
{
  return steps->steps[index].type == TSQueryPredicateStepTypeCapture;
}

// the members of a lua class letter (%a, %d) inside brackets, as the regex engine reads
// them, or nullptr. classes are ascii, as lua's are in the c locale.
static const char *
LuaClass(char letter)
{
  switch (letter | 0x20) {
    case 'a':
      return "A-Za-z";
    case 'c':
      return "\x01-\x1f\x7f";
    case 'd':
      return "0-9";
    case 'l':
      return "a-z";
    case 'p':
      return "!-/:-@\\[-`{-~";
    case 's':
      return " \\t\\n\\r\\f\\v";
    case 'u':
      return "A-Z";
    case 'w':
      return "A-Za-z0-9";
    case 'x':
      return "0-9A-Fa-f";
    default:
      return nullptr;
  }
}

// rewrites a lua pattern (for #lua-match?) in the regex engine's syntax: %a style classes
// (a capital letter is the complement, outside brackets), %x escapes, and the lazy
// repetition "-", which is "*" for a test that only asks whether it matches.
// characters special to the engine but not to lua ({, }, | and \, ] outside brackets,
// and ^ and $ away from the ends) are escaped.
// %b and %f have no equivalent and match their letter.
static void
ConvertLuaPattern(const char *source, StrBuf *output)
{
  bool inBracket = false;

  for (const char *at = source; *at; at++) {
    if (*at == '%' && at[1]) {
      at++;
      const char *class = LuaClass(*at);
      bool complement = *at >= 'A' && *at <= 'Z';

      if (!class) {
        if (ispunct((u8)*at)) StrBufAppendByte(output, '\\');
        StrBufAppendByte(output, *at);
      } else if (inBracket) {
        StrBufAppendString(output, class);
      } else {
        StrBufAppendFormat(output, "[%s%s]", complement ? "^" : "", class);
      }

      continue;
    }

    if (inBracket) {
      if (*at == ']') inBracket = false;
      if (*at == '\\') StrBufAppendByte(output, '\\');
      StrBufAppendByte(output, *at);
      continue;
    }

    if (*at == '[') {
      inBracket = true;
      StrBufAppendByte(output, *at);
      if (at[1] == '^') StrBufAppendByte(output, *++at);
      if (at[1] == ']') {
        StrBufAppendString(output, "\\]");
        at++;
      }

      continue;
    }

    if (*at == '-') {
      StrBufAppendByte(output, '*');
      continue;
    }

    bool anchor = (*at == '^' && at == source) || (*at == '$' && !at[1]);
    bool special = *at == '{' || *at == '}' || *at == '|' || *at == '\\' || *at == ']';
    if (special || ((*at == '^' || *at == '$') && !anchor)) StrBufAppendByte(output, '\\');
    StrBufAppendByte(output, *at);
  }
}

// compiles #match?'s pattern, written for rust's regular expressions, which the engine's
// syntax follows, or #lua-match?'s, rewritten.
static bool
CompileRegex(QueryPredicate *predicate, const char *source, bool lua, StrBuf *error)
{
  StrBuf converted = {};
  if (lua) ConvertLuaPattern(source, &converted);
  else StrBufAppendString(&converted, source);
  StrBuf reason = {};
  const char *text = StrBufText(&converted);
  bool valid = RegexCompile(&predicate->regex, text, converted.length, false, &reason);
  if (!valid) StrBufAppendFormat(error, "bad regular expression %s: %s", source, reason.data);
  StrBufFree(&converted);
  StrBufFree(&reason);
  return valid;
}

static bool
ParseTest(const char *name, QueryPredicate *predicate, bool *lua)
{
  predicate->negate = strncmp(name, "not-", 4) == 0;
  if (predicate->negate) name += 4;

  *lua = strcmp(name, "lua-match?") == 0;
  if (strcmp(name, "eq?") == 0) predicate->test = QueryTestEq;
  else if (strcmp(name, "match?") == 0 || *lua) predicate->test = QueryTestMatch;
  else if (strcmp(name, "any-of?") == 0) predicate->test = QueryTestAnyOf;
  else return false;

  return true;
}

// reads a text predicate whose first argument is a capture.
static bool
ParsePredicate(const Steps *steps, QueryPattern *pattern, StrBuf *error)
{
  QueryPredicate predicate = {};
  bool lua;
  if (!ParseTest(StepString(steps, 0), &predicate, &lua)) return true;

  if (steps->count < 3 || !StepIsCapture(steps, 1)) {
    StrBufAppendFormat(error, "bad #%s predicate", StepString(steps, 0));
    return false;
  }

  predicate.capture = steps->steps[1].value_id;
  predicate.withCapture = predicate.test == QueryTestEq && StepIsCapture(steps, 2);
  predicate.otherCapture = steps->steps[2].value_id;
  predicate.strings = MemoryAllocate(steps->count * sizeof(const char *));

  for (u32 index = 2; index < steps->count; index++) {
    const char *string = StepString(steps, index);
    if (string) predicate.strings[predicate.stringCount++] = string;
  }

  bool valid = predicate.withCapture || predicate.stringCount > 0;
  if (!valid) StrBufAppendFormat(error, "bad #%s predicate", StepString(steps, 0));

  if (valid && predicate.test == QueryTestMatch) {
    valid = CompileRegex(&predicate, predicate.strings[0], lua, error);
  }

  if (!valid) {
    free(predicate.strings);
    return false;
  }

  u32 count = pattern->predicateCount + 1;
  pattern->predicates = MemoryResize(pattern->predicates, count, sizeof(QueryPredicate));
  pattern->predicates[pattern->predicateCount++] = predicate;
  return true;
}

// reads the injection settings made with #set!.
static void
ParseSetting(const Steps *steps, QueryPattern *pattern)
{
  const char *key = steps->count > 1 ? StepString(steps, 1) : nullptr;
  const char *value = steps->count > 2 ? StepString(steps, 2) : nullptr;
  if (!key) return;

  if (strcmp(key, "injection.language") == 0 && value) pattern->injectionLanguage = value;
  if (strcmp(key, "injection.include-children") == 0) pattern->includeChildren = true;
}

static bool
ParsePattern(const TSQuery *tsQuery, u32 index, QueryPattern *pattern, StrBuf *error)
{
  u32 stepCount;
  const TSQueryPredicateStep *all;
  all = ts_query_predicates_for_pattern(tsQuery, index, &stepCount);
  u32 start = 0;

  for (u32 at = 0; at < stepCount; at++) {
    if (all[at].type != TSQueryPredicateStepTypeDone) continue;

    Steps steps = {tsQuery, all + start, at - start};
    start = at + 1;
    const char *name = steps.count ? StepString(&steps, 0) : nullptr;
    if (!name) continue;

    if (strcmp(name, "set!") == 0) ParseSetting(&steps, pattern);
    else if (!ParsePredicate(&steps, pattern, error)) return false;
  }

  return true;
}

static const char *
ErrorName(TSQueryError error)
{
  switch (error) {
    case TSQueryErrorSyntax:
      return "syntax error";
    case TSQueryErrorNodeType:
      return "unknown node type";
    case TSQueryErrorField:
      return "unknown field";
    case TSQueryErrorCapture:
      return "unknown capture";
    case TSQueryErrorStructure:
      return "impossible pattern";
    case TSQueryErrorLanguage:
      return "incompatible language";
    case TSQueryErrorNone:
      break;
  }

  return "error";
}

bool
QueryInit(
  Query *query,
  const TSLanguage *language,
  const u8 *source,
  u64 length,
  StrBuf *error
)
{
  *query = (Query){};
  u32 offset;
  TSQueryError status;
  query->query = ts_query_new(language, (const char *)source, (u32)length, &offset, &status);

  if (!query->query) {
    StrBufAppendFormat(error, "%s at byte %u", ErrorName(status), offset);
    return false;
  }

  query->patternCount = ts_query_pattern_count(query->query);
  query->patterns = MemoryAllocateZeroed(query->patternCount, sizeof(QueryPattern));

  for (u32 index = 0; index < query->patternCount; index++) {
    if (ParsePattern(query->query, index, &query->patterns[index], error)) continue;

    QueryFree(query);
    return false;
  }

  return true;
}

void
QueryFree(Query *query)
{
  for (u32 index = 0; index < query->patternCount; index++) {
    QueryPattern *pattern = &query->patterns[index];

    for (u32 at = 0; at < pattern->predicateCount; at++) {
      QueryPredicate *predicate = &pattern->predicates[at];
      if (predicate->test == QueryTestMatch) RegexFree(&predicate->regex);
      free(predicate->strings);
    }

    free(pattern->predicates);
  }

  free(query->patterns);
  if (query->query) ts_query_delete(query->query);
  *query = (Query){};
}

// the text of the first node captured with an index, or false when there is none.
static bool
CaptureText(
  const TSQueryMatch *match,
  u32 capture,
  QueryNodeText text,
  void *context,
  StrBuf *output
)
{
  for (u32 index = 0; index < match->capture_count; index++) {
    if (match->captures[index].index != capture) continue;

    StrBufClear(output);
    text(context, match->captures[index].node, output);
    return true;
  }

  return false;
}

// true when regex matches somewhere in text.
static bool
Matches(const Regex *regex, const StrBuf *text)
{
  RegexSpan span;
  return RegexFind(regex, StrBufText(text), text->length, 0, text->length, false, &span, 1);
}

static bool
Passes(
  const QueryPredicate *predicate,
  const TSQueryMatch *match,
  QueryNodeText text,
  void *context
)
{
  StrBuf value = {};
  StrBuf other = {};
  bool result = true;

  // a predicate on a capture the match lacks (an optional one) does not apply.
  if (!CaptureText(match, predicate->capture, text, context, &value)) return true;

  switch (predicate->test) {
    case QueryTestEq:
      if (predicate->withCapture) {
        CaptureText(match, predicate->otherCapture, text, context, &other);
        result = strcmp(StrBufText(&value), StrBufText(&other)) == 0;
      } else {
        result = strcmp(StrBufText(&value), predicate->strings[0]) == 0;
      }

      break;
    case QueryTestMatch:
      result = Matches(&predicate->regex, &value);
      break;
    case QueryTestAnyOf:
      result = false;

      for (u32 index = 0; index < predicate->stringCount && !result; index++) {
        result = strcmp(StrBufText(&value), predicate->strings[index]) == 0;
      }

      break;
  }

  StrBufFree(&value);
  StrBufFree(&other);
  return result != predicate->negate;
}

bool
QueryAccepts(const Query *query, const TSQueryMatch *match, QueryNodeText text, void *context)
{
  const QueryPattern *pattern = &query->patterns[match->pattern_index];

  for (u32 index = 0; index < pattern->predicateCount; index++) {
    if (!Passes(&pattern->predicates[index], match, text, context)) return false;
  }

  return true;
}

u32
QueryCapture(const Query *query, const char *name)
{
  u32 count = ts_query_capture_count(query->query);

  for (u32 index = 0; index < count; index++) {
    u32 length;
    const char *capture = ts_query_capture_name_for_id(query->query, index, &length);
    if (strlen(name) == length && strncmp(capture, name, length) == 0) return index;
  }

  return UINT32_MAX;
}
