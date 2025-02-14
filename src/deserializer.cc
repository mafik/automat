// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "deserializer.hh"

#include "format.hh"
#include "log.hh"
#include "status.hh"

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

Str ToStr(const JsonToken& token) {
  Str value;
  switch (token.type) {
    case JsonToken::kNullTokenType:
      return "null";
    case JsonToken::kBooleanTokenType:
      return token.value.b ? "true" : "false";
    case JsonToken::kIntTokenType:
      return f("%d", token.value.i);
    case JsonToken::kUintTokenType:
      return f("%u", token.value.u);
    case JsonToken::kInt64TokenType:
      return f("%" PRId64, token.value.i64);
    case JsonToken::kUint64TokenType:
      return f("%" PRIu64, token.value.u64);
    case JsonToken::kDoubleTokenType:
      return f("%f", token.value.d);
    case JsonToken::kRawNumberTokenType:
      value = token.value.raw_number;
      break;
    case JsonToken::kStringTokenType:
      return f("\"%*s\"", token.value.string.size(), token.value.string.data());
    case JsonToken::kKeyTokenType:
      value = token.value.key;
      break;
    default:
      value = "??";
      break;
  }
  return f("%s(%s)", ToStr(token.type).c_str(), value.c_str());
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
    deserializer.token.value.string = StrView(str, length);
    return true;
  }
  bool StartObject() {
    deserializer.token.type = kStartObjectTokenType;
    return true;
  }
  bool Key(const char* str, rapidjson::SizeType length, bool copy) {
    deserializer.token.type = kKeyTokenType;
    deserializer.token.value.key = StrView(str, length);
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

static void FillToken(Deserializer& d) {
  if (d.token.type == kNoTokenType) {
    Handler handler(d);
    d.reader.IterativeParseNext<kParseNoFlags>(d.stream, handler);
  }
}

static void RecoverParser(Deserializer& d) {
  int n_objects = 0;
  int n_arrays = 0;
  if (d.token.type == JsonToken::kStartObjectTokenType) {
    n_objects = 1;
  } else if (d.token.type == JsonToken::kStartArrayTokenType) {
    n_arrays = 1;
  } else {
    d.token.type = kNoTokenType;
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

Deserializer::Deserializer(InsituStringStream& stream) : stream(stream) {
  reader.IterativeParseInit();
}

void Deserializer::Get(Str& result, Status& status) {
  FillToken(*this);
  if (token.type == kStringTokenType) {
    token.type = kNoTokenType;
    result = token.value.string;
  } else {
    AppendErrorMessage(status) += "Expected a string but got " + ToStr(token.type);
    RecoverParser(*this);
  }
}
void Deserializer::Get(double& result, Status& status) {
  FillToken(*this);
  if (token.type == kDoubleTokenType) {
    token.type = kNoTokenType;
    result = token.value.d;
  } else if (token.type == kUintTokenType) {
    token.type = kNoTokenType;
    result = token.value.u;
  } else if (token.type == kIntTokenType) {
    token.type = kNoTokenType;
    result = token.value.i;
  } else if (token.type == kInt64TokenType) {
    token.type = kNoTokenType;
    result = token.value.i64;
  } else if (token.type == kUint64TokenType) {
    token.type = kNoTokenType;
    result = token.value.u64;
  } else {
    AppendErrorMessage(status) += "Expected a double but got " + ToStr(token.type);
    RecoverParser(*this);
  }
}
void Deserializer::Get(float& result, Status& status) {
  double d;
  Get(d, status);
  if (OK(status)) {
    result = d;
  }
}
void Deserializer::Get(int& result, Status& status) {
  FillToken(*this);
  if (token.type == JsonToken::kIntTokenType) {
    token.type = kNoTokenType;
    result = token.value.i;
  } else if (token.type == JsonToken::kUintTokenType) {
    token.type = kNoTokenType;
    result = token.value.u;
  } else {
    AppendErrorMessage(status) += "Expected an integer but got " + ToStr(token.type);
    RecoverParser(*this);
  }
}
void Deserializer::Get(int64_t& result, Status& status) {
  FillToken(*this);
  if (token.type == JsonToken::kInt64TokenType) {
    token.type = kNoTokenType;
    result = token.value.i64;
  } else if (token.type == JsonToken::kIntTokenType) {
    token.type = kNoTokenType;
    result = token.value.i;
  } else if (token.type == JsonToken::kUintTokenType) {
    token.type = kNoTokenType;
    result = token.value.u;
  } else {
    AppendErrorMessage(status) += "Expected an integer but got " + ToStr(token.type);
    RecoverParser(*this);
  }
}
void Deserializer::Get(bool& result, Status& status) {
  FillToken(*this);
  if (token.type == JsonToken::kBooleanTokenType) {
    token.type = kNoTokenType;
    result = token.value.b;
  } else {
    AppendErrorMessage(status) += "Expected a boolean";
    RecoverParser(*this);
  }
}
void Deserializer::Skip() {
  FillToken(*this);
  RecoverParser(*this);
  token.type = kNoTokenType;
}

ObjectView::ObjectView(Deserializer& deserializer, Status& status)
    : deserializer(deserializer),
      status(status),
      debug_json_path_size(deserializer.debug_path_size) {
  FillToken(deserializer);
  if (deserializer.token.type != JsonToken::kStartObjectTokenType) {
    AppendErrorMessage(status) += "Expected an object but got " + ToStr(deserializer.token.type);
    RecoverParser(deserializer);
    finished = true;
    deserializer.debug_path_size = debug_json_path_size;
    return;
  }
  deserializer.token.type = kNoTokenType;
  ReadKey();
}

// Move the error from `cleared` to `filled` (if `filled` is OK) or just clear it.
static void ClearError(Status& cleared, Status& filled) {
  if (!OK(cleared)) {
    if (OK(filled)) {
      filled = std::move(cleared);
    } else {
      cleared.Reset();
    }
  }
}

void ObjectView::ReadKey() {
  ClearError(status, first_issue);
  FillToken(deserializer);
  if (deserializer.token.type == JsonToken::kEndObjectTokenType) {
    deserializer.token.type = kNoTokenType;
    finished = true;
    deserializer.debug_path_size = debug_json_path_size;
    status = std::move(first_issue);
  } else if (deserializer.token.type == JsonToken::kKeyTokenType) {
    deserializer.token.type = kNoTokenType;
    key = Str(deserializer.token.value.key);

    deserializer.debug_path_size = debug_json_path_size;

    bool bracket = false;
    if (key.find_first_of(" .[]") != Str::npos) {
      bracket = true;
      deserializer.DebugPut('[');
    } else if (deserializer.debug_path_size) {
      deserializer.DebugPut('.');
    }
    for (char c : key) {
      deserializer.DebugPut(c);
    }
    if (bracket) {
      deserializer.DebugPut(']');
    }

  } else {
    AppendErrorMessage(status) +=
        "Unknown field " + deserializer.DebugPath() + ": " + ToStr(deserializer.token);
    deserializer.Skip();
    ReadKey();
  }
}

ArrayView::ArrayView(Deserializer& deserializer, Status& status)
    : deserializer(deserializer),
      status(status),
      debug_json_path_size(deserializer.debug_path_size) {
  FillToken(deserializer);
  if (deserializer.token.type != JsonToken::kStartArrayTokenType) {
    AppendErrorMessage(status) += "Expected an array";
    RecoverParser(deserializer);
    finished = true;
    deserializer.debug_path_size = debug_json_path_size;
  } else {
    deserializer.token.type = kNoTokenType;
    FillToken(deserializer);
    if (deserializer.token.type == JsonToken::kEndArrayTokenType) {
      deserializer.token.type = kNoTokenType;
      finished = true;
      deserializer.debug_path_size = debug_json_path_size;
    }
  }
}

void ArrayView::Next() {
  ClearError(status, first_issue);
  ++i;
  FillToken(deserializer);
  deserializer.debug_path_size = debug_json_path_size;
  if (deserializer.token.type == JsonToken::kEndArrayTokenType) {
    deserializer.token.type = kNoTokenType;
    finished = true;
    status = std::move(first_issue);
  } else {
    deserializer.DebugPut('[');
    auto i_str = std::to_string(i);
    for (char c : i_str) {
      deserializer.DebugPut(c);
    }
    deserializer.DebugPut(']');
    // Note that we're not clearing the token type. This means that the next `FillToken` call will
    // reuse the existing token!
  }
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