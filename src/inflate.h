#pragma once

// inflate: decodes a zlib stream (RFC 1950 around RFC 1951 deflate), as git stores its
// objects.
// only decoding is supported, and the adler-32 check at the end is not verified.
#include "strbuf.h"
#include "types.h"

// appends the data decoded from the zlib stream in the size bytes at data to out.
// expected is the decoded size the caller knows from elsewhere (git's object headers), or
// 0 when it is not known; more output than expected is an error.
// returns false on a corrupt or truncated stream, with out holding what was decoded.
bool
InflateZlib(const u8 *data, u64 size, u64 expected, StrBuf *out);
