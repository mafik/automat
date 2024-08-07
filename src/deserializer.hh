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

maf::Str ToStr(JsonToken::TokenType);
maf::Str ToStr(const JsonToken&);

struct Deserializer {
  Deserializer(rapidjson::InsituStringStream&);
  void Get(maf::Str&, maf::Status&);
  void Get(double&, maf::Status&);
  void Get(float&, maf::Status&);
  void Get(int&, maf::Status&);
  void Get(bool&, maf::Status&);
  void Skip();

  maf::Str ErrorContext();

  rapidjson::InsituStringStream& stream;
  rapidjson::Reader reader;
  JsonToken token;

  char debug_path[256] = {};
  int debug_path_size = 0;

  maf::Str DebugPath() const { return maf::Str(debug_path, debug_path_size); }
  void DebugPut(char c) {
    if (debug_path_size < sizeof(debug_path)) {
      debug_path[debug_path_size++] = c;
    }
  }
};

// Provides begin and end iterators that can be used to iterate over the keys & fields of an object.
//
// This class also helps with Status management. It saves the first error that occurs and makes sure
// that its returned through the Status object after iteration ends. On every iteration cycle, it
// ensures that the Status object is clean.
//
// This limits any parsing errors to the property that had issues and allows other properties to be
// parsed correctly, while still providing the initial error message after iteration ends.
struct ObjectView {
  struct EndIterator {};
  struct Iterator {
    ObjectView& view;
    Iterator(ObjectView& view) : view(view) {}

    Iterator& operator++() {
      view.ReadKey();
      return *this;
    }
    bool operator!=(EndIterator) const { return !view.finished; }
    maf::Str& operator*() { return view.key; }
  };

  ObjectView(Deserializer&, maf::Status&);

  // Return `this` as an iterator for less verbose code.
  Iterator begin() { return *this; }
  EndIterator end() { return {}; }

  void ReadKey();

  maf::Str key;
  Deserializer& deserializer;
  bool finished = false;
  maf::Status& status;
  maf::Status first_issue;
  int debug_json_path_size;
};

struct ArrayView {
  struct EndIterator {};
  struct Iterator {
    ArrayView& view;
    Iterator(ArrayView& view) : view(view) {}

    Iterator& operator++() {
      view.Next();
      return *this;
    }
    bool operator!=(EndIterator) const { return !view.finished; }
    int operator*() { return view.i; }
  };

  ArrayView(Deserializer&, maf::Status&);

  Iterator begin() { return *this; }
  EndIterator end() { return {}; }

  void Next();

  Deserializer& deserializer;
  int i = 0;
  bool finished = false;
  maf::Status& status;
  maf::Status first_issue;
  int debug_json_path_size;
};

};  // namespace automat