#pragma once

// matching pairs of brackets: (), [] and {}.
// brackets are matched by counting brackets of the same kind, without regard to strings
// or comments.
#include "buffer.h"
#include "types.h"

// finds the bracket matching the one at position.
// returns false when there is no bracket at position or it has no match.
bool
BracketMatch(const Buffer *buffer, Position position, Position *match);

// finds the closest pair of brackets around range: an opening bracket before range.start
// whose match is at or after range.end.
// pair receives the positions of the two brackets.
bool
BracketEnclosing(const Buffer *buffer, Range range, Range *pair);
