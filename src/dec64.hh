// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "build_variant.hh"
#include "format.hh"
#include "int.hh"
#include "log.hh"
#include "str.hh"

namespace automat {

// See https://www.crockford.com/dec64.html
union DEC64 {
  U64 repr;
  struct {
    I8 exp;
    I64 coeff : 56;
  };

  template <typename T>
  constexpr static DEC64 MakeValue(T t) {
    DEC64 ret;
    ret.exp = 0;
    while (t < -0x80000000000000 || t > 0x7fffffffffffff) {
      t /= 10;
      if (ret.exp == 127) {
        if (t < 0) {
          ret.coeff = -0x80000000000000;
        } else {
          ret.coeff = 0x7fffffffffffff;
        }
        return ret;
      } else {
        ret.exp++;
      }
    }
    ret.coeff = t;
    return ret;
  }

  // Initialize from an integer in the range (-0x80000000000000, 0x7fffffffffffff)
  constexpr static DEC64 MakeRaw(I64 coeff, I8 exp = 0) {
    if constexpr (build_variant::NotRelease) {
      if (coeff < -0x80000000000000 || coeff > 0x7fffffffffffff) {
        FATAL
            << "DEC64 initialized with " << coeff
            << " which is outside of the supported range (-36028797018963968, 36028797018963967).";
      }
    }
    return DEC64{.exp = exp, .coeff = coeff};
  }

  constexpr I64 GetCoefficient() const { return coeff; }
  constexpr I8 GetExponent() const { return exp; }
  constexpr bool IsNaN() const { return exp == -128; }

  DEC64 operator+(const DEC64& rhs) const;

  Str ToStr() const {
    Str ret = f("{}", GetCoefficient());
    if (GetExponent()) {
      ret += "×10";
      Str exp = f("{}", GetExponent());
      for (auto c : exp) {
        switch (c) {
          case '-':
            ret += "⁻";
            break;
          case '0':
            ret += "⁰";
            break;
          case '1':
            ret += "¹";
            break;
          case '2':
            ret += "²";
            break;
          case '3':
            ret += "³";
            break;
          case '4':
            ret += "⁴";
            break;
          case '5':
            ret += "⁵";
            break;
          case '6':
            ret += "⁶";
            break;
          case '7':
            ret += "⁷";
            break;
          case '8':
            ret += "⁸";
            break;
          case '9':
            ret += "⁹";
            break;
          default:
            ret += c;
            break;
        }
      }
    }
    return ret;
  }

  // bool operator==(const DEC64& rhs) const {
  //   if (repr == rhs.repr) return true;
  //   if (rhs.IsNaN() && IsNaN()) return true;
  //   I64 x = repr ^ rhs.repr;
  //   if (x & 0x8000000000000000) return false;  // different signs
  //   if ((x & 0xFF) == 0) return false;         // same exponents

  //   DEC64 diff = *this - rhs;
  // }
};

constexpr static DEC64 DEC64_NaN = {0x00000000000000'80};
constexpr static DEC64 DEC64_Max = {0x7fffffffffffff'7f};
constexpr static DEC64 DEC64_Min = {0x80000000000000'7f};
constexpr static DEC64 DEC64_Zero = {0x00000000000000'00};

}  // namespace automat
