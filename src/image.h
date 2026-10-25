#pragma once

// image: a picture file opened to be looked at.
// its format is recognised from its first bytes and it is decoded into a bitmap once,
// which is drawn in sixels at whatever size a pane gives it: its own when it fits, or else
// shrunk to fit, keeping its shape.
#include "bitmap.h"
#include "file.h"
#include "strbuf.h"
#include "types.h"

// the image drawn for one area: its sixel sequence, its size in pixels and in cells, and a
// key no other rendering ever has, so the screen can tell when the picture changes.
typedef struct ImageRendering {
  StrBuf sixel;
  u32 width;
  u32 height;
  i32 columns;
  i32 rows;
  u64 key;

  // the share of the image's size it is drawn at, as a percentage to show: 100 when it is
  // cut to the area at its actual size, less when shrunk, and 0 when it is whole at its
  // own size.
  u32 scale;

  // the area it was made for, in cells, the size of a cell in pixels, and the part of the
  // image it shows when cut, from its top left pixel.
  i32 areaColumns;
  i32 areaRows;
  i32 cellWidth;
  i32 cellHeight;
  bool cut;
  u32 left;
  u32 top;
} ImageRendering;

typedef struct Image {
  Bitmap bitmap;

  // the format's name ("PNG"), what its decoder said of the pixels ("8-bit RGBA") and the
  // file's size in bytes.
  const char *format;
  StrBuf details;
  u64 fileSize;

  // shown at its actual size, cut to the area, rather than shrunk to fit; and then the
  // pixel at the top left of the part shown, for every view of it, and whether the part
  // shown is still to be put in the middle of the image, once its area is known.
  bool actualSize;
  u32 left;
  u32 top;
  bool centre;

  // the last two renderings, so both sides of a split can show the image at their own
  // sizes; next is the one made over next.
  ImageRendering renderings[2];
  u32 next;
} Image;

// opens the file at path as an image, giving the stamp of the version read.
// returns nullptr when it cannot be shown, with the reason appended to message: it is not
// in a format the editor knows (a binary file), it is damaged, or it is too large.
Image *
ImageOpen(const char *path, FileStamp *stamp, StrBuf *message);

// frees the image, which may be nullptr.
void
ImageFree(Image *image);

// the image drawn in an area of columns by rows cells, each cellWidth by cellHeight pixels:
// fitted to it, or at its actual size cut to it (with the part shown kept inside the
// image), with transparent pixels over a checkerboard.
// it is valid until the image is drawn for two other areas, or freed.
const ImageRendering *
ImageRender(Image *image, i32 columns, i32 rows, i32 cellWidth, i32 cellHeight);

// switches between fitting the image to its area and showing it at its actual size, from
// its middle, and returns whether it is now at its actual size.
bool
ImageToggleSize(Image *image);

// moves the part of an image shown at its actual size across and down by pixels, stopping
// at its edges; it does nothing while the image is fitted.
void
ImagePan(Image *image, i64 across, i64 down);

// appends the format and size, as "PNG 1920×1080".
void
ImageDescribe(const Image *image, StrBuf *text);

// appends the format, size, what the pixels are and the file's size, as
// "PNG 1920×1080, 8-bit RGBA, 2.4 MB".
void
ImageStatistics(const Image *image, StrBuf *text);
