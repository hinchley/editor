#include "highlight.h"

#include <dlfcn.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tree_sitter/api.h>

#include "file.h"
#include "lang.h"
#include "memory.h"
#include "query.h"
#include "settings.h"

// a byte's group sits in the low bits.
// the literal bit marks code inside prose (fenced code, code spans, urls), which spell
// check passes over whatever colours it, and the special bit marks a string that is not
// text (a css colour, a regular expression, a json key).
static constexpr u8 literalBit = 0x80;
static constexpr u8 specialBit = 0x40;

// a capture with no group colours nothing.
static constexpr u8 noGroup = 0xFF;

// lines coloured at once around the line asked for.
static constexpr u64 windowBefore = 16;
static constexpr u64 windowAfter = 96;

// a line longer than longLineSize is coloured only around the byte asked for, from a
// quarter of longLineReach before it to longLineReach after it, so the cost of colouring
// does not grow with the length of the line.
static constexpr u64 longLineSize = 16384;
static constexpr u64 longLineReach = 65536;

// how deep languages may nest (markdown, its inline text, html inside that, a script).
static constexpr u32 maxDepth = 4;

// injected layers kept when they are not in sight.
static constexpr u32 maxLayers = 512;

// a language's grammar and queries, loaded and compiled on first use and shared by every
// highlighter.
typedef struct Compiled {
  // the grammar from parser.so, or nullptr with why it could not be loaded in failure
  // (left empty for a package without parser.so, which is not a problem).
  bool loadTried;
  const TSLanguage *language;
  StrBuf failure;

  bool tried;
  bool ready;
  Query highlights;
  Query injections;

  // the group of each capture of the highlight query (with specialBit), or noGroup.
  u8 *groups;
  u32 contentCapture;
  u32 languageCapture;

  // the tags query, compiled on its own the first time symbols are asked for.
  // kinds holds the kind of each @definition.<kind> capture, or nullptr.
  bool tagsTried;
  bool tagsReady;
  Query tags;
  const char **kinds;
  u32 nameCapture;
  u32 bodyCapture;
} Compiled;

// one for each language, by Lang.index.
static Compiled *compiled;

// a language injected over some ranges of the buffer.
typedef struct Layer {
  u32 grammar;
  TSTree *tree;
  TSRange *ranges;
  u32 rangeCount;

  // edited since it was parsed, and when it was last coloured.
  bool dirty;
  u64 used;
} Layer;

// the bytes of a line that a window colours: all of a short line, part of a long one.
typedef struct Span {
  u64 from;
  u64 to;
} Span;

// the groups of count lines from first, one byte per text byte.
// the groups of line first + index start at bytes + starts[index], and only the bytes in
// spans[index] are coloured.
typedef struct Window {
  bool valid;
  u64 first;
  u64 count;
  u64 *starts;
  Span *spans;
  u8 *bytes;
  u64 used;
} Window;

// part of a window that one query run colours: a run of short lines, or the span of a
// long line.
typedef struct Segment {
  TSPoint start;
  TSPoint end;
} Segment;

struct Highlight {
  Buffer *buffer;
  u32 grammar;
  TSParser *parser;

  // the whole buffer's tree, and whether it was edited since it was parsed.
  TSTree *tree;
  bool dirty;

  Layer *layers;
  u32 layerCount;
  u32 layerCapacity;

  Window windows[2];

  // counts window computations, to tell which layers and windows were used last.
  u64 serial;

  // the byte offset of each line start, known for the first offsetCount lines.
  u64 *offsets;
  u64 offsetCount;
  u64 offsetCapacity;

  // highlighting is off while the buffer is too large, and once a parse took too long
  // until the text is reloaded.
  u64 size;
  bool tooLarge;
  bool tooSlow;
};

// one capture to paint.
typedef struct Paint {
  TSPoint start;
  TSPoint end;
  u8 group;
  u32 pattern;
  u32 order;
} Paint;

typedef struct PaintList {
  Paint *paints;
  u32 count;
  u32 capacity;
} PaintList;

// an injection found in the window: a grammar over some ranges.
typedef struct Site {
  u32 grammar;
  TSRange *ranges;
  u32 rangeCount;
} Site;

typedef struct SiteList {
  Site *sites;
  u32 count;
  u32 capacity;
} SiteList;

static void
CompileGroups(Compiled *grammar)
{
  TSQuery *query = grammar->highlights.query;
  u32 count = ts_query_capture_count(query);
  grammar->groups = MemoryAllocate(count ? count : 1);

  for (u32 index = 0; index < count; index++) {
    u32 length;
    const char *name = ts_query_capture_name_for_id(query, index, &length);
    enum ThemeGroup group;
    bool special = strncmp(name, "string.special", 14) == 0;
    u8 bits = special ? specialBit : 0;
    grammar->groups[index] = ThemeOfCapture(name, &group) ? (u8)group | bits : noGroup;
  }
}

// makes room for the state of every language, before any is used.
static void
Prepare(void)
{
  if (!compiled) {
    compiled = MemoryAllocateZeroed(LangCount() ? LangCount() : 1, sizeof(Compiled));
  }
}

// loads a language's grammar from its parser.so the first time it is asked for.
// the handle is never closed, as trees and queries point into the grammar.
static const TSLanguage *
Load(u32 index)
{
  Prepare();
  Compiled *grammar = &compiled[index];
  if (grammar->loadTried) return grammar->language;

  grammar->loadTried = true;
  const Lang *lang = LangAt(index);
  if (!lang->hasParser) return nullptr;

  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/parser.so", lang->folder);
  void *handle = dlopen(path.data, RTLD_NOW | RTLD_LOCAL);
  StrBufFree(&path);
  StrBuf *failure = &grammar->failure;

  if (!handle) {
    StrBufAppendString(failure, dlerror());
    return nullptr;
  }

  void *symbol = dlsym(handle, lang->symbol);

  if (!symbol) {
    StrBufAppendFormat(failure, "parser.so has no %s", lang->symbol);
    return nullptr;
  }

  // posix allows a function pointer to be read from what dlsym gives.
  const TSLanguage *(*make)(void);
  memcpy(&make, &symbol, sizeof(make));
  const TSLanguage *language = make();
  u32 abi = ts_language_abi_version(language);
  u32 newest = TREE_SITTER_LANGUAGE_VERSION;
  u32 oldest = TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION;

  if (abi < oldest || abi > newest) {
    const char *age = abi > newest ? "newer" : "older";
    StrBufAppendFormat(
      failure,
      "parser.so has ABI %u, %s than the editor supports (%u to %u)",
      abi,
      age,
      oldest,
      newest
    );
    return nullptr;
  }

  grammar->language = language;
  return language;
}

