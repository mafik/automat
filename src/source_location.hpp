#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <source_location>

#include "int.hpp"
#include "str.hpp"

namespace automat {

// Alternative to std::source_location that can be constructed directly.
struct SourceLocation {
  union Names {
    struct View {  // owned == false
      const char* file_name;
      const char* function_name;
    } view;
    struct Heap {    // owned == true
      char* buffer;  // "<file>\0<function>\0"
      U32 file_name_size;
      U32 function_name_size;
    } heap;
  } names = {.view = {"", ""}};
  U32 line = 0;
  U32 column : 31 = 0;
  U32 owned : 1 = false;

  SourceLocation() = default;
  SourceLocation(std::source_location loc)
      : names{.view = {loc.file_name(), loc.function_name()}},
        line(loc.line()),
        column(loc.column()) {}
  SourceLocation(StrView file_name, StrView function_name, U32 line, U32 column);

  SourceLocation(const SourceLocation&);
  SourceLocation(SourceLocation&&);
  SourceLocation& operator=(SourceLocation);
  ~SourceLocation();

  // Warning: on Linux this leaks ~24 B every time it's called
  operator std::source_location() const;

  StrView file_name() const {
    return owned ? StrView(names.heap.buffer, names.heap.file_name_size)
                 : StrView(names.view.file_name);
  }
  StrView function_name() const {
    return owned ? StrView(names.heap.buffer + names.heap.file_name_size + 1,
                           names.heap.function_name_size)
                 : StrView(names.view.function_name);
  }
};

static_assert(sizeof(SourceLocation) == 24);

}  // namespace automat
