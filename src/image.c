#include "image.h"

#include <stdlib.h>

#include "bmp.h"
#include "gif.h"
#include "memory.h"
#include "png.h"
#include "sixel.h"

// an image format: its name, whether a file's first bytes are its, and its decoder.
typedef struct Format {
  const char *name;
  bool (*is)(const u8 *data, u64 size);
  bool (*decode)(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message);
} Format;

static const Format formats[] =
  {{"PNG", PngIs, PngDecode}, {"GIF", GifIs, GifDecode}, {"BMP", BmpIs, BmpDecode}};

// sixel bands are six pixels tall and foot may round a picture's height up to a whole
// band, so this many pixels of an area are kept spare below it.
static constexpr u64 bandSlack = 5;

// the key of the last rendering made.
static u64 lastKey;

Image *
ImageOpen(const char *path, FileStamp *stamp, StrBuf *message)
{
  StrBuf bytes = {};
  enum FileStatus status = FileReadBytes(path, &bytes, stamp, message);
  if (status == FileStatusNew) StrBufAppendFormat(message, "%s: no longer exists", path);

  if (status != FileStatusOk) {
    StrBufFree(&bytes);
    return nullptr;
  }

  const u8 *data = (const u8 *)bytes.data;
  const Format *format = nullptr;

  for (u64 index = 0; index < sizeof(formats) / sizeof(formats[0]) && !format; index++) {
    if (formats[index].is(data, bytes.length)) format = &formats[index];
  }

  Image *image = nullptr;

  if (format) {
    image = MemoryAllocateZeroed(1, sizeof(Image));
    image->format = format->name;
    StrBuf reason = {};

    if (!format->decode(data, bytes.length, &image->bitmap, &image->details, &reason)) {
      StrBufAppendFormat(message, "%s: %s, not opened", path, reason.data);
      ImageFree(image);
      image = nullptr;
    }

    StrBufFree(&reason);
  } else {
    StrBufAppendFormat(message, "%s: binary file, not opened", path);
  }

  StrBufFree(&bytes);
  return image;
}

void
ImageFree(Image *image)
{
  if (!image) return;

  for (u32 index = 0; index < 2; index++) StrBufFree(&image->renderings[index].sixel);
  StrBufFree(&image->details);
  BitmapFree(&image->bitmap);
  free(image);
}

// the size a bitmap is drawn at in an area of width by height pixels: its own when it
// fits, or else the largest of the same shape that does.
static void
Fit(const Bitmap *bitmap, u64 areaWidth, u64 areaHeight, u32 *width, u32 *height)
{
  u64 fitWidth = bitmap->width;
  u64 fitHeight = bitmap->height;

  if (fitWidth > areaWidth) {
    fitWidth = areaWidth;
    fitHeight = (u64)bitmap->height * areaWidth / bitmap->width;
  }

  if (fitHeight > areaHeight) {
    fitWidth = (u64)bitmap->width * areaHeight / bitmap->height;
    fitHeight = areaHeight;
  }

  *width = fitWidth ? (u32)fitWidth : 1;
  *height = fitHeight ? (u32)fitHeight : 1;
}

const ImageRendering *
ImageRender(Image *image, i32 columns, i32 rows, i32 cellWidth, i32 cellHeight)
{
  for (u32 index = 0; index < 2; index++) {
    ImageRendering *made = &image->renderings[index];
    bool same = made->key && made->areaColumns == columns && made->areaRows == rows &&
      made->cellWidth == cellWidth && made->cellHeight == cellHeight;
    if (same) return made;
  }

  ImageRendering *rendering = &image->renderings[image->next];
  image->next ^= 1;

  rendering->areaColumns = columns;
  rendering->areaRows = rows;
  rendering->cellWidth = cellWidth;
  rendering->cellHeight = cellHeight;

  u64 areaWidth = (u64)columns * (u64)cellWidth;
  u64 areaHeight = (u64)rows * (u64)cellHeight;
  areaHeight = areaHeight > bandSlack ? areaHeight - bandSlack : 1;
  Fit(&image->bitmap, areaWidth, areaHeight, &rendering->width, &rendering->height);

  const Bitmap *bitmap = &image->bitmap;
  bool shrunk = rendering->width != bitmap->width || rendering->height != bitmap->height;

  if (shrunk) {
    Bitmap scaled;
    BitmapShrink(bitmap, rendering->width, rendering->height, &scaled);
    SixelEncode(&scaled, &rendering->sixel);
    BitmapFree(&scaled);
  } else {
    SixelEncode(bitmap, &rendering->sixel);
  }

  // the cells it covers, with its height rounded up to whole bands.
  u64 bandHeight = ((u64)rendering->height + 5) / 6 * 6;
  rendering->columns = (i32)(((u64)rendering->width + (u64)cellWidth - 1) / (u64)cellWidth);
  rendering->rows = (i32)((bandHeight + (u64)cellHeight - 1) / (u64)cellHeight);
  rendering->key = ++lastKey;
  return rendering;
}

void
ImageDescribe(const Image *image, StrBuf *text)
{
  const Bitmap *bitmap = &image->bitmap;
  StrBufAppendFormat(text, "%s %u×%u", image->format, bitmap->width, bitmap->height);
}