// reads a query file of a language's package into text.
// returns false, saying why in error, when it exists but cannot be read; a missing file
// leaves text empty and sets missing.
static bool
ReadQuery(const Lang *lang, const char *name, StrBuf *text, bool *missing, StrBuf *error)
{
  StrBuf path = {};
  StrBufAppendFormat(&path, "%s/%s", lang->folder, name);
  FileStamp stamp;
  enum FileStatus status = FileRead(path.data, text, &stamp, nullptr);
  StrBufFree(&path);
  *missing = status == FileStatusNew;
  if (status == FileStatusOk || *missing) return true;

  StrBufAppendFormat(error, "%s cannot be read", name);
  return false;
}

// compiles one query file of a language's package, which must exist unless optional.
// error names the file when the query is wrong.
static bool
CompileQuery(
  const Lang *lang,
  const TSLanguage *language,
  const char *name,
  bool optional,
  Query *query,
  StrBuf *error
)
{
  StrBuf text = {};
  bool missing;
  bool ready = ReadQuery(lang, name, &text, &missing, error);

  if (ready && missing && !optional) {
    StrBufAppendFormat(error, "%s is missing", name);
    ready = false;
  }

  if (ready && !missing) {
    StrBuf reason = {};
    ready = QueryInit(query, language, (const u8 *)text.data, text.length, &reason);
    if (!ready) StrBufAppendFormat(error, "%s: %s", name, reason.data ? reason.data : "");
    StrBufFree(&reason);
  }

  StrBufFree(&text);
  return ready;
}

static void
Report(StrBuf *message, const char *name, const char *reason)
{
  if (message->length) StrBufAppendString(message, "; ");
  StrBufAppendFormat(message, "%s highlighting failed: %s", name, reason);
}

// loads a language's grammar and compiles its queries the first time it is used.
static bool
Compile(u32 index, StrBuf *message)
{
  Prepare();
  Compiled *grammar = &compiled[index];
  if (grammar->tried) return grammar->ready;

  grammar->tried = true;
  const Lang *lang = LangAt(index);
  const char *name = lang->name ? lang->name : lang->id;
  const TSLanguage *language = Load(index);

  if (!language) {
    const StrBuf *failure = &grammar->failure;
    if (failure->length) Report(message, name, failure->data);
    return false;
  }

  StrBuf error = {};
  bool ready =
    CompileQuery(lang, language, "highlights.scm", false, &grammar->highlights, &error);

  if (ready) {
    Query *query = &grammar->injections;
    ready = CompileQuery(lang, language, "injections.scm", true, query, &error);

    if (ready && query->query) {
      grammar->contentCapture = QueryCapture(query, "injection.content");
      grammar->languageCapture = QueryCapture(query, "injection.language");
    }
  }

  if (ready) CompileGroups(grammar);
  else QueryFree(&grammar->highlights);

  if (!ready) Report(message, name, error.data);
  StrBufFree(&error);
  grammar->ready = ready;
  return ready;
}

// the byte offset of a line start, extending the known offsets as far as it.
static u64
LineOffset(Highlight *highlight, u64 line)
{
  if (line + 1 > highlight->offsetCapacity) {
    u64 capacity = highlight->offsetCapacity ? highlight->offsetCapacity : 1024;
    while (capacity < line + 1) capacity *= 2;
    highlight->offsets = MemoryResize(highlight->offsets, capacity, sizeof(u64));
    highlight->offsetCapacity = capacity;
  }

  u64 *offsets = highlight->offsets;
  const Buffer *buffer = highlight->buffer;

  while (highlight->offsetCount <= line) {
    u64 index = highlight->offsetCount++;
    if (index == 0) offsets[index] = 0;
    else offsets[index] = offsets[index - 1] + BufferLine(buffer, index - 1)->length + 1;
  }

  return offsets[line];
}

static u64
TextSize(const Buffer *buffer)
{
  u64 lineCount = BufferLineCount(buffer);
  u64 size = lineCount - 1;
  for (u64 line = 0; line < lineCount; line++) size += BufferLine(buffer, line)->length;
  return size;
}

static void
InvalidateWindows(Highlight *highlight)
{
  highlight->windows[0].valid = false;
  highlight->windows[1].valid = false;
}

static void
FreeLayer(Layer *layer)
{
  if (layer->tree) ts_tree_delete(layer->tree);
  free(layer->ranges);
}

// forgets every tree, so the next line asked for parses everything again.
static void
ForgetTrees(Highlight *highlight)
{
  if (highlight->tree) ts_tree_delete(highlight->tree);
  highlight->tree = nullptr;
  highlight->dirty = true;

  for (u32 index = 0; index < highlight->layerCount; index++) {
    FreeLayer(&highlight->layers[index]);
  }

  highlight->layerCount = 0;
  InvalidateWindows(highlight);

  // glibc keeps freed memory for the process, and a large tree or abandoned parse leaves
  // tens of megabytes of it.
  malloc_trim(0);
}

// where a byte and its point end up after an edit, as BufferAdjust moves positions.
static void
AdjustPlace(u32 *byte, TSPoint *point, const TSInputEdit *edit)
{
  if (*byte < edit->start_byte) return;

  if (*byte < edit->old_end_byte) {
    *byte = edit->start_byte;
    *point = edit->start_point;
    return;
  }

  *byte = *byte - edit->old_end_byte + edit->new_end_byte;

  if (point->row == edit->old_end_point.row) {
    point->column = point->column - edit->old_end_point.column + edit->new_end_point.column;
  }

  point->row = point->row - edit->old_end_point.row + edit->new_end_point.row;
}

