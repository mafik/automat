#pragma once

#include "str.hh"

#if !__has_builtin(__builtin_dump_struct)
#include <typeinfo>
#endif

namespace maf {

// TODO: replace this with std::format when it's available
Str f(const char *fmt, ...);

// Prefix each line with `spaces` spaces.
std::string IndentString(std::string in, int spaces = 2);

std::string Slugify(std::string in);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
constexpr void constexpr_sprintf(std::string &out, const char *format,
                                 auto... args) {
  int n = snprintf(nullptr, 0, format, args...) + 1;
  char buf[n];
  snprintf(buf, n, format, args...);
  out += buf;
}
#pragma clang diagnostic pop

template <typename T> std::string dump_struct(const T &t) {
  std::string s;
#if __has_builtin(__builtin_dump_struct)
  __builtin_dump_struct(&t, constexpr_sprintf, s);
#else
#if __cpp_rtti
  s += typeid(T).name();
  s += ' ';
#endif
  for (int i = 0; i < sizeof(T); ++i) {
    constexpr_sprintf(s, "%02x ", ((unsigned char *)&t)[i]);
  }
#endif
  return s;
}

} // namespace maf