#include "deserializer.hh"

#include <cmath>

using namespace maf;
using namespace rapidjson;

namespace automat {

struct DeserializeHandler {
  enum TokenType {
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

  DeserializeHandler() : type(kNullTokenType) {}

  bool Null() {
    type = kNullTokenType;
    return true;
  }
  bool Bool(bool b) {
    type = kBooleanTokenType;
    value.b = b;
    return true;
  }
  bool Int(int i) {
    type = kIntTokenType;
    value.i = i;
    return true;
  }
  bool Uint(unsigned u) {
    type = kUintTokenType;
    value.u = u;
    return true;
  }
  bool Int64(int64_t i64) {
    type = kInt64TokenType;
    value.i64 = i64;
    return true;
  }
  bool Uint64(uint64_t u64) {
    type = kUint64TokenType;
    value.u64 = u64;
    return true;
  }
  bool Double(double d) {
    type = kDoubleTokenType;
    value.d = d;
    return true;
  }
  bool RawNumber(const char* str, SizeType length, bool copy) {
    type = kRawNumberTokenType;
    value.raw_number = StrView(str, length);
    return true;
  }
  bool String(const char* str, SizeType length, bool copy) {
    type = kStringTokenType;
    value.string = StrView(str, length);
    return true;
  }
  bool StartObject() {
    type = kStartObjectTokenType;
    return true;
  }
  bool Key(const char* str, SizeType length, bool copy) {
    type = kKeyTokenType;
    value.key = StrView(str, length);
    return true;
  }
  bool EndObject(SizeType memberCount) {
    type = kEndObjectTokenType;
    return true;
  }
  bool StartArray() {
    type = kStartArrayTokenType;
    return true;
  }
  bool EndArray(SizeType elementCount) {
    type = kEndArrayTokenType;
    return true;
  }
};

static void RecoverParser(rapidjson::Reader& reader, DeserializeHandler& handler,
                          StringStream& stream) {
  int n_objects = 0;
  int n_arrays = 0;
  if (handler.type == DeserializeHandler::kStartObjectTokenType) {
    n_objects = 1;
  } else if (handler.type == DeserializeHandler::kStartArrayTokenType) {
    n_arrays = 1;
  }
  while (n_objects + n_arrays > 0) {
    DeserializeHandler handler;
    reader.IterativeParseNext<kParseNoFlags>(stream, handler);
    if (handler.type == DeserializeHandler::kStartObjectTokenType) {
      n_objects++;
    } else if (handler.type == DeserializeHandler::kStartArrayTokenType) {
      n_arrays++;
    } else if (handler.type == DeserializeHandler::kEndObjectTokenType) {
      n_objects--;
    } else if (handler.type == DeserializeHandler::kEndArrayTokenType) {
      n_arrays--;
    }
  }
}

Deserializer::Deserializer(StringStream& stream) : stream(stream) { reader.IterativeParseInit(); }
double Deserializer::GetDouble(Status& status) {
  DeserializeHandler handler;
  reader.IterativeParseNext<kParseNoFlags>(stream, handler);
  if (handler.type != DeserializeHandler::kDoubleTokenType) {
    AppendErrorMessage(status) += "Expected a double";
    RecoverParser(reader, handler, stream);
    return NAN;
  }
  return handler.value.d;
}
bool Deserializer::GetBool(Status& status) {
  DeserializeHandler handler;
  reader.IterativeParseNext<kParseNoFlags>(stream, handler);
  if (handler.type != DeserializeHandler::kBooleanTokenType) {
    AppendErrorMessage(status) += "Expected a boolean";
    RecoverParser(reader, handler, stream);
    return false;
  }
  return handler.value.b;
}

ObjectView::ObjectView(Deserializer& deserializer, maf::Status& status)
    : deserializer(deserializer) {
  DeserializeHandler handler;
  deserializer.reader.IterativeParseNext<kParseNoFlags>(deserializer.stream, handler);
  if (handler.type != DeserializeHandler::kStartObjectTokenType) {
    AppendErrorMessage(status) += "Expected an object";
    RecoverParser(deserializer.reader, handler, deserializer.stream);
  }
  if (!OK(status)) {
    AppendErrorMessage(status) += "Expected '{'";
  }
  ++*this;
}

ObjectView& ObjectView::begin() { return *this; }

ObjectView& ObjectView::operator++() {
  DeserializeHandler handler;
  deserializer.reader.IterativeParseNext<kParseNoFlags>(deserializer.stream, handler);
  if (handler.type == DeserializeHandler::kEndObjectTokenType) {
    finished = true;
    return *this;
  } else if (handler.type == DeserializeHandler::kKeyTokenType) {
    key = Str(handler.value.key);
    return *this;
  } else {
    // append an error message maybe?
    finished = true;
    return *this;
  }
}

}  // namespace automat