static void
EditLayer(Layer *layer, const TSInputEdit *edit)
{
  if (layer->tree) ts_tree_edit(layer->tree, edit);
  u32 first = layer->ranges[0].start_byte;
  u32 last = layer->ranges[layer->rangeCount - 1].end_byte;
  if (edit->start_byte <= last && edit->old_end_byte >= first) layer->dirty = true;

  for (u32 index = 0; index < layer->rangeCount; index++) {
    TSRange *range = &layer->ranges[index];
    AdjustPlace(&range->start_byte, &range->start_point, edit);
    AdjustPlace(&range->end_byte, &range->end_point, edit);
  }
}

static TSPoint
PointOf(Position position)
{
  return (TSPoint){(u32)position.line, (u32)position.byte};
}

static void
Changed(void *context, const BufferChange *change)
{
  Highlight *highlight = context;
  InvalidateWindows(highlight);
  highlight->size = highlight->size - change->oldLength + change->newLength;

  bool tooLarge = highlight->size > maxHighlightSize;
  if (tooLarge && !highlight->tooLarge) ForgetTrees(highlight);
  highlight->tooLarge = tooLarge;

  // the offsets of lines up to the change's first line are still right afterwards.
  // a highlighter that is off only drops the rest, rather than walk every line up to the
  // change to work out where it starts.
  if (tooLarge || highlight->tooSlow) {
    u64 kept = change->start.line + 1;
    if (highlight->offsetCount > kept) highlight->offsetCount = kept;
    return;
  }

  u64 start = LineOffset(highlight, change->start.line) + change->start.byte;
  highlight->offsetCount = change->start.line + 1;

  TSInputEdit edit = {
    .start_byte = (u32)start,
    .old_end_byte = (u32)(start + change->oldLength),
    .new_end_byte = (u32)(start + change->newLength),
    .start_point = PointOf(change->start),
    .old_end_point = PointOf(change->oldEnd),
    .new_end_point = PointOf(change->newEnd)
  };

  if (highlight->tree) ts_tree_edit(highlight->tree, &edit);
  highlight->dirty = true;

  for (u32 index = 0; index < highlight->layerCount; index++) {
    EditLayer(&highlight->layers[index], &edit);
  }
}

static void
CheckSize(Highlight *highlight)
{
  highlight->size = TextSize(highlight->buffer);
  highlight->tooLarge = highlight->size > maxHighlightSize;
}

Highlight *
HighlightNew(Buffer *buffer, const Lang *lang, StrBuf *message)
{
  u32 grammar = lang->index;
  if (!Compile(grammar, message)) return nullptr;

  Highlight *highlight = MemoryAllocateZeroed(1, sizeof(Highlight));
  highlight->buffer = buffer;
  highlight->grammar = grammar;
  highlight->parser = ts_parser_new();
  highlight->dirty = true;
  CheckSize(highlight);
  BufferListen(buffer, Changed, highlight);
  return highlight;
}

void
HighlightFree(Highlight *highlight)
{
  if (!highlight) return;

  BufferUnlisten(highlight->buffer, Changed, highlight);
  ForgetTrees(highlight);
  ts_parser_delete(highlight->parser);
  free(highlight->layers);
  free(highlight->offsets);

  for (u32 index = 0; index < 2; index++) {
    free(highlight->windows[index].starts);
    free(highlight->windows[index].spans);
    free(highlight->windows[index].bytes);
  }

  free(highlight);
}

void
HighlightReset(Highlight *highlight)
{
  if (!highlight) return;

  ForgetTrees(highlight);
  highlight->offsetCount = 0;
  highlight->tooSlow = false;
  CheckSize(highlight);
}

bool
HighlightDisabled(const Highlight *highlight)
{
  return highlight && (highlight->tooLarge || highlight->tooSlow);
}

// hands the parser the rest of the line at a point, then its line break.
static const char *
Read(void *payload, [[maybe_unused]] u32 byte, TSPoint point, u32 *bytesRead)
{
  const Buffer *buffer = payload;
  u64 lineCount = BufferLineCount(buffer);
  *bytesRead = 0;
  if (point.row >= lineCount) return "";

  const StrBuf *line = BufferLine(buffer, point.row);

  if (point.column < line->length) {
    *bytesRead = (u32)(line->length - point.column);
    return line->data + point.column;
  }

  if (point.row + 1 == lineCount) return "";

  *bytesRead = 1;
  return "\n";
}

// the processor time this thread has used, so a busy machine, a stopped process or a
// suspend during a parse does not count against its deadline.
static i64
Milliseconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return now.tv_sec * 1000 + now.tv_nsec / 1'000'000;
}

// asks the parser to stop once its deadline has passed.
static bool
PastDeadline(TSParseState *state)
{
  const i64 *deadline = state->payload;
  return Milliseconds() > *deadline;
}

// parses a grammar over ranges (or the whole buffer with none), reusing an edited tree.
// a parse that runs out of time gives nullptr and marks the highlighter too slow.
static TSTree *
Parse(Highlight *highlight, u32 grammar, TSTree *old, const TSRange *ranges, u32 count)
{
  TSParser *parser = highlight->parser;
  ts_parser_set_language(parser, compiled[grammar].language);
  ts_parser_set_included_ranges(parser, ranges, count);

  TSInput input =
    {.payload = highlight->buffer, .read = Read, .encoding = TSInputEncodingUTF8};

  i64 deadline = Milliseconds() + maxParseTime;
  TSParseOptions options = {.payload = &deadline, .progress_callback = PastDeadline};
  TSTree *tree = ts_parser_parse_with_options(parser, old, input, options);
  if (tree) return tree;

  // a stopped parse would otherwise resume with the next one.
  ts_parser_reset(parser);
  highlight->tooSlow = true;
  return nullptr;
}

static void
ParseBuffer(Highlight *highlight)
{
  if (!highlight->dirty) return;

  TSTree *tree = Parse(highlight, highlight->grammar, highlight->tree, nullptr, 0);
  if (highlight->tree) ts_tree_delete(highlight->tree);
  highlight->tree = tree;
  highlight->dirty = false;
}

