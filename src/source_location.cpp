// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "source_location.hpp"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace automat {

static SourceLocation::Names Join(StrView file_name, StrView function_name) {
  char* buffer = (char*)malloc(file_name.size() + function_name.size() + 2);
  memcpy(buffer, file_name.data(), file_name.size());
  buffer[file_name.size()] = '\0';
  char* function = buffer + file_name.size() + 1;
  memcpy(function, function_name.data(), function_name.size());
  function[function_name.size()] = '\0';
  return {.heap = {buffer, (U32)file_name.size(), (U32)function_name.size()}};
}

SourceLocation::SourceLocation(StrView file_name, StrView function_name, U32 line, U32 column)
    : names(Join(file_name, function_name)), line(line), column(column), owned(true) {}

SourceLocation::SourceLocation(const SourceLocation& other)
    : names(other.owned ? Join(other.file_name(), other.function_name()) : other.names),
      line(other.line),
      column(other.column),
      owned(other.owned) {}

SourceLocation::SourceLocation(SourceLocation&& other)
    : names(other.names), line(other.line), column(other.column), owned(other.owned) {
  other.names = {.view = {"", ""}};
  other.owned = false;
}

SourceLocation& SourceLocation::operator=(SourceLocation other) {
  std::swap(names, other.names);
  std::swap(line, other.line);
  U32 column_tmp = column;
  bool owned_tmp = owned;
  column = other.column, owned = other.owned;
  other.column = column_tmp, other.owned = owned_tmp;
  return *this;
}

SourceLocation::~SourceLocation() {
  if (owned) {
    free(names.heap.buffer);
  }
}

SourceLocation::operator std::source_location() const {
  struct Impl {  // layout of libstdc++'s private std::source_location::__impl
    const char* file_name;
    const char* function_name;
    unsigned line;
    unsigned column;
  };
  static_assert(sizeof(std::source_location) == sizeof(const Impl*));
  Names impl_names = owned ? Join(file_name(), function_name()) : names;
  auto* impl = new Impl{
      .file_name = owned ? impl_names.heap.buffer : impl_names.view.file_name,
      .function_name = owned ? impl_names.heap.buffer + impl_names.heap.file_name_size + 1
                             : impl_names.view.function_name,
      .line = line,
      .column = column};  // leaked - std::source_location can only view its data
  return std::bit_cast<std::source_location>(impl);
}

}  // namespace automat
