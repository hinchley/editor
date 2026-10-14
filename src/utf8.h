#pragma once

// utf-8 decoding and encoding, and code point display width.
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

// writes the utf-8 form of codepoint into output and returns its length (1 to 4).
u32
Utf8Encode(u32 codepoint, char output[4]);

// display width of a code point in columns: 0, 1 or 2.
// control characters report 1; callers decide how to show them.
u32
Utf8Width(u32 codepoint);
