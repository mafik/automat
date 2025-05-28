// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "format.hh"

#include <cstdarg>
#include <cstdio>

namespace automat {

std::string f(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  va_list args2;
  va_copy(args2, args);
  int n = vsnprintf(NULL, 0, fmt, args) + 1;
  char buf[n];
  vsnprintf(buf, sizeof(buf), fmt, args2);
  va_end(args);
  return std::string(buf);
}

std::string IndentString(std::string in, int spaces) {
  std::string out(spaces, ' ');
  for (char c : in) {
    out += c;
    if (c == '\n') {
      for (int i = 0; i < spaces; ++i) {
        out += ' ';
      }
    }
  }
  return out;
}

std::string Slugify(std::string in) {
  std::string out;
  bool unk = false;
  for (char c : in) {
    if (c >= 'A' && c <= 'Z') {
      if (unk) {
        if (!out.empty()) {
          out += '-';
        }
        unk = false;
      }
      out += c - 'A' + 'a';
    } else if (c >= 'a' && c <= 'z') {
      if (unk) {
        if (!out.empty()) {
          out += '-';
        }
        unk = false;
      }
      out += c;
    } else if (c >= '0' && c <= '9') {
      if (unk) {
        if (!out.empty()) {
          out += '-';
        }
        unk = false;
      }
      out += c;
    } else {
      unk = true;
    }
  }
  return out;
}

std::string_view CleanTypeName(std::string_view mangled) {
#ifdef _WIN32
  // On Windows we get a long name that starts with a struct and then a sequence of namespaces:
  // "struct automat::library::FlipFlopButton"
  // We extract just the last component.
  if (mangled.starts_with("struct ")) {
    mangled.remove_prefix(7);
  }
  for (int i = mangled.size() - 2; i > 0; --i) {
    if (mangled[i] == ':' && mangled[i + 1] == ':') {
      mangled.remove_prefix(i + 2);
      break;
    }
  }
  return mangled;
#else
  // On Linux we get a C++-mangled name:
  // "N7automat7library14FlipFlopButtonE"
  if (mangled.starts_with("N") && mangled.ends_with("E")) {
    mangled.remove_prefix(1);  // Remove 'N'
    mangled.remove_suffix(1);  // Remove 'E'
    while (mangled.size() > 1 && mangled[0] >= '0' && mangled[0] <= '9') {
      // Parse the length of the next component
      size_t length = 0;
      size_t i = 0;
      while (i < mangled.size() && mangled[i] >= '0' && mangled[i] <= '9') {
        length = length * 10 + (mangled[i] - '0');
        i++;
      }
      // Skip this component
      if (i + length < mangled.size()) {
        mangled.remove_prefix(i + length);
      } else {
        mangled.remove_prefix(i);  // final component - remove only its length
        break;
      }
    }
  }
  return mangled;
#endif
}
}  // namespace automat