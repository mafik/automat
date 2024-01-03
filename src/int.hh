#pragma once

#include <type_traits>

namespace maf {

using I8 = signed char;
using I16 = signed short;
using I32 = signed int;
using I64 = signed long;
using I128 = __int128; // _BitInt(128);

static_assert(sizeof(I64) == 8);

using U8 = unsigned char;
using U16 = unsigned short;

struct U24 {
  unsigned int data : 24;
  constexpr U24(unsigned int x) : data(x) {}
  constexpr operator unsigned int() const { return data; }
} __attribute__((__packed__));

// using U24 = unsigned _BitInt(24); // this has size of 4!

static_assert(sizeof(U24) == 3);

using U32 = unsigned int;
using U64 = unsigned long;
using U128 = unsigned __int128; // _BitInt(128);

using Size = unsigned long;
using SSize = signed long;

} // namespace maf

namespace std {

template <> struct is_integral<maf::U24> {
  static constexpr bool value = true;
};

} // namespace std