static void
NodeText(void *context, TSNode node, StrBuf *text)
{
  const Buffer *buffer = context;
  TSPoint start = ts_node_start_point(node);
  TSPoint end = ts_node_end_point(node);
  Range range = {{start.row, start.column}, {end.row, end.column}};
  BufferGetText(buffer, range, text);
}

static TSQueryCursor *
CursorOver(const Query *query, TSNode root, Segment segment)
{
  TSQueryCursor *cursor = ts_query_cursor_new();
  ts_query_cursor_set_point_range(cursor, segment.start, segment.end);
  ts_query_cursor_exec(cursor, query->query, root);
  return cursor;
}

static void
AddPaint(PaintList *list, Paint paint)
{
  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 64;
    list->paints = MemoryResize(list->paints, list->capacity, sizeof(Paint));
  }

  paint.order = list->count;
  list->paints[list->count++] = paint;
}

static i32
ComparePoints(TSPoint a, TSPoint b)
{
  if (a.row != b.row) return a.row < b.row ? -1 : 1;
  if (a.column != b.column) return a.column < b.column ? -1 : 1;
  return 0;
}

// paints go on in order: by start, enclosing nodes before the nodes inside them, and for
// the same node the later pattern (by its order, see firstPatternWins) last, so it wins.
static i32
ComparePaints(const void *a, const void *b)
{
  const Paint *first = a;
  const Paint *second = b;
  i32 order = ComparePoints(first->start, second->start);
  if (order) return order;

  order = ComparePoints(second->end, first->end);
  if (order) return order;

  if (first->pattern != second->pattern) return first->pattern < second->pattern ? -1 : 1;
  return first->order < second->order ? -1 : 1;
}

// paints a capture over the part of it inside the segment, so a node that reaches into
// other segments cannot paint over what they coloured.
static void
PaintRows(Highlight *highlight, Window *window, Segment segment, const Paint *paint)
{
  u8 group = paint->group;
  enum ThemeGroup themeGroup = (enum ThemeGroup)(group & ~specialBit);
  bool literal = themeGroup == ThemeLiteral || themeGroup == ThemeUri;
  TSPoint start = paint->start;
  TSPoint end = paint->end;
  if (ComparePoints(start, segment.start) < 0) start = segment.start;
  if (ComparePoints(end, segment.end) > 0) end = segment.end;
  u64 last = window->first + window->count - 1;
  if (end.row < last) last = end.row;

  for (u64 line = start.row; line <= last; line++) {
    u64 length = BufferLine(highlight->buffer, line)->length;
    u64 from = line == start.row ? start.column : 0;
    u64 to = line == end.row ? end.column : length;
    if (to > length) to = length;

    u8 *bytes = window->bytes + window->starts[line - window->first];

    for (u64 byte = from; byte < to; byte++) {
      u8 keep = (bytes[byte] & literalBit) | (literal ? literalBit : 0);
      bytes[byte] = keep | group;
    }
  }
}

static void
PaintCaptures(Highlight *highlight, u32 grammar, TSNode root, Window *window, Segment segment)
{
  const Compiled *queries = &compiled[grammar];
  TSQueryCursor *cursor = CursorOver(&queries->highlights, root, segment);
  PaintList list = {};
  TSQueryMatch match;
  u32 captureIndex;

  while (ts_query_cursor_next_capture(cursor, &match, &captureIndex)) {
    const TSQueryCapture *capture = &match.captures[captureIndex];
    u8 group = queries->groups[capture->index];
    if (group == noGroup) continue;
    if (!QueryAccepts(&queries->highlights, &match, NodeText, highlight->buffer)) continue;

    // paints of the same node go on in pattern order, so the last one wins.
    u32 pattern = match.pattern_index;
    if (LangAt(grammar)->firstPatternWins) pattern = UINT32_MAX - pattern;

    Paint paint = {
      .start = ts_node_start_point(capture->node),
      .end = ts_node_end_point(capture->node),
      .group = group,
      .pattern = pattern
    };

    AddPaint(&list, paint);
  }

  ts_query_cursor_delete(cursor);
  if (list.count) qsort(list.paints, list.count, sizeof(Paint), ComparePaints);

  for (u32 index = 0; index < list.count; index++) {
    PaintRows(highlight, window, segment, &list.paints[index]);
  }

  free(list.paints);
}

static void
AddRange(TSRange **ranges, u32 *count, TSRange range)
{
  if (range.end_byte <= range.start_byte) return;

  *ranges = MemoryResize(*ranges, *count + 1, sizeof(TSRange));
  (*ranges)[(*count)++] = range;
}

// the ranges of an injected node: all of it, or without its named children, which belong
// to the outer language (such as the "> " of a quoted code block).
// anonymous children are only tokens of the outer grammar inside the text.
static u32
ContentRanges(TSNode node, bool includeChildren, TSRange **ranges)
{
  *ranges = nullptr;
  u32 count = 0;
  TSRange range = {
    ts_node_start_point(node),
    ts_node_end_point(node),
    ts_node_start_byte(node),
    ts_node_end_byte(node)
  };

  u32 childCount = includeChildren ? 0 : ts_node_named_child_count(node);

  for (u32 index = 0; index < childCount; index++) {
    TSNode child = ts_node_named_child(node, index);
    TSRange before = range;
    before.end_byte = ts_node_start_byte(child);
    before.end_point = ts_node_start_point(child);
    AddRange(ranges, &count, before);

    if (ts_node_end_byte(child) > range.start_byte) {
      range.start_byte = ts_node_end_byte(child);
      range.start_point = ts_node_end_point(child);
    }
  }

  AddRange(ranges, &count, range);
  return count;
}

// the grammar a match injects, or UINT32_MAX.
static u32
InjectedGrammar(Highlight *highlight, u32 grammar, const TSQueryMatch *match)
{
  const Compiled *queries = &compiled[grammar];
  const char *name = queries->injections.patterns[match->pattern_index].injectionLanguage;
  StrBuf text = {};

  for (u32 index = 0; index < match->capture_count; index++) {
    if (match->captures[index].index != queries->languageCapture) continue;

    NodeText(highlight->buffer, match->captures[index].node, &text);
    name = text.data;
  }

  const Lang *lang = name ? LangOfInjection(name, strlen(name)) : nullptr;
  u32 injected = lang ? lang->index : UINT32_MAX;
  StrBufFree(&text);
  return injected;
}

