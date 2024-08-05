#include "deserializer.hh"

#include <cmath>

#include "format.hh"
#include "log.hh"

using namespace maf;
using namespace rapidjson;

namespace automat {

Str ToStr(JsonToken::TokenType type) {
  switch (type) {
    case JsonToken::kNullTokenType:
      return "NullToken";
    case JsonToken::kBooleanTokenType:
      return "BooleanToken";
    case JsonToken::kIntTokenType:
      return "IntToken";
    case JsonToken::kUintTokenType:
      return "UintToken";
    case JsonToken::kInt64TokenType:
      return "Int64Token";
    case JsonToken::kUint64TokenType:
      return "Uint64Token";
    case JsonToken::kDoubleTokenType:
      return "DoubleToken";
    case JsonToken::kRawNumberTokenType:
      return "RawNumberToken";
    case JsonToken::kStringTokenType:
      return "StringToken";
    case JsonToken::kStartObjectTokenType:
      return "StartObjectToken";
    case JsonToken::kKeyTokenType:
      return "KeyToken";
    case JsonToken::kEndObjectTokenType:
      return "EndObjectToken";
    case JsonToken::kStartArrayTokenType:
      return "StartArrayToken";
    case JsonToken::kEndArrayTokenType:
      return "EndArrayToken";
    default:
      return "UnknownToken";
  }
}

using enum JsonToken::TokenType;

struct Handler {
  Deserializer& deserializer;

  Handler(Deserializer& deserializer) : deserializer(deserializer) {
    if (deserializer.token.type != kNoTokenType) {
      FATAL << "Attempting to parse the next token while the previous one ("
            << ToStr(deserializer.token.type) << ") wasn't consumed";
    }
  }

