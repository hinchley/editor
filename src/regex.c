#include "regex.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "utf8.h"

// the most instructions a program may hold, which bounds the time a search takes.
static constexpr u32 maxInstructions = 4096;

// the largest count a repetition may give.
static constexpr u32 maxRepeat = 1000;

// a repetition with no upper bound.
static constexpr u32 unbounded = UINT32_MAX;

// the deepest groups may nest, since the parser recurses once per level.
static constexpr u32 maxDepth = 1000;

enum RegexOp : u8 {
  // consumes one code point: value (case folded when ignoring case), any, or one in the
  // class value indexes.
  RegexOpChar,
  RegexOpAny,
  RegexOpClass,

  // goes on at x and, with lower priority, at y.
  RegexOpSplit,
  RegexOpJump,

  // records the position in slot value: twice the group, plus 1 for its end.
  RegexOpSave,

  // goes on only where the enum RegexAssert in value holds.
  RegexOpAssert,
  RegexOpMatch
};

enum RegexAssert : u8 {
  RegexAssertLineStart,
  RegexAssertLineEnd,
  RegexAssertWordBoundary,
  RegexAssertNotWordBoundary
};

// jump targets are relative to the instruction, so a block of code can be moved or
// copied as it is.
struct RegexInstruction {
  enum RegexOp op;
  u32 value;
  i32 x;
  i32 y;
};

struct RegexRange {
  u32 low;
  u32 high;
};

// a class is its ranges in regex->ranges, sorted and not touching.
struct RegexClass {
  u32 first;
  u32 count;
  bool negated;
};

typedef struct RangeList {
  RegexRange *ranges;
  u32 count;
  u32 capacity;
} RangeList;

typedef struct Parser {
  Regex *regex;
  const char *source;
  u64 length;
  u64 at;
  u32 groups;
  u32 depth;
  StrBuf *error;
  bool failed;
} Parser;

static const RegexRange digitRanges[] = {{'0', '9'}};
static const RegexRange wordRanges[] =
  {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}, {0x80, 0x10FFFF}};
static const RegexRange spaceRanges[] = {{'\t', '\t'}, {'\v', '\r'}, {' ', ' '}};

static void
Fail(Parser *parser, const char *reason)
{
  if (!parser->failed) StrBufAppendString(parser->error, reason);
  parser->failed = true;
}

static u32
Emit(Parser *parser, RegexInstruction instruction)
{
  Regex *regex = parser->regex;
  if (regex->count == maxInstructions) Fail(parser, "pattern too complex");

  if (regex->count == regex->capacity) {
    regex->capacity = regex->capacity ? regex->capacity * 2 : 32;
    regex->program = MemoryResize(regex->program, regex->capacity, sizeof(RegexInstruction));
  }

  regex->program[regex->count] = instruction;
  return regex->count++;
}

// puts instruction at index, moving the code from there along.
static void
Insert(Parser *parser, u32 index, RegexInstruction instruction)
{
  Emit(parser, instruction);
  RegexInstruction *program = parser->regex->program;
  u32 moved = parser->regex->count - 1 - index;
  memmove(program + index + 1, program + index, moved * sizeof(RegexInstruction));
  program[index] = instruction;
}

// appends a copy of the code from start to end.
static void
Copy(Parser *parser, u32 start, u32 end)
{
  for (u32 index = start; index < end && !parser->failed; index++) {
    Emit(parser, parser->regex->program[index]);
  }
}

static RegexInstruction
Split(i32 first, i32 second, bool swapped)
{
  if (swapped) return (RegexInstruction){.op = RegexOpSplit, .x = second, .y = first};
  return (RegexInstruction){.op = RegexOpSplit, .x = first, .y = second};
}

static void
AddRange(RangeList *list, u32 low, u32 high)
{
  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 8;
    list->ranges = MemoryResize(list->ranges, list->capacity, sizeof(RegexRange));
  }

  list->ranges[list->count++] = (RegexRange){low, high};
}

// adds the code points outside sorted ranges.
static void
AddComplement(RangeList *list, const RegexRange *ranges, u32 count)
{
  u32 low = 0;

  for (u32 index = 0; index < count; index++) {
    if (ranges[index].low > low) AddRange(list, low, ranges[index].low - 1);
    low = ranges[index].high + 1;
  }

  if (low <= 0x10FFFF) AddRange(list, low, 0x10FFFF);
}

