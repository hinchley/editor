#include "highlight.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <tree_sitter/api.h>

#include "lang.h"
#include "memory.h"
#include "query.h"
#include "settings.h"

const TSLanguage *tree_sitter_c(void);
const TSLanguage *tree_sitter_css(void);
const TSLanguage *tree_sitter_html(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_json(void);
const TSLanguage *tree_sitter_markdown(void);
const TSLanguage *tree_sitter_markdown_inline(void);

static const u8 cHighlights[] = {
#embed "../vendor/queries/c/highlights.scm"
};

static const u8 cssHighlights[] = {
#embed "../vendor/queries/css/highlights.scm"
};

static const u8 htmlHighlights[] = {
#embed "../vendor/queries/html/highlights.scm"
};

static const u8 htmlInjections[] = {
#embed "../vendor/queries/html/injections.scm"
};

static const u8 javascriptHighlights[] = {
#embed "../vendor/queries/javascript/highlights.scm"
};

static const u8 javascriptInjections[] = {
#embed "../vendor/queries/javascript/injections.scm"
};

static const u8 jsonHighlights[] = {
#embed "../vendor/queries/json/highlights.scm"
};

static const u8 markdownHighlights[] = {
#embed "../vendor/queries/markdown/highlights.scm"
};

static const u8 markdownInjections[] = {
#embed "../vendor/queries/markdown/injections.scm"
};

static const u8 markdownInlineHighlights[] = {
#embed "../vendor/queries/markdown-inline/highlights.scm"
};

static const u8 markdownInlineInjections[] = {
#embed "../vendor/queries/markdown-inline/injections.scm"
};

typedef struct Embedded {
  const u8 *data;
  u64 size;
} Embedded;

typedef struct Grammar {
  // the display name from lang.c, or nullptr for a grammar that is only ever injected.
  const char *name;

  // the lowercase name other grammars inject it by.
  const char *injectionId;
  const TSLanguage *(*language)(void);
  Embedded highlights;
  Embedded injections;

  // prose is spell checked outside code; other languages only in comments and strings.
  bool prose;

  // parse again from scratch after an edit, for a grammar whose scanner gives different
  // trees when parsing incrementally (markdown's inline grammar).
  bool freshParse;
} Grammar;

static const Grammar grammars[] = {
  {
    .name = "C",
    .injectionId = "c",
    .language = tree_sitter_c,
    .highlights = {cHighlights, sizeof cHighlights}
  },
  {
    .name = "CSS",
    .injectionId = "css",
    .language = tree_sitter_css,
    .highlights = {cssHighlights, sizeof cssHighlights}
  },
  {
    .name = "HTML",
    .injectionId = "html",
    .language = tree_sitter_html,
    .highlights = {htmlHighlights, sizeof htmlHighlights},
    .injections = {htmlInjections, sizeof htmlInjections}
  },
  {
    .name = "JavaScript",
    .injectionId = "javascript",
    .language = tree_sitter_javascript,
    .highlights = {javascriptHighlights, sizeof javascriptHighlights},
    .injections = {javascriptInjections, sizeof javascriptInjections}
  },
  {
    .name = "JSON",
    .injectionId = "json",
    .language = tree_sitter_json,
    .highlights = {jsonHighlights, sizeof jsonHighlights}
  },
  {
    .name = "Markdown",
    .injectionId = "markdown",
    .language = tree_sitter_markdown,
    .highlights = {markdownHighlights, sizeof markdownHighlights},
    .injections = {markdownInjections, sizeof markdownInjections},
    .prose = true
  },
  {
    .injectionId = "markdown_inline",
    .language = tree_sitter_markdown_inline,
    .highlights = {markdownInlineHighlights, sizeof markdownInlineHighlights},
    .injections = {markdownInlineInjections, sizeof markdownInlineInjections},
    .prose = true,
    .freshParse = true
  }
};

static constexpr u32 grammarCount = sizeof grammars / sizeof grammars[0];

// a byte's group sits in the low bits.
// the literal bit marks code inside prose (fenced code, code spans, urls), which spell
// check passes over whatever colours it, and the special bit marks a string that is not
// text (a css colour, a regular expression, a json key).
static constexpr u8 literalBit = 0x80;
static constexpr u8 specialBit = 0x40;

// a capture with no group colours nothing.
static constexpr u8 noGroup = 0xFF;

// lines coloured at once around the line asked for.
static constexpr u64 windowBefore = 64;
static constexpr u64 windowAfter = 192;

// a line longer than longLineSize is coloured only around the byte asked for, from a
// quarter of longLineReach before it to longLineReach after it, so the cost of colouring
// does not grow with the length of the line.
static constexpr u64 longLineSize = 16384;
static constexpr u64 longLineReach = 65536;

// how deep languages may nest (markdown, its inline text, html inside that, a script).
static constexpr u32 maxDepth = 4;

// injected layers kept when they are not in sight.
static constexpr u32 maxLayers = 512;

// the queries of a grammar, compiled on first use and shared by every highlighter.
typedef struct Compiled {
  bool tried;
  bool ready;
  Query highlights;
  Query injections;

  // the group of each capture of the highlight query (with specialBit), or noGroup.
  u8 *groups;
  u32 contentCapture;
  u32 languageCapture;
} Compiled;

static Compiled compiled[grammarCount];

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

static u32
FindGrammarByName(const char *name)
{
  for (u32 index = 0; index < grammarCount; index++) {
    if (grammars[index].name && strcmp(grammars[index].name, name) == 0) return index;
  }

  return UINT32_MAX;
}

// an injection names a grammar by its id or, failing that, by a file extension, so a
// fenced code block can say "js" as well as "javascript".
static u32
FindInjected(const char *name, u64 length)
{
  for (u32 index = 0; index < grammarCount; index++) {
    const char *id = grammars[index].injectionId;
    if (strlen(id) == length && strncasecmp(id, name, length) == 0) return index;
  }

  const Lang *lang = LangOfExtension(name, length);
  return lang ? FindGrammarByName(lang->name) : UINT32_MAX;
}

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

// compiles a grammar's queries the first time it is used.
static bool
Compile(u32 index, StrBuf *message)
{
  Compiled *grammar = &compiled[index];
  if (grammar->tried) return grammar->ready;

  grammar->tried = true;
  const Grammar *source = &grammars[index];
  const TSLanguage *language = source->language();
  StrBuf error = {};
  const Embedded *highlights = &source->highlights;
  const Embedded *injections = &source->injections;
  Query *query = &grammar->highlights;
  bool ready = QueryInit(query, language, highlights->data, highlights->size, &error);

  if (ready && injections->data) {
    query = &grammar->injections;
    ready = QueryInit(query, language, injections->data, injections->size, &error);
    grammar->contentCapture = QueryCapture(query, "injection.content");
    grammar->languageCapture = QueryCapture(query, "injection.language");
  }

  if (ready) CompileGroups(grammar);
  else QueryFree(&grammar->highlights);

  const char *name = source->name ? source->name : source->injectionId;
  const char *reason = error.data ? error.data : "";
  if (!ready) StrBufAppendFormat(message, "%s highlighting failed: %s", name, reason);
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

  // the offsets of lines up to the change's first line are still right afterwards.
  u64 start = LineOffset(highlight, change->start.line) + change->start.byte;
  highlight->offsetCount = change->start.line + 1;

  bool tooLarge = highlight->size > maxHighlightSize;
  if (tooLarge && !highlight->tooLarge) ForgetTrees(highlight);
  highlight->tooLarge = tooLarge;
  if (tooLarge || highlight->tooSlow) return;

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
HighlightNew(Buffer *buffer, const char *language, StrBuf *message)
{
  u32 grammar = FindGrammarByName(language);
  if (grammar == UINT32_MAX || !Compile(grammar, message)) return nullptr;

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

static i64
Milliseconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
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
  ts_parser_set_language(parser, grammars[grammar].language());
  ts_parser_set_included_ranges(parser, ranges, count);

  TSInput input = {
    .payload = highlight->buffer,
    .read = Read,
    .encoding = TSInputEncodingUTF8
  };

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
// the same node the later pattern last, so it wins.
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
PaintCaptures(
  Highlight *highlight,
  u32 grammar,
  TSNode root,
  Window *window,
  Segment segment
) {
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

    Paint paint = {
      .start = ts_node_start_point(capture->node),
      .end = ts_node_end_point(capture->node),
      .group = group,
      .pattern = match.pattern_index
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

  u32 injected = name ? FindInjected(name, strlen(name)) : UINT32_MAX;
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

  TSTree *old = grammars[site->grammar].freshParse ? nullptr : layer->tree;
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
  return grammars[highlight->grammar].prose && markup;
}
