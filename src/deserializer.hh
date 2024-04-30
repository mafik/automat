#pragma once

#include <rapidjson/prettywriter.h>
#include <rapidjson/reader.h>

#include "status.hh"

namespace automat {

using Serializer = rapidjson::PrettyWriter<rapidjson::StringBuffer>;

struct JsonToken {
  enum TokenType {
    kNoTokenType,
    kNullTokenType,
    kBooleanTokenType,
    kIntTokenType,
    kUintTokenType,
    kInt64TokenType,
    kUint64TokenType,
    kDoubleTokenType,
    kRawNumberTokenType,
    kStringTokenType,
    kStartObjectTokenType,
    kKeyTokenType,
    kEndObjectTokenType,
    kStartArrayTokenType,
    kEndArrayTokenType,
    kEndOfStreamTokenType,
  } type;
  union Union {
    Union() {}
    bool b;
    int i;
    unsigned u;
    int64_t i64;
    uint64_t u64;
    double d;
    maf::StrView raw_number;
    maf::StrView string;
    maf::StrView key;
  } value;

  JsonToken() : type(kNoTokenType) {}
};

maf::Str ToStr(JsonToken::TokenType type);

struct Deserializer {
  Deserializer(rapidjson::StringStream&);
  maf::Str GetString(maf::Status&);
  double GetDouble(maf::Status&);
  int GetInt(maf::Status&);
  bool GetBool(maf::Status&);

  maf::Str ErrorContext();

  rapidjson::StringStream& stream;
  rapidjson::Reader reader;
  JsonToken token;
};

// Provides begin and end iterators that can be used to iterate over the keys & fields of an object.
struct ObjectView {
  struct EndIterator {};

  ObjectView(Deserializer&, maf::Status&);

  // Return `this` as an iterator for less verbose code.
  ObjectView& begin();
  EndIterator end() { return {}; }

  ObjectView& operator++();
  bool operator!=(EndIterator) const { return !finished; }
  maf::Str& operator*() { return key; }

  maf::Str key;
  Deserializer& deserializer;
  bool finished = false;
};

struct ArrayView {
  struct EndIterator {};

  ArrayView(Deserializer&, maf::Status&);

  ArrayView& begin();
  EndIterator end() { return {}; }

  ArrayView& operator++();
  bool operator!=(EndIterator) const { return !finished; }
  int operator*() { return i; }

  Deserializer& deserializer;
  int i = 0;
  bool finished = false;
};

};  // namespace automat