// adds the class named by the letter of an escape (d, w or s, and capitals for their
// complements), or returns false for any other letter.
static bool
AddNamedClass(RangeList *list, char letter)
{
  const RegexRange *ranges;
  u32 count;

  switch (letter | 0x20) {
    case 'd':
      ranges = digitRanges;
      count = sizeof(digitRanges) / sizeof(*digitRanges);
      break;
    case 'w':
      ranges = wordRanges;
      count = sizeof(wordRanges) / sizeof(*wordRanges);
      break;
    case 's':
      ranges = spaceRanges;
      count = sizeof(spaceRanges) / sizeof(*spaceRanges);
      break;
    default:
      return false;
  }

  if (letter >= 'a') {
    for (u32 index = 0; index < count; index++) {
      AddRange(list, ranges[index].low, ranges[index].high);
    }
  } else {
    AddComplement(list, ranges, count);
  }

  return true;
}

// the code point an escape stands for: a control character, or ascii punctuation or a
// space for itself.
static bool
EscapedCodepoint(char letter, u32 *codepoint)
{
  switch (letter) {
    case 't':
      *codepoint = '\t';
      return true;
    case 'r':
      *codepoint = '\r';
      return true;
    case 'n':
      *codepoint = '\n';
      return true;
    case 'f':
      *codepoint = '\f';
      return true;
    case 'v':
      *codepoint = '\v';
      return true;
    default:
      break;
  }

  bool letterOrDigit = ((letter | 0x20) >= 'a' && (letter | 0x20) <= 'z') ||
    (letter >= '0' && letter <= '9');
  if (letter < ' ' || letter > '~' || letterOrDigit) return false;

  *codepoint = (u8)letter;
  return true;
}

static void
UnknownEscape(Parser *parser, char letter)
{
  if (parser->failed) return;

  if (letter >= '1' && letter <= '9') {
    StrBufAppendFormat(parser->error, "no backreferences (\\%c)", letter);
  } else {
    StrBufAppendFormat(parser->error, "unknown escape \\%c", letter);
  }

  parser->failed = true;
}

static i32
CompareRanges(const void *left, const void *right)
{
  const RegexRange *a = left;
  const RegexRange *b = right;
  return (a->low > b->low) - (a->low < b->low);
}

// sorts and merges the ranges of list into a new class, and emits a test for it.
static void
EmitClass(Parser *parser, RangeList *list, bool negated)
{
  Regex *regex = parser->regex;
  qsort(list->ranges, list->count, sizeof(RegexRange), CompareRanges);
  u32 first = regex->rangeCount;

  for (u32 index = 0; index < list->count; index++) {
    RegexRange range = list->ranges[index];
    RegexRange *last = regex->rangeCount > first ? &regex->ranges[regex->rangeCount - 1]
                                                 : nullptr;

    if (last && range.low <= last->high + 1) {
      if (range.high > last->high) last->high = range.high;
      continue;
    }

    regex->ranges = MemoryResize(regex->ranges, regex->rangeCount + 1, sizeof(RegexRange));
    regex->ranges[regex->rangeCount++] = range;
  }

  regex->classes = MemoryResize(regex->classes, regex->classCount + 1, sizeof(RegexClass));
  regex->classes[regex->classCount] = (RegexClass){first, regex->rangeCount - first, negated};
  Emit(parser, (RegexInstruction){.op = RegexOpClass, .value = regex->classCount++});
}

static bool
AtEnd(const Parser *parser)
{
  return parser->at >= parser->length;
}

static char
Peek(const Parser *parser)
{
  return AtEnd(parser) ? '\0' : parser->source[parser->at];
}

static u32
ReadCodepoint(Parser *parser)
{
  u32 codepoint;
  parser->at +=
    Utf8Decode(parser->source + parser->at, parser->length - parser->at, &codepoint);
  return codepoint;
}

static void
EmitChar(Parser *parser, u32 codepoint)
{
  u32 value = parser->regex->ignoreCase ? Utf8Fold(codepoint) : codepoint;
  Emit(parser, (RegexInstruction){.op = RegexOpChar, .value = value});
}

