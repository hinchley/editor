#pragma once

// markdown: the commands that know markdown's own syntax, beyond colouring it.
// a table is found through the syntax tree (see HighlightNodeOfType), so text that only
// looks like one, such as in a code block, is left alone.
#include "document.h"
#include "types.h"

// what aligning a table did: its columns, its lines after aligning, and whether its text
// changed.
typedef struct MarkdownTableResult {
  u32 columns;
  u64 lines;
  bool changed;
} MarkdownTableResult;

// true when the newest cursor of the document's view is in a table.
bool
MarkdownInTable(Document *document);

// aligns the columns of the table at the newest cursor, as one undo step: each column is
// as wide as its widest cell, up to maxWidth columns, with the text left aligned and
// padded with spaces, and the delimiter row keeps its alignment colons.
// a body cell wider than maxWidth wraps at blanks (or, for a longer word, between
// characters) onto rows added below, whose other cells are empty.
// header cells never wrap, as a row added after the header would end the table's head.
// rows with fewer cells than the widest row get empty ones, every row takes the
// indentation (and any quote markers) of the header, and the cursor ends up at the start
// of the cell it was in.
// returns false, changing nothing, when the cursor is not in a table.
bool
MarkdownAlignTable(Document *document, u64 maxWidth, MarkdownTableResult *result);
