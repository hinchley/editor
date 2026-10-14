#pragma once

// a compiled tree-sitter query together with what the runtime leaves to its user: the
// text predicates (#eq?, #match?, #any-of? and their #not- forms) and the injection
// settings made with #set!.
// other predicates (#is-not? local, #offset!) are ignored.
#include <tree_sitter/api.h>

#include "strbuf.h"
#include "types.h"

typedef struct QueryPredicate QueryPredicate;

typedef struct QueryPattern {
  QueryPredicate *predicates;
  u32 predicateCount;

  // set by #set! injection.language and #set! injection.include-children.
  const char *injectionLanguage;
  bool includeChildren;
} QueryPattern;

typedef struct Query {
  TSQuery *query;
  QueryPattern *patterns;
  u32 patternCount;
} Query;

// appends the text of a node to text.
typedef void (*QueryNodeText)(void *context, TSNode node, StrBuf *text);

// compiles a query, or returns false with the reason appended to error.
bool
QueryInit(
  Query *query,
  const TSLanguage *language,
  const u8 *source,
  u64 length,
  StrBuf *error
);

void
QueryFree(Query *query);

// true when a match passes the text predicates of its pattern.
bool
QueryAccepts(const Query *query, const TSQueryMatch *match, QueryNodeText text, void *context);

// the index of a capture name, or UINT32_MAX when the query has no such capture.
u32
QueryCapture(const Query *query, const char *name);