// reads one member of a bracket expression: a code point, returning true, or a named
// class, which goes into list.
static bool
ReadMember(Parser *parser, RangeList *list, u32 *codepoint)
{
  if (Peek(parser) != '\\') {
    *codepoint = ReadCodepoint(parser);
    return true;
  }

  if (parser->at + 1 >= parser->length) {
    Fail(parser, "trailing backslash");
    return false;
  }

  char letter = parser->source[parser->at + 1];
  parser->at += 2;
  if (AddNamedClass(list, letter)) return false;
  if (EscapedCodepoint(letter, codepoint)) return true;

  UnknownEscape(parser, letter);
  return false;
}

static void
ParseClass(Parser *parser)
{
  parser->at++;
  bool negated = Peek(parser) == '^';
  parser->at += negated;
  RangeList list = {};

  for (bool first = true;; first = false) {
    if (AtEnd(parser)) {
      Fail(parser, "missing ]");
      break;
    }

    if (Peek(parser) == ']' && !first) {
      parser->at++;
      break;
    }

    u32 low;
    if (!ReadMember(parser, &list, &low)) {
      if (parser->failed) break;
      continue;
    }

    bool range = Peek(parser) == '-' && parser->at + 1 < parser->length &&
      parser->source[parser->at + 1] != ']';

    // without case, a member's folded form counts too (the text's own forms are tried as
    // it is matched), so [ſ] takes s; a range is left as it is.
    if (!range) {
      AddRange(&list, low, low);
      u32 folded = Utf8Fold(low);
      if (parser->regex->ignoreCase && folded != low) AddRange(&list, folded, folded);
      continue;
    }

    parser->at++;
    u32 high;

    if (!ReadMember(parser, &list, &high) || high < low) {
      Fail(parser, "bad range");
      break;
    }

    AddRange(&list, low, high);
  }

  if (!parser->failed) EmitClass(parser, &list, negated);
  free(list.ranges);
}

// reads a decimal number of at most a few digits, or returns false without moving.
static bool
ReadCount(Parser *parser, u32 *count)
{
  u64 start = parser->at;
  u32 value = 0;

  while (Peek(parser) >= '0' && Peek(parser) <= '9' && parser->at - start < 6) {
    value = value * 10 + (u32)(Peek(parser) - '0');
    parser->at++;
  }

  *count = value;
  return parser->at > start;
}

// reads a repetition: * + ? {m} {m,} {,n} {m,n}.
// a "{" that does not start a well formed one is left as a literal, so nothing moves.
static bool
ReadRepeat(Parser *parser, u32 *minimum, u32 *maximum)
{
  switch (Peek(parser)) {
    case '*':
      *minimum = 0;
      *maximum = unbounded;
      parser->at++;
      return true;
    case '+':
      *minimum = 1;
      *maximum = unbounded;
      parser->at++;
      return true;
    case '?':
      *minimum = 0;
      *maximum = 1;
      parser->at++;
      return true;
    case '{':
      break;
    default:
      return false;
  }

  u64 start = parser->at++;
  bool hasMinimum = ReadCount(parser, minimum);
  if (!hasMinimum) *minimum = 0;
  bool comma = Peek(parser) == ',';
  parser->at += comma;
  bool hasMaximum = comma && ReadCount(parser, maximum);
  if (!comma) *maximum = *minimum;
  else if (!hasMaximum) *maximum = unbounded;

  if (Peek(parser) != '}' || (!hasMinimum && !comma)) {
    parser->at = start;
    return false;
  }

  parser->at++;
  bool tooLarge = *minimum > maxRepeat || (*maximum != unbounded && *maximum > maxRepeat);
  if (tooLarge) Fail(parser, "repeat count over 1000");
  else if (*maximum < *minimum) Fail(parser, "bad repeat count");
  return true;
}

// true when a repetition comes next, without reading it.
static bool
RepeatAhead(Parser *parser)
{
  u64 start = parser->at;
  bool failed = parser->failed;
  u32 minimum;
  u32 maximum;
  bool found = ReadRepeat(parser, &minimum, &maximum);
  parser->at = start;
  parser->failed = failed;
  return found;
}