static void
AddSite(SiteList *list, Site site)
{
  if (list->count == list->capacity) {
    list->capacity = list->capacity ? list->capacity * 2 : 16;
    list->sites = MemoryResize(list->sites, list->capacity, sizeof(Site));
  }

  list->sites[list->count++] = site;
}

static void
FindSites(Highlight *highlight, u32 grammar, TSNode root, Segment segment, SiteList *sites)
{
  const Compiled *queries = &compiled[grammar];
  const Query *query = &queries->injections;
  TSQueryCursor *cursor = CursorOver(query, root, segment);
  TSQueryMatch match;

  while (ts_query_cursor_next_match(cursor, &match)) {
    if (!QueryAccepts(query, &match, NodeText, highlight->buffer)) continue;

    // a grammar whose queries fail is left uncoloured here, as nowhere could say why.
    u32 injected = InjectedGrammar(highlight, grammar, &match);
    StrBuf error = {};
    bool ready = injected != UINT32_MAX && Compile(injected, &error);
    StrBufFree(&error);
    if (!ready) continue;

    bool includeChildren = query->patterns[match.pattern_index].includeChildren;

    for (u32 index = 0; index < match.capture_count; index++) {
      if (match.captures[index].index != queries->contentCapture) continue;

      TSNode node = match.captures[index].node;
      Site site = {.grammar = injected};
      site.rangeCount = ContentRanges(node, includeChildren, &site.ranges);
      if (site.rangeCount) AddSite(sites, site);
      else free(site.ranges);
    }
  }

  ts_query_cursor_delete(cursor);
}

static bool
SameRanges(const TSRange *a, const TSRange *b, u32 count)
{
  for (u32 index = 0; index < count; index++) {
    if (a[index].start_byte != b[index].start_byte) return false;
    if (a[index].end_byte != b[index].end_byte) return false;
  }

  return true;
}

static Layer *
FindLayer(Highlight *highlight, const Site *site)
{
  for (u32 index = 0; index < highlight->layerCount; index++) {
    Layer *layer = &highlight->layers[index];
    if (layer->grammar != site->grammar || layer->rangeCount != site->rangeCount) continue;
    if (layer->ranges[0].start_byte == site->ranges[0].start_byte) return layer;
  }

  if (highlight->layerCount == highlight->layerCapacity) {
    highlight->layerCapacity = highlight->layerCapacity ? highlight->layerCapacity * 2 : 16;
    u32 capacity = highlight->layerCapacity;
    highlight->layers = MemoryResize(highlight->layers, capacity, sizeof(Layer));
  }

  Layer *layer = &highlight->layers[highlight->layerCount++];
  *layer = (Layer){.grammar = site->grammar, .dirty = true};
  return layer;
}

// finds or parses the layer of a site, taking its ranges, and returns its tree.
static TSTree *
LayerTree(Highlight *highlight, Site *site)
{
  Layer *layer = FindLayer(highlight, site);
  layer->used = highlight->serial;

  bool same = layer->tree && SameRanges(layer->ranges, site->ranges, site->rangeCount);

  if (same && !layer->dirty) {
    free(site->ranges);
    return layer->tree;
  }

  TSTree *old = LangAt(site->grammar)->freshParse ? nullptr : layer->tree;
  TSTree *tree = Parse(highlight, site->grammar, old, site->ranges, site->rangeCount);
  if (layer->tree) ts_tree_delete(layer->tree);
  free(layer->ranges);
  layer->tree = tree;
  layer->ranges = site->ranges;
  layer->rangeCount = site->rangeCount;
  layer->dirty = false;
  return tree;
}

// what PaintTree paints: a part of a window, and how deep the language is nested.
typedef struct PaintTarget {
  Window *window;
  Segment segment;
  u32 depth;
} PaintTarget;

static void
PaintTree(Highlight *highlight, u32 grammar, TSTree *tree, PaintTarget target)
{
  if (!tree) return;

  TSNode root = ts_tree_root_node(tree);
  PaintCaptures(highlight, grammar, root, target.window, target.segment);
  if (!compiled[grammar].injections.query || target.depth + 1 >= maxDepth) return;

  // the sites are all found before any is parsed, as parsing may move the layers.
  SiteList sites = {};
  FindSites(highlight, grammar, root, target.segment, &sites);
  target.depth++;

  for (u32 index = 0; index < sites.count; index++) {
    Site *site = &sites.sites[index];
    u32 injected = site->grammar;
    PaintTree(highlight, injected, LayerTree(highlight, site), target);
  }

  free(sites.sites);
}

// drops the layers not used by the last two windows once there are too many.
static void
TrimLayers(Highlight *highlight)
{
  if (highlight->layerCount <= maxLayers) return;

  u32 kept = 0;

  for (u32 index = 0; index < highlight->layerCount; index++) {
    Layer *layer = &highlight->layers[index];

    if (layer->used + 1 >= highlight->serial) highlight->layers[kept++] = *layer;
    else FreeLayer(layer);
  }

  highlight->layerCount = kept;
}

// the bytes of a line coloured around focus.
static Span
SpanAround(u64 length, u64 focus)
{
  if (length <= longLineSize) return (Span){0, length};

  u64 before = longLineReach / 4;
  u64 from = focus > before ? focus - before : 0;
  u64 to = length - from > longLineReach ? from + longLineReach : length;
  return (Span){from > length ? length : from, to};
}

// true when a window's span of a line covers focus, with room to scroll on.
static bool
Covers(Span span, u64 length, u64 focus)
{
  if (length <= longLineSize) return true;
  return focus >= span.from && (span.to == length || focus + longLineReach / 2 <= span.to);
}

