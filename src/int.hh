#pragma once

#include <sys/types.h>

#include <type_traits>

namespace maf {

using I8 = signed char;
using I16 = signed short;
using I32 = signed int;
using I64 = signed long long;
using I128 = __int128;  // _BitInt(128);

static_assert(sizeof(I64) == 8);

using U8 = unsigned char;
using U16 = unsigned short;

struct U24 {
  unsigned char data[3];
  constexpr U24(unsigned int x)
      : data{(unsigned char)(x & 0xff), (unsigned char)((x >> 8) & 0xff),
             (unsigned char)((x >> 16) & 0xff)} {}
  constexpr operator unsigned int() const {
    return (unsigned int)(data[0]) | ((unsigned int)(data[1]) << 8) |
           ((unsigned int)(data[2]) << 16);
  }
} __attribute__((__packed__));

// using U24 = unsigned _BitInt(24);  // this has size of 4!

static_assert(sizeof(U24) == 3);

using U32 = unsigned int;
using U64 = unsigned long;
using U128 = unsigned __int128;  // _BitInt(128);

using Size = size_t;
using SSize = I64;

}  // namespace maf

namespace std {

template <>
struct is_integral<maf::U24> {
  static constexpr bool value = true;
};

}  // namespace std