// repeats the code from start to the end of the program.
// optional copies are nested, so skipping one skips the rest, as a counted loop does.
static void
Repeat(Parser *parser, u32 start, u32 minimum, u32 maximum, bool lazy)
{
  Regex *regex = parser->regex;
  u32 length = regex->count - start;

  if (maximum == 0) {
    regex->count = start;
    return;
  }

  if (minimum == 0 && maximum == unbounded) {
    Insert(parser, start, Split(1, (i32)length + 2, lazy));
    u32 jump = Emit(parser, (RegexInstruction){.op = RegexOpJump});
    regex->program[jump].x = (i32)start - (i32)jump;
    return;
  }

  // the first copy is the code already there.
  u32 lastCopy = start;
  u32 optional = maximum == unbounded ? 0 : maximum - minimum;

  if (minimum == 0) {
    Insert(parser, start, Split(1, 0, lazy));
    optional--;
  }

  for (u32 copy = 1; copy < minimum && !parser->failed; copy++) {
    lastCopy = regex->count;
    Copy(parser, start, start + length);
  }

  if (maximum == unbounded) {
    u32 split = Emit(parser, Split(0, 1, lazy));
    if (lazy) regex->program[split].y = (i32)lastCopy - (i32)split;
    else regex->program[split].x = (i32)lastCopy - (i32)split;
    return;
  }

  u32 block = minimum == 0 ? start + 1 : start;

  for (u32 copy = 0; copy < optional && !parser->failed; copy++) {
    Emit(parser, Split(1, 0, lazy));
    Copy(parser, block, block + length);
  }

  if (parser->failed) return;

  // every split skips to the end.
  u32 end = regex->count;
  u32 first = minimum == 0 ? start : regex->count - optional * (length + 1);

  for (u32 split = first; split < end; split += length + 1) {
    i32 skip = (i32)(end - split);
    if (lazy) regex->program[split].x = skip;
    else regex->program[split].y = skip;
  }
}

static void
ParseAlternation(Parser *parser);

// parses an escape outside brackets, returning false for an anchor, which cannot be
// repeated.
static bool
ParseEscape(Parser *parser)
{
  if (parser->at + 1 >= parser->length) {
    Fail(parser, "trailing backslash");
    return true;
  }

  char letter = parser->source[parser->at + 1];
  parser->at += 2;

  if (letter == 'b' || letter == 'B') {
    u32 kind = letter == 'b' ? RegexAssertWordBoundary : RegexAssertNotWordBoundary;
    Emit(parser, (RegexInstruction){.op = RegexOpAssert, .value = kind});
    return false;
  }

  RangeList list = {};
  u32 codepoint;

  if (AddNamedClass(&list, letter)) EmitClass(parser, &list, false);
  else if (EscapedCodepoint(letter, &codepoint)) EmitChar(parser, codepoint);
  else UnknownEscape(parser, letter);

  free(list.ranges);
  return true;
}

static void
ParseGroup(Parser *parser)
{
  parser->at++;
  bool capture = Peek(parser) != '?';

  if (!capture) {
    if (parser->at + 1 >= parser->length || parser->source[parser->at + 1] != ':') {
      Fail(parser, "only (?: groups are supported");
      return;
    }

    parser->at += 2;
  }

  if (++parser->depth > maxDepth) {
    Fail(parser, "groups nested too deeply");
    return;
  }

  u32 group = capture ? ++parser->groups : 0;
  if (capture) Emit(parser, (RegexInstruction){.op = RegexOpSave, .value = 2 * group});
  ParseAlternation(parser);
  parser->depth--;
  if (parser->failed) return;

  if (Peek(parser) != ')') {
    Fail(parser, "missing )");
    return;
  }

  parser->at++;
  if (capture) Emit(parser, (RegexInstruction){.op = RegexOpSave, .value = 2 * group + 1});
}

// parses one atom, returning false for an anchor, which cannot be repeated.
static bool
ParseAtom(Parser *parser)
{
  switch (Peek(parser)) {
    case '(':
      ParseGroup(parser);
      return true;
    case '[':
      ParseClass(parser);
      return true;
    case '.':
      parser->at++;
      Emit(parser, (RegexInstruction){.op = RegexOpAny});
      return true;
    case '^':
    case '$': {
      u32 kind = Peek(parser) == '^' ? RegexAssertLineStart : RegexAssertLineEnd;
      parser->at++;
      Emit(parser, (RegexInstruction){.op = RegexOpAssert, .value = kind});
      return false;
    }
    case '\\':
      return ParseEscape(parser);
    case '*':
    case '+':
    case '?':
      Fail(parser, "nothing to repeat");
      return true;
    case '{':
      if (RepeatAhead(parser)) {
        Fail(parser, "nothing to repeat");
        return true;
      }
      break;
    default:
      break;
  }

  EmitChar(parser, ReadCodepoint(parser));
  return true;
}

