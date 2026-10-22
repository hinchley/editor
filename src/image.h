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

  // the area it was made for, in cells, and the size of a cell in pixels.
  i32 areaColumns;
  i32 areaRows;
  i32 cellWidth;
  i32 cellHeight;
} ImageRendering;

typedef struct Image {
  Bitmap bitmap;

  // the format's name ("PNG") and what its decoder said of the pixels ("8-bit RGBA").
  const char *format;
  StrBuf details;

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

// the image drawn in an area of columns by rows cells, each cellWidth by cellHeight pixels.
// it is valid until the image is drawn for two other areas, or freed.
const ImageRendering *
ImageRender(Image *image, i32 columns, i32 rows, i32 cellWidth, i32 cellHeight);

// appends the format and size, as "PNG 1920×1080".
void
ImageDescribe(const Image *image, StrBuf *text);
