#pragma once

// json: reading a message into a flat list of nodes, and writing json into a string.
// a parsed message is its text plus one node per value, in the order they appear, so a
// container's children follow it and a node knows where the next value after it starts.
// an object's children come in pairs, a key (a string node) and its value.
// nodes are found by index, and jsonNone stands for a value that is not there, which
// every accessor takes, so a lookup can go several levels deep before checking.
#include "strbuf.h"
#include "types.h"

static constexpr u32 jsonNone = UINT32_MAX;

enum JsonType : u8 {
  JsonTypeNull,
  JsonTypeBool,
  JsonTypeNumber,
  JsonTypeString,
  JsonTypeArray,
  JsonTypeObject
};

typedef struct JsonNode {
  enum JsonType type;

  // the value's text, quotes included for a string.
  u32 start;
  u32 length;

  // how many values a container holds (an object's members, not its keys and values).
  u32 size;

  // the node after this one and everything inside it.
  u32 after;
} JsonNode;

typedef struct Json {
  const char *text;
  JsonNode *nodes;
  u32 count;
  u32 capacity;
} Json;

// parses text, which must outlive json, and returns false when it is not one json value.
// json may be parsed into again, reusing its nodes, and is released with JsonFree.
bool
JsonParse(Json *json, const char *text, u64 length);

void
JsonFree(Json *json);

// the type of node, or JsonTypeNull for jsonNone.
enum JsonType
JsonTypeOf(const Json *json, u32 node);

// the value of an object's member named key, or jsonNone.
u32
JsonGet(const Json *json, u32 node, const char *key);

// the number of values in an array or members in an object, or 0.
u32
JsonSize(const Json *json, u32 node);

// the first value of an array, or the first key of an object, or jsonNone.
u32
JsonFirst(const Json *json, u32 node);

// the value after an array value, or the key after an object member (given its key),
// within the container holding it; jsonNone after the last.
u32
JsonNext(const Json *json, u32 container, u32 node);

// an integer, or fallback when node is not a number that fits an i64 without a fraction.
i64
JsonInteger(const Json *json, u32 node, i64 fallback);

// true only for the literal true.
bool
JsonTrue(const Json *json, u32 node);

// appends the text of a string with its escapes turned back into bytes, and returns false
// (appending nothing) when node is not a string.
// a \u escape that is not a valid code point (a lone surrogate) becomes U+FFFD.
bool
JsonString(const Json *json, u32 node, StrBuf *text);

// true when node is a string equal to text, comparing the unescaped string.
bool
JsonEquals(const Json *json, u32 node, const char *text);

// appends text as a json string, in quotes, escaping what json requires.
// bytes that are not valid utf-8 are written as U+FFFD, so the result is always valid.
void
JsonAppendString(StrBuf *out, const char *text, u64 length);

// appends a node's text as it was, to pass a value on unchanged (a request's id).
void
JsonAppendRaw(StrBuf *out, const Json *json, u32 node);
