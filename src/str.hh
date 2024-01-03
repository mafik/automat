#pragma once

#include <string>

namespace maf {

using Str = std::string;
using StrView = std::string_view;

using namespace std::literals;

void ReplaceAll(Str &s, const Str &from, const Str &to);
void StripLeadingWhitespace(Str &);
void StripTrailingWhitespace(Str &);
void StripWhitespace(Str &);
Str Indent(StrView, int spaces = 2);

// ToStr function should be the default way of converting values to strings.
//
// It relies on ADL for lookup. Here is what Clang docs say about ADL:
//
//   First, the compiler does unqualified lookup in the scope where the name was
//   written. For a template, this means the lookup is done at the point where
//   the template is defined, not where it's instantiated.
//
//   Second, if the name is called like a function, then the compiler also does
//   argument-dependent lookup (ADL). Sometimes unqualified lookup can suppress
//   ADL; In ADL, the compiler looks at the types of all the arguments to the
//   call. When it finds a class type, it looks up the name in that class's
//   namespace; the result is all the declarations it finds in those namespaces,
//   plus the declarations from unqualified lookup. However, the compiler
//   doesn't do ADL until it knows all the argument types.

inline Str ToStr(int val) { return std::to_string(val); }
inline Str ToStr(long val) { return std::to_string(val); }
inline Str ToStr(long long val) { return std::to_string(val); }
inline Str ToStr(unsigned val) { return std::to_string(val); }
inline Str ToStr(unsigned long val) { return std::to_string(val); }
inline Str ToStr(unsigned long long val) { return std::to_string(val); }
inline Str ToStr(float val) { return std::to_string(val); }
inline Str ToStr(double val) { return std::to_string(val); }
inline Str ToStr(long double val) { return std::to_string(val); }

template <typename T>
  requires requires(T t) {
    { t.ToStr() } -> std::same_as<Str>;
  }
Str ToStr(const T &t) {
  return t.ToStr();
}

template <typename T>
concept Stringer = requires(T t) {
  { ToStr(t) } -> std::same_as<Str>;
};

} // namespace maf