// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <fmt/format.h>
#include <fmt/printf.h>

#include "str.hh"

#if !__has_builtin(__builtin_dump_struct)
#include <typeinfo>
#endif

namespace automat {

// Format strings using fmt library
template <typename... Args>
Str f(fmt::format_string<Args...> fmt, Args&&... args) {
  return fmt::format(fmt, std::forward<Args>(args)...);
}

// Prefix each line with `spaces` spaces.
std::string IndentString(std::string in, int spaces = 2);

std::string Slugify(std::string in);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
constexpr void constexpr_sprintf(std::string& out, const char* format, auto... args) {
  // Convert arguments to handle pointer formatting issues with fmt
  auto convert_arg = [](auto&& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_pointer_v<T> && !std::is_same_v<T, const char*> &&
                  !std::is_same_v<T, char*>) {
      return reinterpret_cast<void*>(
          const_cast<std::remove_const_t<std::remove_pointer_t<T>>*>(arg));
    } else {
      return arg;
    }
  };
  out += fmt::sprintf(format, convert_arg(args)...);
}
#pragma clang diagnostic pop

// Convert a platform-specific type name (obtained from type_info.name()) into a short class name.
std::string_view CleanTypeName(std::string_view mangled);

template <typename T>
std::string dump_struct(const T& t) {
  std::string s;
#if __has_builtin(__builtin_dump_struct)
  __builtin_dump_struct(&t, constexpr_sprintf, s);
#else
#if __cpp_rtti
  s += typeid(T).name();
  s += ' ';
#endif
  for (int i = 0; i < sizeof(T); ++i) {
    s += fmt::format("{:02x} ", ((unsigned char*)&t)[i]);
  }
#endif
  return s;
}

}  // namespace automat