// colours each run of short lines in one go, and each long line over its span alone.
static void
PaintWindow(Highlight *highlight, Window *window)
{
  PaintTarget target = {.window = window};
  u64 end = window->first + window->count;
  u64 line = window->first;

  while (line < end) {
    u64 index = line - window->first;
    u64 length = BufferLine(highlight->buffer, line)->length;
    Span span = window->spans[index];
    Segment *segment = &target.segment;

    if (length > longLineSize) {
      segment->start = (TSPoint){(u32)line, (u32)span.from};
      segment->end = (TSPoint){(u32)line, (u32)span.to};
      line++;
    } else {
      segment->start = (TSPoint){(u32)line, 0};
      while (line < end && BufferLine(highlight->buffer, line)->length <= longLineSize) line++;
      segment->end = (TSPoint){(u32)line, 0};
    }

    PaintTree(highlight, highlight->grammar, highlight->tree, target);
  }
}

static void
Fill(Highlight *highlight, Window *window, u64 line, u64 focus)
{
  u64 lineCount = BufferLineCount(highlight->buffer);
  window->first = line > windowBefore ? line - windowBefore : 0;
  u64 end = line + windowAfter < lineCount ? line + windowAfter : lineCount;
  window->count = end - window->first;
  window->starts = MemoryResize(window->starts, window->count + 1, sizeof(u64));
  window->spans = MemoryResize(window->spans, window->count, sizeof(Span));

  u64 total = 0;

  // other long lines in the window are coloured around the same byte, which suits an
  // unwrapped view scrolled sideways.
  for (u64 index = 0; index < window->count; index++) {
    u64 length = BufferLine(highlight->buffer, window->first + index)->length;
    window->starts[index] = total;
    window->spans[index] = SpanAround(length, focus);
    total += length;
  }

  window->starts[window->count] = total;
  window->bytes = MemoryResize(window->bytes, total ? total : 1, 1);
  memset(window->bytes, ThemeNone, total);

  highlight->serial++;
  window->used = highlight->serial;
  window->valid = true;
  ParseBuffer(highlight);
  PaintWindow(highlight, window);
  TrimLayers(highlight);

  // the trees go only once nothing is using them.
  if (highlight->tooSlow) ForgetTrees(highlight);
}

const u8 *
HighlightLine(Highlight *highlight, u64 line, u64 focus)
{
  if (!highlight || HighlightDisabled(highlight)) return nullptr;

  Window *windows = highlight->windows;
  u64 length = BufferLine(highlight->buffer, line)->length;

  for (u32 index = 0; index < 2; index++) {
    Window *window = &windows[index];
    bool inside = window->valid && line >= window->first;
    inside = inside && line < window->first + window->count;
    if (!inside || !Covers(window->spans[line - window->first], length, focus)) continue;

    return window->bytes + window->starts[line - window->first];
  }

  // the window used longest ago is filled again.
  Window *window = &windows[0];
  if (windows[1].used < windows[0].used || !windows[1].valid) window = &windows[1];
  Fill(highlight, window, line, focus);
  return window->bytes + window->starts[line - window->first];
}

bool
HighlightSpellable(Highlight *highlight, u64 line, u64 byte)
{
  const u8 *groups = HighlightLine(highlight, line, byte);
  if (!groups) return true;
  if (groups[byte] & (literalBit | specialBit)) return false;

  enum ThemeGroup group = HighlightGroup(groups, byte);
  if (group == ThemeComment || group == ThemeString) return true;

  bool markup = group == ThemeNone || (group >= ThemeTitle && group <= ThemeReference);
  return LangAt(highlight->grammar)->prose && markup;
}

// compiles a grammar's tags query the first time symbols are asked for.
// a query that fails to compile leaves the grammar without symbols, which the tags
// harness would show.
static bool
CompileTags(u32 index)
{
  Prepare();
  Compiled *grammar = &compiled[index];
  if (grammar->tagsTried) return grammar->tagsReady;

  grammar->tagsTried = true;
  const Lang *lang = LangAt(index);
  const TSLanguage *language = lang->hasTags ? Load(index) : nullptr;
  if (!language) return false;

  StrBuf error = {};
  Query *query = &grammar->tags;
  grammar->tagsReady = CompileQuery(lang, language, "tags.scm", false, query, &error);
  StrBufFree(&error);
  if (!grammar->tagsReady) return false;

  // "definition.function" is a symbol of kind "function".
  static constexpr char prefix[] = "definition.";
  u32 count = ts_query_capture_count(query->query);
  grammar->kinds = MemoryAllocateZeroed(count ? count : 1, sizeof(const char *));

  for (u32 capture = 0; capture < count; capture++) {
    u32 length;
    const char *name = ts_query_capture_name_for_id(query->query, capture, &length);
    bool definition = strncmp(name, prefix, sizeof(prefix) - 1) == 0;
    if (definition) grammar->kinds[capture] = name + sizeof(prefix) - 1;
  }

  grammar->nameCapture = QueryCapture(query, "name");
  grammar->bodyCapture = QueryCapture(query, "body");
  return true;
}

bool
HighlightHasSymbols(const Lang *lang)
{
  return CompileTags(lang->index);
}

// where the text of a tree comes from: a buffer, or text held in one piece.
typedef struct Source {
  const Buffer *buffer;
  const char *text;
  u64 length;
} Source;

// a place in a tree's text.
typedef struct Place {
  u32 byte;
  TSPoint point;
} Place;

static Place
StartOf(TSNode node)
{
  return (Place){ts_node_start_byte(node), ts_node_start_point(node)};
}

static Place
EndOf(TSNode node)
{
  return (Place){ts_node_end_byte(node), ts_node_end_point(node)};
}

// appends the text from a place up to a byte.
static void
SourceText(const Source *source, Place from, u32 endByte, StrBuf *text)
{
  if (endByte <= from.byte) return;

  if (source->text) {
    StrBufAppend(text, source->text + from.byte, endByte - from.byte);
    return;
  }

  // the end is found by counting the bytes on from the start, a line break being one.
  Position start = {from.point.row, from.point.column};
  Position end = start;
  u64 left = endByte - from.byte;

  while (left) {
    u64 rest = BufferLine(source->buffer, end.line)->length - end.byte;

    if (left <= rest) {
      end.byte += left;
      break;
    }

    left -= rest + 1;
    end = (Position){end.line + 1, 0};
  }

  BufferGetText(source->buffer, (Range){start, end}, text);
}

