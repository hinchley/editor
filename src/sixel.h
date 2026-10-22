#pragma once

// sixel: the sixel graphics sequence that draws a bitmap in foot.
// the colours are reduced to a palette of 255 picked for the bitmap by median cut, with
// Floyd-Steinberg error diffusion to hide the steps between them (unless the bitmap has
// no more colours than that), and pixels less than half opaque are left transparent.
#include "bitmap.h"
#include "strbuf.h"

// replaces what out held with the sequence that draws bitmap from the cursor, its top left
// corner in the cursor's cell.
void
SixelEncode(const Bitmap *bitmap, StrBuf *out);