  bool Null() {
    deserializer.token.type = kNullTokenType;
    return true;
  }
  bool Bool(bool b) {
    deserializer.token.type = kBooleanTokenType;
    deserializer.token.value.b = b;
    return true;
  }
  bool Int(int i) {
    deserializer.token.type = kIntTokenType;
    deserializer.token.value.i = i;
    return true;
  }
  bool Uint(unsigned u) {
    deserializer.token.type = kUintTokenType;
    deserializer.token.value.u = u;
    return true;
  }
  bool Int64(int64_t i64) {
    deserializer.token.type = kInt64TokenType;
    deserializer.token.value.i64 = i64;
    return true;
  }
  bool Uint64(uint64_t u64) {
    deserializer.token.type = kUint64TokenType;
    deserializer.token.value.u64 = u64;
    return true;
  }
  bool Double(double d) {
    deserializer.token.type = kDoubleTokenType;
    deserializer.token.value.d = d;
    return true;
  }
  bool RawNumber(const char* str, SizeType length, bool copy) {
    deserializer.token.type = kRawNumberTokenType;
    deserializer.token.value.raw_number = StrView(str, length);
    return true;
  }
  bool String(const char* str, rapidjson::SizeType length, bool copy) {
    deserializer.token.type = kStringTokenType;
    deserializer.token.value.string = maf::StrView(str, length);
    return true;
  }
  bool StartObject() {
    deserializer.token.type = kStartObjectTokenType;
    return true;
  }
  bool Key(const char* str, rapidjson::SizeType length, bool copy) {
    deserializer.token.type = kKeyTokenType;
    deserializer.token.value.key = maf::StrView(str, length);
    return true;
  }
  bool EndObject(rapidjson::SizeType memberCount) {
    deserializer.token.type = kEndObjectTokenType;
    return true;
  }
  bool StartArray() {
    deserializer.token.type = kStartArrayTokenType;
    return true;
  }
  bool EndArray(rapidjson::SizeType elementCount) {
    deserializer.token.type = kEndArrayTokenType;
    return true;
  }
};

static void RecoverParser(Deserializer& d) {
  int n_objects = 0;
  int n_arrays = 0;
  if (d.token.type == JsonToken::kStartObjectTokenType) {
    n_objects = 1;
  } else if (d.token.type == JsonToken::kStartArrayTokenType) {
    n_arrays = 1;
  }
  while (n_objects + n_arrays > 0) {
    d.token.type = kNoTokenType;
    Handler handler(d);
    d.reader.IterativeParseNext<kParseNoFlags>(d.stream, handler);
    if (d.token.type == JsonToken::kStartObjectTokenType) {
      n_objects++;
    } else if (d.token.type == JsonToken::kStartArrayTokenType) {
      n_arrays++;
    } else if (d.token.type == JsonToken::kEndObjectTokenType) {
      n_objects--;
    } else if (d.token.type == JsonToken::kEndArrayTokenType) {
      n_arrays--;
    }
  }
}

static void FillToken(Deserializer& d) {
  if (d.token.type == kNoTokenType) {
    Handler handler(d);
    d.reader.IterativeParseNext<kParseNoFlags>(d.stream, handler);
  }
}

Deserializer::Deserializer(InsituStringStream& stream) : stream(stream) {
  reader.IterativeParseInit();
}

maf::Str Deserializer::GetString(maf::Status& status) {
  FillToken(*this);
  if (token.type == kStringTokenType) {
    token.type = kNoTokenType;
    return Str(token.value.string);
  } else {
    AppendErrorMessage(status) += "Expected a string but got " + ToStr(token.type);
    RecoverParser(*this);
    return "";
  }
}
double Deserializer::GetDouble(Status& status) {
  FillToken(*this);
  if (token.type == kDoubleTokenType) {
    token.type = kNoTokenType;
    return token.value.d;
  } else {
    AppendErrorMessage(status) += "Expected a double but got " + ToStr(token.type);
    RecoverParser(*this);
    return NAN;
  }
}
int Deserializer::GetInt(Status& status) {
  FillToken(*this);
  if (token.type == JsonToken::kIntTokenType) {
    token.type = kNoTokenType;
    return token.value.i;
  } else if (token.type == JsonToken::kUintTokenType) {
    token.type = kNoTokenType;
    return token.value.u;
  } else {
    AppendErrorMessage(status) += "Expected an integer but got " + ToStr(token.type);
    RecoverParser(*this);
    return 0;
  }
}
bool Deserializer::GetBool(Status& status) {
  FillToken(*this);
  if (token.type != JsonToken::kBooleanTokenType) {
    AppendErrorMessage(status) += "Expected a boolean";
    RecoverParser(*this);
    return false;
  }
  token.type = kNoTokenType;
  return token.value.b;
}

ObjectView::ObjectView(Deserializer& deserializer, maf::Status& status)
    : deserializer(deserializer) {
  FillToken(deserializer);
  if (deserializer.token.type != JsonToken::kStartObjectTokenType) {
    AppendErrorMessage(status) += "Expected an object but got " + ToStr(deserializer.token.type);
    RecoverParser(deserializer);
    finished = true;
  }
  deserializer.token.type = kNoTokenType;
  ++*this;
}

ObjectView& ObjectView::begin() { return *this; }

ObjectView& ObjectView::operator++() {
  FillToken(deserializer);
  if (deserializer.token.type == JsonToken::kEndObjectTokenType) {
    deserializer.token.type = kNoTokenType;
    finished = true;
  } else if (deserializer.token.type == JsonToken::kKeyTokenType) {
    deserializer.token.type = kNoTokenType;
    key = Str(deserializer.token.value.key);
  } else {
    // append an error message maybe?
    finished = true;
  }
  return *this;
}

ArrayView::ArrayView(Deserializer& deserializer, maf::Status& status) : deserializer(deserializer) {
  FillToken(deserializer);
  if (deserializer.token.type != JsonToken::kStartArrayTokenType) {
    AppendErrorMessage(status) += "Expected an array";
    RecoverParser(deserializer);
    finished = true;
  }
  deserializer.token.type = kNoTokenType;
}

ArrayView& ArrayView::begin() { return *this; }

ArrayView& ArrayView::operator++() {
  ++i;
  FillToken(deserializer);
  if (deserializer.token.type == JsonToken::kEndArrayTokenType) {
    deserializer.token.type = kNoTokenType;
    finished = true;
  } else {
    // Note that we're not clearing the token type. This means that the next `FillToken` call will
    // reuse the existing token!
  }
  return *this;
}

Str Deserializer::ErrorContext() {
  auto pos = stream.Tell();
  StrView prefix = StrView(stream.head_, stream.src_);
  int n_lines = std::count(prefix.begin(), prefix.end(), '\n') + 1;
  auto last_newline = prefix.rfind('\n');
  int col = last_newline == Str::npos ? 0 : pos - last_newline;
  return f("line %d, column %d", n_lines, col);
}

}  // namespace automat