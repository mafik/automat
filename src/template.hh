// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "int.hh"

namespace automat {

// Helper for user-defined string literals.
//
// C++ (as of C++20) does not allow string literals to be passed as template arguments. This class
// is a workaround for this. It us useful especially for user-defined string literals where it can
// be used as template argument which is then expanded as:
//
// "#ff0000"_color   =>    operator""_color<TemplateStringArg<8>("#ff0000")>
template <Size N>
struct TemplateStringArg {
  char c_str[N]{};

  static consteval std::size_t size() { return N; }

  consteval TemplateStringArg(char const (&s)[N]) {
    for (int i = 0; i < N; ++i) c_str[i] = s[i];
  }
};

}  // namespace automat