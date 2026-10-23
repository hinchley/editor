#include "image.h"

#include <malloc.h>
#include <stdlib.h>

#include "bmp.h"
#include "gif.h"
#include "jpeg.h"
#include "memory.h"
#include "png.h"
#include "sixel.h"

// an image format: its name, whether a file's first bytes are its, and its decoder.
typedef struct Format {
  const char *name;
  bool (*is)(const u8 *data, u64 size);
  bool (*decode)(const u8 *data, u64 size, Bitmap *bitmap, StrBuf *details, StrBuf *message);
} Format;

static const Format formats[] = {
  {"PNG", PngIs, PngDecode},
  {"GIF", GifIs, GifDecode},
  {"BMP", BmpIs, BmpDecode},
  {"JPEG", JpegIs, JpegDecode}
};

// sixel bands are six pixels tall and foot may round a picture's height up to a whole
// band, so this many pixels of an area are kept spare below it.
static constexpr u64 bandSlack = 5;

// the side of a square of the checkerboard drawn behind transparency, in pixels.
static constexpr u32 checkerSize = 8;

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
    image->fileSize = bytes.length;
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

  // a decoder frees large buffers as it goes, which makes glibc keep later ones in the heap
  // rather than hand them back when freed.
  malloc_trim(0);
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

// true when a rendering was made for the same area, cell size and part of the image.
static bool
SameRendering(const ImageRendering *made, const ImageRendering *wanted)
{
  return made->key && made->areaColumns == wanted->areaColumns &&
    made->areaRows == wanted->areaRows && made->cellWidth == wanted->cellWidth &&
    made->cellHeight == wanted->cellHeight && made->cut == wanted->cut &&
    made->left == wanted->left && made->top == wanted->top;
}

const ImageRendering *
ImageRender(Image *image, i32 columns, i32 rows, i32 cellWidth, i32 cellHeight)
{
  const Bitmap *bitmap = &image->bitmap;
  u64 areaWidth = (u64)columns * (u64)cellWidth;
  u64 areaHeight = (u64)rows * (u64)cellHeight;
  areaHeight = areaHeight > bandSlack ? areaHeight - bandSlack : 1;

  // at its actual size the image is cut to the area when it is larger, and the part shown
  // kept inside it.
  bool cut = image->actualSize && (bitmap->width > areaWidth || bitmap->height > areaHeight);
  u32 shownWidth = (u32)(bitmap->width < areaWidth ? bitmap->width : areaWidth);
  u32 shownHeight = (u32)(bitmap->height < areaHeight ? bitmap->height : areaHeight);

  if (cut) {
    if (image->left > bitmap->width - shownWidth) image->left = bitmap->width - shownWidth;
    if (image->top > bitmap->height - shownHeight) image->top = bitmap->height - shownHeight;
  }

  ImageRendering wanted = {
    .areaColumns = columns,
    .areaRows = rows,
    .cellWidth = cellWidth,
    .cellHeight = cellHeight,
    .cut = cut,
    .left = cut ? image->left : 0,
    .top = cut ? image->top : 0
  };

  for (u32 index = 0; index < 2; index++) {
    if (SameRendering(&image->renderings[index], &wanted)) return &image->renderings[index];
  }

  ImageRendering *rendering = &image->renderings[image->next];
  image->next ^= 1;
  wanted.sixel = rendering->sixel;
  *rendering = wanted;

  // the pixels drawn: the part shown, the image shrunk, or the image itself.
  Bitmap working = {};
  const Bitmap *drawn = bitmap;

  if (cut) {
    BitmapCrop(bitmap, wanted.left, wanted.top, shownWidth, shownHeight, &working);
    drawn = &working;
    rendering->scale = 100;
  } else {
    u32 width;
    u32 height;
    Fit(bitmap, areaWidth, areaHeight, &width, &height);

    if (width != bitmap->width || height != bitmap->height) {
      BitmapShrink(bitmap, width, height, &working);
      drawn = &working;
      u64 scale = (u64)width * 100 / bitmap->width;
      rendering->scale = scale ? (u32)scale : 1;
    }
  }

  // transparency is drawn over a checkerboard, on a copy when the image itself is drawn.
  if (drawn->alpha) {
    if (drawn == bitmap) BitmapCrop(bitmap, 0, 0, bitmap->width, bitmap->height, &working);
    BitmapFlatten(&working, checkerSize);
    drawn = &working;
  }

  SixelEncode(drawn, &rendering->sixel);
  rendering->width = drawn->width;
  rendering->height = drawn->height;
  BitmapFree(&working);

  // the cells it covers, with its height rounded up to whole bands.
  u64 bandHeight = ((u64)rendering->height + 5) / 6 * 6;
  rendering->columns = (i32)(((u64)rendering->width + (u64)cellWidth - 1) / (u64)cellWidth);
  rendering->rows = (i32)((bandHeight + (u64)cellHeight - 1) / (u64)cellHeight);
  rendering->key = ++lastKey;
  return rendering;
}

bool
ImageToggleSize(Image *image)
{
  image->actualSize = !image->actualSize;
  image->left = 0;
  image->top = 0;
  return image->actualSize;
}

// moves a position along an edge of size pixels by step, keeping it inside.
static u32
Move(u32 position, i64 step, u32 size)
{
  i64 moved = (i64)position + step;
  if (moved < 0) return 0;
  return moved >= size ? size - 1 : (u32)moved;
}

void
ImagePan(Image *image, i64 across, i64 down)
{
  if (!image->actualSize) return;

  // the part shown is kept inside the image again when it is next drawn, for its area.
  image->left = Move(image->left, across, image->bitmap.width);
  image->top = Move(image->top, down, image->bitmap.height);
}

void
ImageDescribe(const Image *image, StrBuf *text)
{
  const Bitmap *bitmap = &image->bitmap;
  StrBufAppendFormat(text, "%s %u×%u", image->format, bitmap->width, bitmap->height);
}

void
ImageStatistics(const Image *image, StrBuf *text)
{
  ImageDescribe(image, text);
  if (image->details.length) StrBufAppendFormat(text, ", %s", image->details.data);

  u64 size = image->fileSize;
  if (size >= 1024 * 1024) StrBufAppendFormat(text, ", %.1f MB", (f64)size / (1024 * 1024));
  else if (size >= 1024) StrBufAppendFormat(text, ", %.1f KB", (f64)size / 1024);
  else StrBufAppendFormat(text, ", %lu bytes", size);
}