static void
SourceNodeText(void *context, TSNode node, StrBuf *text)
{
  SourceText(context, StartOf(node), ts_node_end_byte(node), text);
}

// appends text with each run of blanks and line breaks made one space, and none at the
// start of out.
static void
AppendCollapsed(StrBuf *out, const char *text, u64 length)
{
  for (u64 at = 0; at < length; at++) {
    char byte = text[at];
    bool blank = byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';

    if (!blank) {
      StrBufAppendByte(out, byte);
      continue;
    }

    bool afterBlank = !out->length || out->data[out->length - 1] == ' ';
    if (!afterBlank) StrBufAppendByte(out, ' ');
  }
}

static void
TrimEnd(StrBuf *text)
{
  while (text->length && text->data[text->length - 1] == ' ') text->length--;
  if (text->data) text->data[text->length] = '\0';
}

// cuts text to at most maxSignatureLength bytes, on a code point boundary.
static void
Cut(StrBuf *text)
{
  if (text->length <= maxSignatureLength) return;

  u64 length = maxSignatureLength;
  while (length && ((u8)text->data[length] & 0xC0) == 0x80) length--;
  text->length = length;
  TrimEnd(text);
  StrBufAppendString(text, "…");
}

// how far into a definition its signature is looked for, so a definition with no body is
// not walked to its end.
static constexpr u32 signatureReach = 8 * maxSignatureLength;

static bool
IsComment(TSNode node)
{
  return strstr(ts_node_type(node), "comment") != nullptr;
}

// the definition's text up to its first body (a "body" field, or the node captured as
// @body), with comments left out and blanks collapsed.
static void
Signature(const Source *source, TSNode definition, TSNode body, StrBuf *signature)
{
  u32 start = ts_node_start_byte(definition);
  u32 stop = ts_node_end_byte(definition);
  if (stop - start > signatureReach) stop = start + signatureReach;

  // the definition's nodes in order, noting comments, until the body.
  TSTreeCursor cursor = ts_tree_cursor_new(definition);
  TSNode comments[16];
  u32 commentCount = 0;
  bool more = ts_tree_cursor_goto_first_child(&cursor);

  while (more) {
    TSNode node = ts_tree_cursor_current_node(&cursor);
    u32 nodeStart = ts_node_start_byte(node);
    if (nodeStart >= stop) break;

    const char *field = ts_tree_cursor_current_field_name(&cursor);
    bool isBody = (field && strcmp(field, "body") == 0) || ts_node_eq(node, body);

    if (isBody) {
      stop = nodeStart;
      break;
    }

    bool comment = IsComment(node);
    if (comment && commentCount < 16) comments[commentCount++] = node;

    // into the node, or on past it.
    if (!comment && ts_tree_cursor_goto_first_child(&cursor)) continue;

    while (!(more = ts_tree_cursor_goto_next_sibling(&cursor))) {
      if (!ts_tree_cursor_goto_parent(&cursor)) break;
    }
  }

  ts_tree_cursor_delete(&cursor);

  // the text between the comments, from the definition's start to the stop.
  StrBuf text = {};
  Place from = StartOf(definition);

  for (u32 index = 0; index <= commentCount; index++) {
    u32 end = index < commentCount ? ts_node_start_byte(comments[index]) : stop;
    if (end > stop) end = stop;

    StrBufClear(&text);
    SourceText(source, from, end, &text);
    AppendCollapsed(signature, text.data, text.length);
    if (signature->length > maxSignatureLength || index == commentCount) break;

    // a blank stands in for the comment.
    AppendCollapsed(signature, " ", 1);
    from = EndOf(comments[index]);
  }

  StrBufFree(&text);
  TrimEnd(signature);
  Cut(signature);
}

// one match of a tags query.
typedef struct Found {
  TSNode definition;
  TSNode name;
  TSNode body;
  const char *kind;
  u32 pattern;
} Found;

static i32
CompareFound(const void *a, const void *b)
{
  const Found *first = a;
  const Found *second = b;
  u32 starts[2] = {
    ts_node_start_byte(first->definition),
    ts_node_start_byte(second->definition)
  };
  if (starts[0] != starts[1]) return starts[0] < starts[1] ? -1 : 1;

  u32 names[2] = {ts_node_start_byte(first->name), ts_node_start_byte(second->name)};
  if (names[0] != names[1]) return names[0] < names[1] ? -1 : 1;

  if (first->pattern != second->pattern) return first->pattern < second->pattern ? -1 : 1;
  return 0;
}

static Position
PositionOf(TSPoint point)
{
  return (Position){point.row, point.column};
}

// runs a grammar's tags query over a tree and visits its symbols in order.
// a definition that several patterns match is visited once, for the earliest pattern.
static void
VisitSymbols(
  u32 grammar,
  TSNode root,
  const Source *source,
  HighlightVisit visit,
  void *context
)
{
  const Compiled *queries = &compiled[grammar];
  const Query *query = &queries->tags;
  TSQueryCursor *cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, query->query, root);

  Found *found = nullptr;
  u64 count = 0;
  u64 capacity = 0;
  TSQueryMatch match;

  while (ts_query_cursor_next_match(cursor, &match)) {
    if (!QueryAccepts(query, &match, SourceNodeText, (void *)source)) continue;

    Found item = {.pattern = match.pattern_index};

    for (u32 index = 0; index < match.capture_count; index++) {
      const TSQueryCapture *capture = &match.captures[index];
      const char *kind = queries->kinds[capture->index];

      if (kind) {
        item.definition = capture->node;
        item.kind = kind;
      }

      if (capture->index == queries->nameCapture) item.name = capture->node;
      if (capture->index == queries->bodyCapture) item.body = capture->node;
    }

    if (!item.kind || ts_node_is_null(item.name)) continue;

    if (count == capacity) {
      capacity = capacity ? capacity * 2 : 64;
      found = MemoryResize(found, capacity, sizeof(Found));
    }

    found[count++] = item;
  }

  ts_query_cursor_delete(cursor);
  if (count) qsort(found, count, sizeof(Found), CompareFound);

  StrBuf name = {};
  StrBuf text = {};
  StrBuf signature = {};

  for (u64 index = 0; index < count; index++) {
    const Found *item = &found[index];
    bool repeated = index > 0 && ts_node_eq(item->definition, found[index - 1].definition) &&
      ts_node_eq(item->name, found[index - 1].name);
    if (repeated) continue;

    StrBufClear(&name);
    StrBufClear(&text);
    StrBufClear(&signature);
    SourceNodeText((void *)source, item->name, &text);
    AppendCollapsed(&name, text.data, text.length);
    TrimEnd(&name);
    Signature(source, item->definition, item->body, &signature);

    HighlightSymbol symbol = {
      .kind = item->kind,
      .start = PositionOf(ts_node_start_point(item->definition)),
      .name =
        {PositionOf(ts_node_start_point(item->name)),
          PositionOf(ts_node_end_point(item->name))},
      .nameText = name.data ? name.data : "",
      .nameLength = name.length,
      .signature = signature.data ? signature.data : "",
      .signatureLength = signature.length
    };

    visit(context, &symbol);
  }

  StrBufFree(&name);
  StrBufFree(&text);
  StrBufFree(&signature);
  free(found);
}