static void
ParseRepeated(Parser *parser)
{
  u32 start = parser->regex->count;
  bool repeatable = ParseAtom(parser);
  u32 minimum;
  u32 maximum;
  if (parser->failed || !ReadRepeat(parser, &minimum, &maximum)) return;

  if (!repeatable) {
    Fail(parser, "nothing to repeat");
    return;
  }

  bool lazy = Peek(parser) == '?';
  parser->at += lazy;

  if (RepeatAhead(parser)) {
    Fail(parser, "multiple repeat");
    return;
  }

  if (!parser->failed) Repeat(parser, start, minimum, maximum, lazy);
}

static void
ParseSequence(Parser *parser)
{
  while (!AtEnd(parser) && Peek(parser) != '|' && Peek(parser) != ')' && !parser->failed) {
    ParseRepeated(parser);
  }
}

// alternatives are chained: each but the last is tried before a split to the next, and
// jumps to the end after it.
// the jumps wait for the end in a list linked through their values (index plus 1).
static void
ParseAlternation(Parser *parser)
{
  Regex *regex = parser->regex;
  u32 start = regex->count;
  u32 pending = 0;
  ParseSequence(parser);

  while (Peek(parser) == '|' && !parser->failed) {
    parser->at++;
    Insert(parser, start, Split(1, (i32)(regex->count - start) + 2, false));
    pending = Emit(parser, (RegexInstruction){.op = RegexOpJump, .value = pending}) + 1;
    start = regex->count;
    ParseSequence(parser);
  }

  while (pending && !parser->failed) {
    u32 jump = pending - 1;
    pending = regex->program[jump].value;
    regex->program[jump].value = 0;
    regex->program[jump].x = (i32)(regex->count - jump);
  }
}

// finds what every match must start with, for searches to skip ahead.
static void
Analyse(Regex *regex)
{
  u32 pc = 0;
  while (regex->program[pc].op == RegexOpSave) pc++;

  const RegexInstruction *first = &regex->program[pc];
  regex->lineStart = first->op == RegexOpAssert && first->value == RegexAssertLineStart;
  regex->firstByte = 0;
  if (first->op != RegexOpChar || first->value >= 0x80) return;

  // the kelvin sign and the long s fold to k and s, so a skip to the ascii letters would
  // pass over them.
  bool foldedFromWide = first->value == 'k' || first->value == 's';
  if (regex->ignoreCase && foldedFromWide) return;

  regex->firstByte = (u8)first->value;
}

bool
RegexCompile(Regex *regex, const char *source, u64 length, bool ignoreCase, StrBuf *error)
{
  regex->count = 0;
  regex->classCount = 0;
  regex->rangeCount = 0;
  regex->ignoreCase = ignoreCase;
  Parser parser = {.regex = regex, .source = source, .length = length, .error = error};
  ParseAlternation(&parser);
  if (!parser.failed && !AtEnd(&parser)) Fail(&parser, "unmatched )");
  Emit(&parser, (RegexInstruction){.op = RegexOpMatch});

  if (parser.failed) {
    regex->count = 0;
    return false;
  }

  Analyse(regex);
  return true;
}

void
RegexFree(Regex *regex)
{
  free(regex->program);
  free(regex->classes);
  free(regex->ranges);
  *regex = (Regex){};
}

// the vm.

// the threads at one position, in priority order, each with its slots.
typedef struct Threads {
  u32 *pcs;
  u64 *slots;
  u32 count;
} Threads;

// a pc still to follow, or (when pc is restoreSlot) a slot to put back once the code
// that set it has been followed.
typedef struct Pending {
  u32 pc;
  u32 slot;
  u64 value;
} Pending;

static constexpr u32 restoreSlot = UINT32_MAX;

// where the vm stands: an offset and the word characters either side of it.
typedef struct Place {
  u64 at;
  bool wordBefore;
  bool wordAfter;
} Place;

// scratch space for searches, sized for the largest program and slot count seen so far.
// the editor searches from one thread, and never runs one search inside another.
static struct {
  Threads lists[2];
  u32 *marks;
  u32 generation;
  Pending *stack;
  u64 *work;
  u64 *best;
  u32 programCapacity;
  u32 slotCapacity;
} scratch;

