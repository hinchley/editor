#pragma once

// inflate: decodes a zlib stream (RFC 1950 around RFC 1951 deflate), as git stores its
// objects.
// only decoding is supported, and the adler-32 check at the end is not verified.
#include "strbuf.h"
#include "types.h"

// appends the data decoded from the zlib stream in the size bytes at data to out.
// limit is the most output allowed, or 0 for no limit; more is an error. callers that know
// the decoded size (git's pack entries, PNG pixels) pass it, and check the length after.
// returns false on a corrupt or truncated stream, with out holding what was decoded.
bool
InflateZlib(const u8 *data, u64 size, u64 limit, StrBuf *out);
