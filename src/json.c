#include "json.h"

#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "utf8.h"

// containers nested deeper than this are refused, which bounds the recursion.
static constexpr u32 maxDepth = 256;

typedef struct Parser {
  Json *json;
  const char *text;
  u32 length;
  u32 at;
} Parser;

static void
SkipBlanks(Parser *parser)
{
  while (parser->at < parser->length) {
    char byte = parser->text[parser->at];
    if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') return;
    parser->at++;
  }
}

static u32
AddNode(Parser *parser, enum JsonType type)
{
  Json *json = parser->json;

  if (json->count == json->capacity) {
    json->capacity = json->capacity ? json->capacity * 2 : 64;
    json->nodes = MemoryResize(json->nodes, json->capacity, sizeof(JsonNode));
  }

  json->nodes[json->count] = (JsonNode){.type = type, .start = parser->at};
  return json->count++;
}

static bool
IsDigit(char byte)
{
  return byte >= '0' && byte <= '9';
}

static bool
IsHex(char byte)
{
  return IsDigit(byte) || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

static bool
ParseLiteral(Parser *parser, const char *word)
{
  u32 length = (u32)strlen(word);
  if (parser->length - parser->at < length) return false;
  if (memcmp(parser->text + parser->at, word, length) != 0) return false;

  parser->at += length;
  return true;
}

static bool
ParseString(Parser *parser)
{
  // the opening quote.
  parser->at++;

  while (parser->at < parser->length) {
    char byte = parser->text[parser->at++];
    if (byte == '"') return true;
    if ((u8)byte < 0x20) return false;
    if (byte != '\\') continue;
    if (parser->at == parser->length) return false;

    char escape = parser->text[parser->at++];
    if (strchr("\"\\/bfnrt", escape)) continue;
    if (escape != 'u' || parser->length - parser->at < 4) return false;

    for (u32 digit = 0; digit < 4; digit++) {
      if (!IsHex(parser->text[parser->at++])) return false;
    }
  }

  return false;
}

static bool
ParseNumber(Parser *parser)
{
  const char *text = parser->text;
  u32 length = parser->length;
  u32 start = parser->at;
  if (parser->at < length && text[parser->at] == '-') parser->at++;
  if (parser->at == length || !IsDigit(text[parser->at])) return false;
  while (parser->at < length && IsDigit(text[parser->at])) parser->at++;

  if (parser->at < length && text[parser->at] == '.') {
    parser->at++;
    if (parser->at == length || !IsDigit(text[parser->at])) return false;
    while (parser->at < length && IsDigit(text[parser->at])) parser->at++;
  }

  if (parser->at < length && (text[parser->at] == 'e' || text[parser->at] == 'E')) {
    parser->at++;
    if (parser->at < length && (text[parser->at] == '+' || text[parser->at] == '-')) parser->at++;
    if (parser->at == length || !IsDigit(text[parser->at])) return false;
    while (parser->at < length && IsDigit(text[parser->at])) parser->at++;
  }

  return parser->at > start;
}

static bool ParseValue(Parser *parser, u32 depth);

// parses the values of an array, or the members of an object, up to its closing bracket.
static bool
ParseContainer(Parser *parser, u32 node, bool object, u32 depth)
{
  char close = object ? '}' : ']';
  parser->at++;
  SkipBlanks(parser);

  if (parser->at < parser->length && parser->text[parser->at] == close) {
    parser->at++;
    return true;
  }

  while (true) {
    if (object) {
      SkipBlanks(parser);
      if (parser->at == parser->length || parser->text[parser->at] != '"') return false;

      u32 key = AddNode(parser, JsonTypeString);
      if (!ParseString(parser)) return false;
      parser->json->nodes[key].length = parser->at - parser->json->nodes[key].start;
      parser->json->nodes[key].after = parser->json->count;

      SkipBlanks(parser);
      if (parser->at == parser->length || parser->text[parser->at] != ':') return false;
      parser->at++;
    }

    if (!ParseValue(parser, depth + 1)) return false;
    parser->json->nodes[node].size++;
    SkipBlanks(parser);
    if (parser->at == parser->length) return false;

    char byte = parser->text[parser->at++];
    if (byte == close) return true;
    if (byte != ',') return false;
  }
}

static bool
ParseValue(Parser *parser, u32 depth)
{
  if (depth > maxDepth) return false;

  SkipBlanks(parser);
  if (parser->at == parser->length) return false;

  char byte = parser->text[parser->at];
  enum JsonType type = JsonTypeNumber;
  if (byte == '"') type = JsonTypeString;
  else if (byte == '[') type = JsonTypeArray;
  else if (byte == '{') type = JsonTypeObject;
  else if (byte == 't' || byte == 'f') type = JsonTypeBool;
  else if (byte == 'n') type = JsonTypeNull;

  u32 node = AddNode(parser, type);
  bool parsed = false;

  switch (type) {
  case JsonTypeNull: parsed = ParseLiteral(parser, "null"); break;
  case JsonTypeBool: parsed = ParseLiteral(parser, byte == 't' ? "true" : "false"); break;
  case JsonTypeNumber: parsed = ParseNumber(parser); break;
  case JsonTypeString: parsed = ParseString(parser); break;
  case JsonTypeArray: parsed = ParseContainer(parser, node, false, depth); break;
  case JsonTypeObject: parsed = ParseContainer(parser, node, true, depth); break;
  }

  // the nodes may have moved while the children were added.
  JsonNode *added = &parser->json->nodes[node];
  added->length = parser->at - added->start;
  added->after = parser->json->count;
  return parsed;
}

bool
JsonParse(Json *json, const char *text, u64 length)
{
  json->text = text;
  json->count = 0;
  if (length >= UINT32_MAX) return false;

  Parser parser = {.json = json, .text = text, .length = (u32)length};
  if (!ParseValue(&parser, 0)) return false;

  SkipBlanks(&parser);
  return parser.at == parser.length;
}

void
JsonFree(Json *json)
{
  free(json->nodes);
  *json = (Json){};
}

enum JsonType
JsonTypeOf(const Json *json, u32 node)
{
  return node < json->count ? json->nodes[node].type : JsonTypeNull;
}

u32
JsonSize(const Json *json, u32 node)
{
  enum JsonType type = JsonTypeOf(json, node);
  return type == JsonTypeArray || type == JsonTypeObject ? json->nodes[node].size : 0;
}

u32
JsonFirst(const Json *json, u32 node)
{
  return JsonSize(json, node) ? node + 1 : jsonNone;
}

u32
JsonNext(const Json *json, u32 container, u32 node)
{
  if (node >= json->count) return jsonNone;

  // a member is its key and its value, so the next key follows the value.
  u32 next = json->nodes[node].after;
  if (json->nodes[container].type == JsonTypeObject) next = json->nodes[next].after;
  return next < json->nodes[container].after ? next : jsonNone;
}

u32
JsonGet(const Json *json, u32 node, const char *key)
{
  if (JsonTypeOf(json, node) != JsonTypeObject) return jsonNone;

  for (u32 at = JsonFirst(json, node); at != jsonNone; at = JsonNext(json, node, at)) {
    if (JsonEquals(json, at, key)) return at + 1;
  }

  return jsonNone;
}

i64
JsonInteger(const Json *json, u32 node, i64 fallback)
{
  if (JsonTypeOf(json, node) != JsonTypeNumber) return fallback;

  const JsonNode *number = &json->nodes[node];
  const char *text = json->text + number->start;
  bool negative = text[0] == '-';
  u64 value = 0;

  for (u32 at = negative; at < number->length; at++) {
    if (!IsDigit(text[at])) return fallback;
    if (value > (u64)INT64_MAX / 10) return fallback;
    value = value * 10 + (u64)(text[at] - '0');
  }

  if (value > (u64)INT64_MAX) return fallback;
  return negative ? -(i64)value : (i64)value;
}

bool
JsonTrue(const Json *json, u32 node)
{
  return JsonTypeOf(json, node) == JsonTypeBool && json->text[json->nodes[node].start] == 't';
}

static u32
HexValue(const char *text)
{
  u32 value = 0;

  for (u32 at = 0; at < 4; at++) {
    char digit = text[at];
    u32 nibble = IsDigit(digit) ? (u32)(digit - '0') : (u32)((digit | 0x20) - 'a' + 10);
    value = value << 4 | nibble;
  }

  return value;
}

// the code point of the \u escape at text (after the backslash and u), joining a
// surrogate pair, and how many bytes it used from text.
static u32
DecodeEscape(const char *text, const char *end, u32 *used)
{
  u32 value = HexValue(text);
  *used = 4;
  if (value < 0xD800 || value > 0xDFFF) return value;
  if (value > 0xDBFF) return utf8Replacement;

  // a high surrogate counts only with a low one after it.
  bool pair = end - text >= 10 && text[4] == '\\' && text[5] == 'u';
  u32 low = pair ? HexValue(text + 6) : 0;
  if (low < 0xDC00 || low > 0xDFFF) return utf8Replacement;

  *used = 10;
  return 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
}

bool
JsonString(const Json *json, u32 node, StrBuf *text)
{
  if (JsonTypeOf(json, node) != JsonTypeString) return false;

  const JsonNode *string = &json->nodes[node];
  const char *at = json->text + string->start + 1;
  const char *end = json->text + string->start + string->length - 1;

  while (at < end) {
    const char *backslash = memchr(at, '\\', (u64)(end - at));
    const char *stop = backslash ? backslash : end;
    StrBufAppend(text, at, (u64)(stop - at));
    if (!backslash) break;

    char escape = backslash[1];
    at = backslash + 2;

    switch (escape) {
    case 'b': StrBufAppendByte(text, '\b'); break;
    case 'f': StrBufAppendByte(text, '\f'); break;
    case 'n': StrBufAppendByte(text, '\n'); break;
    case 'r': StrBufAppendByte(text, '\r'); break;
    case 't': StrBufAppendByte(text, '\t'); break;

    case 'u': {
      u32 used;
      char encoded[4];
      u32 codepoint = DecodeEscape(at, end, &used);
      StrBufAppend(text, encoded, Utf8Encode(codepoint, encoded));
      at += used;
      break;
    }

    // a quote, backslash or slash stands for itself.
    default: StrBufAppendByte(text, escape); break;
    }
  }

  return true;
}

bool
JsonEquals(const Json *json, u32 node, const char *text)
{
  if (JsonTypeOf(json, node) != JsonTypeString) return false;

  // strings without escapes, which keys and method names are, compare in place.
  const JsonNode *string = &json->nodes[node];
  const char *contents = json->text + string->start + 1;
  u64 length = string->length - 2;

  if (!memchr(contents, '\\', length)) {
    return strlen(text) == length && memcmp(contents, text, length) == 0;
  }

  StrBuf unescaped = {};
  JsonString(json, node, &unescaped);
  u64 size = unescaped.length;
  bool equal = size == strlen(text) && memcmp(unescaped.data, text, size) == 0;
  StrBufFree(&unescaped);
  return equal;
}

void
JsonAppendString(StrBuf *out, const char *text, u64 length)
{
  StrBufReserve(out, out->length + length + 2);
  StrBufAppendByte(out, '"');
  u64 run = 0;

  for (u64 at = 0; at < length;) {
    u8 byte = (u8)text[at];
    bool plain = byte >= 0x20 && byte < 0x80 && byte != '"' && byte != '\\';

    if (plain) {
      at++;
      continue;
    }

    StrBufAppend(out, text + run, at - run);

    if (byte >= 0x80) {
      // a valid sequence is copied as it is, and an invalid byte becomes U+FFFD.
      u32 codepoint;
      u32 used = Utf8Decode(text + at, length - at, &codepoint);
      bool valid = codepoint != utf8Replacement || (used == 3 && (u8)text[at] == 0xEF);
      if (valid) StrBufAppend(out, text + at, used);
      else StrBufAppendString(out, "\xEF\xBF\xBD");
      at += used;
    } else {
      switch (byte) {
      case '"': StrBufAppendString(out, "\\\""); break;
      case '\\': StrBufAppendString(out, "\\\\"); break;
      case '\n': StrBufAppendString(out, "\\n"); break;
      case '\r': StrBufAppendString(out, "\\r"); break;
      case '\t': StrBufAppendString(out, "\\t"); break;
      default: StrBufAppendFormat(out, "\\u%04x", byte); break;
      }

      at++;
    }

    run = at;
  }

  StrBufAppend(out, text + run, length - run);
  StrBufAppendByte(out, '"');
}

void
JsonAppendRaw(StrBuf *out, const Json *json, u32 node)
{
  if (node >= json->count) {
    StrBufAppendString(out, "null");
    return;
  }

  StrBufAppend(out, json->text + json->nodes[node].start, json->nodes[node].length);
}