static void
Reserve(u32 count, u32 slotCount)
{
  if (count <= scratch.programCapacity && slotCount <= scratch.slotCapacity) return;

  u32 programs = count > scratch.programCapacity ? count : scratch.programCapacity;
  u32 slots = slotCount > scratch.slotCapacity ? slotCount : scratch.slotCapacity;

  for (u32 index = 0; index < 2; index++) {
    Threads *list = &scratch.lists[index];
    list->pcs = MemoryResize(list->pcs, programs, sizeof(u32));
    list->slots = MemoryResize(list->slots, (u64)programs * slots, sizeof(u64));
  }

  free(scratch.marks);
  scratch.marks = MemoryAllocateZeroed(programs, sizeof(u32));
  scratch.generation = 0;
  scratch.stack = MemoryResize(scratch.stack, programs + 1, sizeof(Pending));
  scratch.work = MemoryResize(scratch.work, slots, sizeof(u64));
  scratch.best = MemoryResize(scratch.best, slots, sizeof(u64));
  scratch.programCapacity = programs;
  scratch.slotCapacity = slots;
}

// starts a new list of threads, which no pc has been added to yet.
static void
NewGeneration(void)
{
  if (++scratch.generation == UINT32_MAX) {
    memset(scratch.marks, 0, scratch.programCapacity * sizeof(u32));
    scratch.generation = 1;
  }
}

static bool
Holds(u32 kind, const Place *place, u64 length)
{
  switch (kind) {
    case RegexAssertLineStart:
      return place->at == 0;
    case RegexAssertLineEnd:
      return place->at == length;
    case RegexAssertWordBoundary:
      return place->wordBefore != place->wordAfter;
    default:
      return place->wordBefore == place->wordAfter;
  }
}

// adds the threads reached from pc without consuming anything, in priority order, each
// with scratch.work as its slots (updated by the saves on its way).
static void
AddThread(
  const Regex *regex,
  Threads *list,
  u32 start,
  const Place *place,
  u64 length,
  u32 slotCount
)
{
  Pending *stack = scratch.stack;
  u64 *work = scratch.work;
  u32 top = 0;
  stack[top++] = (Pending){.pc = start};

  while (top) {
    Pending pending = stack[--top];

    if (pending.pc == restoreSlot) {
      work[pending.slot] = pending.value;
      continue;
    }

    for (u32 pc = pending.pc; scratch.marks[pc] != scratch.generation;) {
      scratch.marks[pc] = scratch.generation;
      const RegexInstruction *instruction = &regex->program[pc];

      if (instruction->op == RegexOpJump) {
        pc = (u32)((i32)pc + instruction->x);
      } else if (instruction->op == RegexOpSplit) {
        stack[top++] = (Pending){.pc = (u32)((i32)pc + instruction->y)};
        pc = (u32)((i32)pc + instruction->x);
      } else if (instruction->op == RegexOpSave) {
        u32 slot = instruction->value;

        if (slot < slotCount) {
          stack[top++] = (Pending){.pc = restoreSlot, .slot = slot, .value = work[slot]};
          work[slot] = place->at;
        }

        pc++;
      } else if (instruction->op == RegexOpAssert) {
        if (!Holds(instruction->value, place, length)) break;
        pc++;
      } else {
        list->pcs[list->count] = pc;
        memcpy(list->slots + (u64)list->count * slotCount, work, slotCount * sizeof(u64));
        list->count++;
        break;
      }
    }
  }
}

static bool
InRanges(const Regex *regex, const RegexClass *class, u32 codepoint)
{
  const RegexRange *ranges = regex->ranges + class->first;

  for (u32 index = 0; index < class->count && ranges[index].low <= codepoint; index++) {
    if (codepoint <= ranges[index].high) return true;
  }

  return false;
}

static bool
InClass(const Regex *regex, u32 index, u32 codepoint)
{
  const RegexClass *class = &regex->classes[index];
  bool found = InRanges(regex, class, codepoint);

  if (!found && regex->ignoreCase) {
    found = InRanges(regex, class, Utf8Lower(codepoint)) ||
      InRanges(regex, class, Utf8Upper(codepoint)) ||
      InRanges(regex, class, Utf8Fold(codepoint));
  }

  return found != class->negated;
}

static bool
Consumes(const Regex *regex, const RegexInstruction *instruction, u32 codepoint, u32 folded)
{
  switch (instruction->op) {
    case RegexOpChar:
      return instruction->value == (regex->ignoreCase ? folded : codepoint);
    case RegexOpAny:
      return true;
    case RegexOpClass:
      return InClass(regex, instruction->value, codepoint);
    default:
      return false;
  }
}

