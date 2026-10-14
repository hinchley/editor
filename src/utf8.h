#pragma once

// utf-8 decoding and encoding, grapheme clusters, display width and case folding.
// text is any byte sequence: an invalid byte is treated as one U+FFFD code point, so
// every byte belongs to exactly one code point and text is never altered.
#include "types.h"

// the code point substituted for each byte of an invalid sequence.
static constexpr u32 utf8Replacement = 0xFFFD;

// length of the sequence started by a lead byte, or 0 when the byte cannot start one.
u32
Utf8SequenceLength(u8 lead);

// decodes the code point at the start of text into codepoint.
// returns the number of bytes used, which is at least 1 when length is not 0.
// an invalid or truncated sequence yields utf8Replacement and uses one byte.
u32
Utf8Decode(const char *text, u64 length, u32 *codepoint);

// start of the code point that ends at offset at (which must be greater than 0).
// agrees with Utf8Decode, so stepping back and then forward lands on the same offsets.
u64
Utf8PreviousStart(const char *text, u64 at);

// writes the utf-8 form of codepoint into output and returns its length (1 to 4).
u32
Utf8Encode(u32 codepoint, char output[4]);

// display width of a code point in columns: 0, 1 or 2.
// control characters report 1; callers decide how to show them.
u32
Utf8Width(u32 codepoint);

// offset of the grapheme cluster boundary after at, or length when at is in the last one.
// uses the extended grapheme cluster rules, without the indic conjunct rule.
u64
Utf8NextBoundary(const char *text, u64 length, u64 at);

// offset of the grapheme cluster boundary before at, or 0.
u64
Utf8PreviousBoundary(const char *text, u64 length, u64 at);

// display width of one grapheme cluster, given exactly its bytes.
// it is the width of the first code point, except that a regional indicator pair, and a
// pictographic character followed by the emoji presentation selector, are 2 columns.
// a cluster of only zero width code points reports 0; callers decide how to show it.
u32
Utf8ClusterWidth(const char *text, u64 length);

// simple case folding of one code point, used for case insensitive comparison.
u32
Utf8Fold(u32 codepoint);