// parses the buffer if it changed, returning false when it cannot be parsed in time or is
// not highlighted at all.
static bool
ParsedTree(Highlight *highlight)
{
  if (HighlightDisabled(highlight)) return false;

  ParseBuffer(highlight);

  if (highlight->tooSlow) {
    ForgetTrees(highlight);
    return false;
  }

  return highlight->tree != nullptr;
}

bool
HighlightSymbols(Highlight *highlight, HighlightVisit visit, void *context)
{
  if (!highlight || !CompileTags(highlight->grammar) || !ParsedTree(highlight)) return false;

  Source source = {.buffer = highlight->buffer};
  TSNode root = ts_tree_root_node(highlight->tree);
  VisitSymbols(highlight->grammar, root, &source, visit, context);
  return true;
}

// hands the parser the rest of a text in one piece.
static const char *
ReadText(void *payload, u32 byte, [[maybe_unused]] TSPoint point, u32 *bytesRead)
{
  const Source *source = payload;
  *bytesRead = byte < source->length ? (u32)(source->length - byte) : 0;
  return *bytesRead ? source->text + byte : "";
}

bool
HighlightSymbolsOfText(
  const Lang *lang,
  const char *text,
  u64 length,
  HighlightVisit visit,
  void *context
)
{
  u32 grammar = lang->index;
  if (length > maxHighlightSize || !CompileTags(grammar)) return true;

  TSParser *parser = ts_parser_new();
  ts_parser_set_language(parser, compiled[grammar].language);

  Source source = {.text = text, .length = length};
  TSInput input = {.payload = &source, .read = ReadText, .encoding = TSInputEncodingUTF8};
  i64 deadline = Milliseconds() + maxParseTime;
  TSParseOptions options = {.payload = &deadline, .progress_callback = PastDeadline};
  TSTree *tree = ts_parser_parse_with_options(parser, nullptr, input, options);

  if (tree) {
    VisitSymbols(grammar, ts_tree_root_node(tree), &source, visit, context);
    ts_tree_delete(tree);
  }

  ts_parser_delete(parser);
  return tree != nullptr;
}

// the smallest named node of a tree that holds the bytes from start to end and more.
static bool
NodeAround(TSTree *tree, u32 start, u32 end, TSNode *around)
{
  TSNode node = ts_node_named_descendant_for_byte_range(ts_tree_root_node(tree), start, end);

  for (; !ts_node_is_null(node); node = ts_node_parent(node)) {
    u32 nodeStart = ts_node_start_byte(node);
    u32 nodeEnd = ts_node_end_byte(node);
    bool holds = nodeStart <= start && nodeEnd >= end;
    bool more = nodeStart < start || nodeEnd > end;

    if (holds && more && ts_node_is_named(node)) {
      *around = node;
      return true;
    }
  }

  return false;
}

static bool
LayerHolds(const Layer *layer, u32 start, u32 end)
{
  for (u32 index = 0; index < layer->rangeCount; index++) {
    const TSRange *range = &layer->ranges[index];
    if (range->start_byte <= start && end <= range->end_byte) return true;
  }

  return false;
}

bool
HighlightNodeAround(Highlight *highlight, Range range, Range *outer)
{
  if (!highlight || !ParsedTree(highlight)) return false;

  u32 start = (u32)(LineOffset(highlight, range.start.line) + range.start.byte);
  u32 end = (u32)(LineOffset(highlight, range.end.line) + range.end.byte);
  TSNode best;
  bool found = NodeAround(highlight->tree, start, end, &best);

  // an injected language (a fenced code block) may hold a smaller node, when its layer is
  // parsed and up to date.
  for (u32 index = 0; index < highlight->layerCount; index++) {
    const Layer *layer = &highlight->layers[index];
    if (!layer->tree || layer->dirty || !LayerHolds(layer, start, end)) continue;

    TSNode node;
    if (!NodeAround(layer->tree, start, end, &node)) continue;

    u32 size = ts_node_end_byte(node) - ts_node_start_byte(node);
    bool smaller = !found || size < ts_node_end_byte(best) - ts_node_start_byte(best);

    if (smaller) {
      best = node;
      found = true;
    }
  }

  if (!found) return false;

  outer->start = PositionOf(ts_node_start_point(best));
  outer->end = PositionOf(ts_node_end_point(best));
  return true;
}

bool
HighlightNodeOfType(Highlight *highlight, Position position, const char *type, Range *range)
{
  if (!highlight || !ParsedTree(highlight)) return false;

  u32 offset = (u32)(LineOffset(highlight, position.line) + position.byte);
  TSNode root = ts_tree_root_node(highlight->tree);
  TSNode node = ts_node_descendant_for_byte_range(root, offset, offset);

  for (; !ts_node_is_null(node); node = ts_node_parent(node)) {
    if (strcmp(ts_node_type(node), type) != 0) continue;

    range->start = PositionOf(ts_node_start_point(node));
    range->end = PositionOf(ts_node_end_point(node));
    return true;
  }

  return false;
}
