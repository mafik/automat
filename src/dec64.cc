// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "dec64.hh"

namespace automat {

DEC64 DEC64::operator+(const DEC64& rhs) const {
  if (IsNaN() || rhs.IsNaN()) {
    return DEC64_NaN;
  }
  I64 my_coeff = GetCoefficient();
  I64 rhs_coeff = rhs.GetCoefficient();
  I8 my_exp = GetExponent();
  I8 rhs_exp = rhs.GetExponent();
  U8 exp_diff;
  if (my_exp == rhs_exp) {
    goto add_coefficients;
  }
  // Make sure that my_exp is the greater one
  if (rhs_exp > my_exp) {
    std::swap(my_exp, rhs_exp);
    std::swap(my_coeff, rhs_coeff);
  }
  // Decrease my_exp. If can be lowered to rhs_exp, add the coefficients.
  // Do this as long as we don't overflow my_coeff.
  while (true) {
    I64 new_coeff = my_coeff * 10;
    if (new_coeff < -0x80000000000000 || new_coeff > 0x7fffffffffffff) {
      break;
    }
    my_coeff = new_coeff;
    my_exp--;
    if (my_exp == rhs_exp) {
      goto add_coefficients;
    }
  }
  // Now we have to increase rhs_exp.
  exp_diff = my_exp - rhs_exp;
  if (exp_diff > 17) {
    return DEC64::MakeRaw(my_coeff, my_exp);
  }
  constexpr static I64 powers[] = {
      1,
      10,
      100,
      1000,
      10000,
      100000,
      1000000,
      10000000,
      100000000,
      1000000000,
      10000000000,
      100000000000,
      1000000000000,
      10000000000000,
      100000000000000,
      1000000000000000,
      10000000000000000,
      100000000000000000,
  };
  rhs_coeff /= powers[exp_diff];
  // fallthrough to add_coefficients

add_coefficients:
  I64 coeff = my_coeff + rhs_coeff;
  if (coeff == 0) {
    return DEC64_Zero;
  } else if (coeff < -0x80000000000000 || coeff > 0x7fffffffffffff) {
    if (my_exp == 127) {
      if (coeff < 0) {
        return DEC64_Min;
      } else {
        return DEC64_Max;
      }
    } else {
      return DEC64::MakeRaw(coeff / 10, my_exp + 1);
    }
  } else {
    return DEC64::MakeRaw(coeff, my_exp);
  }
}

}  // namespace automat