// the code point at at (and its size, 0 at the end) and the place there.
static Place
PlaceAt(const char *text, u64 length, u64 at, u32 *codepoint, u32 *size)
{
  Place place = {.at = at};
  *codepoint = 0;
  *size = 0;

  if (at > 0) {
    u64 previous = Utf8PreviousStart(text, at);
    u32 before;
    Utf8Decode(text + previous, at - previous, &before);
    place.wordBefore = Utf8IsWord(before);
  }

  if (at < length) {
    *size = Utf8Decode(text + at, length - at, codepoint);
    place.wordAfter = Utf8IsWord(*codepoint);
  }

  return place;
}

// the first offset from at holding the byte every match starts with, or length.
static u64
NextFirst(const Regex *regex, const char *text, u64 length, u64 at)
{
  u8 wanted = regex->firstByte;
  bool letter = wanted >= 'a' && wanted <= 'z';

  if (!regex->ignoreCase || !letter) {
    const char *found = memchr(text + at, wanted, length - at);
    return found ? (u64)(found - text) : length;
  }

  while (at < length && ((u8)text[at] | 0x20) != wanted) at++;
  return at;
}

bool
RegexFind(
  const Regex *regex,
  const char *text,
  u64 length,
  u64 from,
  u64 limit,
  bool anchored,
  RegexSpan *spans,
  u32 spanCount
)
{
  if (!regex->count || from >= length || from >= limit) return false;
  if (regex->lineStart && from > 0) return false;

  u32 slotCount = 2 * spanCount;
  Reserve(regex->count, slotCount);
  Threads *current = &scratch.lists[0];
  Threads *next = &scratch.lists[1];
  current->count = 0;
  NewGeneration();

  u64 at = from;
  u32 codepoint;
  u32 size;
  Place place = PlaceAt(text, length, at, &codepoint, &size);
  bool matched = false;
  anchored = anchored || regex->lineStart;

  for (;;) {
    if (!matched && (!anchored || at == from) && at < limit) {
      if (!current->count && !anchored && regex->firstByte) {
        u64 found = NextFirst(regex, text, length, at);
        if (found == length || found >= limit) break;
        if (found != at) place = PlaceAt(text, length, found, &codepoint, &size);
        at = found;
      }

      for (u32 slot = 0; slot < slotCount; slot++) scratch.work[slot] = regexUnset;
      scratch.work[0] = at;
      AddThread(regex, current, 0, &place, length, slotCount);
    }

    // with no threads left an unanchored search still goes on to the next start.
    if (!current->count && (matched || anchored || at >= limit)) break;

    u64 after = at + size;
    u32 nextCodepoint = 0;
    u32 nextSize = 0;
    Place afterPlace = {.at = after, .wordBefore = size && Utf8IsWord(codepoint)};

    if (after < length) {
      nextSize = Utf8Decode(text + after, length - after, &nextCodepoint);
      afterPlace.wordAfter = Utf8IsWord(nextCodepoint);
    }

    u32 folded = regex->ignoreCase ? Utf8Fold(codepoint) : codepoint;
    NewGeneration();
    next->count = 0;

    for (u32 index = 0; index < current->count; index++) {
      const RegexInstruction *instruction = &regex->program[current->pcs[index]];
      u64 *slots = current->slots + (u64)index * slotCount;

      // a match cuts off the threads of lower priority, but those before it go on in case
      // they match later.
      if (instruction->op == RegexOpMatch) {
        if (at == slots[0]) continue;

        memcpy(scratch.best, slots, slotCount * sizeof(u64));
        scratch.best[1] = at;
        matched = true;
        break;
      }

      if (size && Consumes(regex, instruction, codepoint, folded)) {
        memcpy(scratch.work, slots, slotCount * sizeof(u64));
        AddThread(regex, next, current->pcs[index] + 1, &afterPlace, length, slotCount);
      }
    }

    if (!size) break;

    Threads *swap = current;
    current = next;
    next = swap;
    at = after;
    codepoint = nextCodepoint;
    size = nextSize;
    place = afterPlace;
  }

  if (!matched) return false;

  for (u32 span = 0; span < spanCount; span++) {
    u64 start = scratch.best[2 * span];
    u64 end = scratch.best[2 * span + 1];
    bool set = start != regexUnset && end != regexUnset;
    spans[span] = set ? (RegexSpan){start, end} : (RegexSpan){regexUnset, regexUnset};
  }

  return true;
}
