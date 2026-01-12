// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/reader.h>

#include "status.hh"

namespace automat {

using Serializer = rapidjson::PrettyWriter<rapidjson::StringBuffer>;

using JsonValue = rapidjson::GenericValue<rapidjson::UTF8<>>;

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
    StrView raw_number;
    StrView string;
    StrView key;
  } value;

  JsonToken() : type(kNoTokenType) {}
};

Str ToStr(JsonToken::TokenType);
Str ToStr(const JsonToken&);

struct Deserializer {
  Deserializer(rapidjson::StringStream);
  void Get(Str&, Status&);
  void Get(double&, Status&);
  void Get(float&, Status&);
  void Get(int&, Status&);
  void Get(int64_t&, Status&);
  void Get(uint64_t&, Status&);
  void Get(bool&, Status&);
  void Skip();

  Str ErrorContext();

  rapidjson::StringStream stream;
  rapidjson::Reader reader;
  JsonToken token;

  char debug_path[256] = {};
  int debug_path_size = 0;

  Str DebugPath() const { return Str(debug_path, debug_path_size); }
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
    Str& operator*() { return view.key; }
  };

  ObjectView(Deserializer&, Status&);

  // Return `this` as an iterator for less verbose code.
  Iterator begin() { return *this; }
  EndIterator end() { return {}; }

  void ReadKey();

  Str key;
  Deserializer& deserializer;
  bool finished = false;
  Status& status;
  Status first_issue;
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

  ArrayView(Deserializer&, Status&);

  Iterator begin() { return *this; }
  EndIterator end() { return {}; }

  void Next();

  Deserializer& deserializer;
  int i = 0;
  bool finished = false;
  Status& status;
  Status first_issue;
  int debug_json_path_size;
};

};  // namespace automat
