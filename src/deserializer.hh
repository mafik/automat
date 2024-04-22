#pragma once

#include <rapidjson/reader.h>

#include "status.hh"

namespace automat {

struct Deserializer {
  Deserializer(rapidjson::StringStream&);
  double GetDouble(maf::Status&);
  bool GetBool(maf::Status&);

  rapidjson::StringStream& stream;
  rapidjson::Reader reader;
};

// Provides begin and end iterators that can be used to iterate over the keys & fields of an object.
struct ObjectView {
  struct EndIterator {};

  ObjectView(Deserializer&, maf::Status&);

  ObjectView& begin();
  EndIterator end() { return {}; }

  ObjectView& operator++();
  bool operator!=(EndIterator) const { return !finished; }
  maf::Str& operator*() { return key; }

  maf::Str key;
  Deserializer& deserializer;
  bool finished = false;
};

};  // namespace automat