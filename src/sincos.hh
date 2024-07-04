#pragma once

#include <include/core/SkMatrix.h>
#include <sys/types.h>

#include <cmath>
#include <cstdint>

union Vec2;

namespace maf {

// TODO: use FastTrigo for the implementation.

// A number in the range [-1, 1], with 23 bits of precision and fast conversion to float.
struct Fixed1 {
  // Valid values are in the range [-2^23, 2^23)].
  int32_t value;

  constexpr static int N_BITS = 28;
  constexpr static int EXTRA_BITS = N_BITS - 23;

  constexpr Fixed1() : value(0) {}
  constexpr Fixed1(const Fixed1& other) : value(other.value) {}
  constexpr Fixed1(float number) {
    uint32_t repr = std::bit_cast<uint32_t>(number);
    // Extract the exponent.
    uint8_t raw_exponent = (repr >> 23) & 0xFF;
    // Extract the mantissa.
    uint32_t raw_mantissa = repr & 0x7FFFFF;
    // LOG << "Constructing from " << number << " with sign " << negative << ", exponent "
    //     << f("%x", raw_exponent) << ", mantissa " << f("%x", raw_mantissa);
    if (raw_exponent == 0) {
      value = 0;
    } else {
      int32_t exponent = (int32_t)raw_exponent - 127 + EXTRA_BITS;
      uint32_t mantissa = raw_mantissa | (1 << 23);
      if (exponent) {
        if (raw_exponent == 255) {
          if (raw_mantissa) {  // NaN
            mantissa = 0;
          } else {  // infinity
            mantissa = (1 << N_BITS);
          }
        } else if (exponent > 0) {
          mantissa <<= exponent;
        } else {
          // Uncomment to enable more accurate rounding (not tested).
          // bool carry = (mantissa >> (-exponent - 1)) & 1;
          mantissa >>= -exponent;
          // if (carry) {
          //   mantissa++;
          // }
        }
      }
      bool sign = repr >> 31;
      value = sign ? -mantissa : mantissa;
    }
  }
  constexpr Fixed1(double number) : Fixed1((float)number) {}
  constexpr explicit operator float() const {
    uint32_t ret = 0;
    if (value) {
      uint32_t mantissa;
      if (value < 0) {
        mantissa = -value;
        ret |= 0x8000'0000;
      } else {
        mantissa = value;
      }
      // LOG << "Value is " << f("%x", value);
      int leading_zeroes = __builtin_clz(mantissa);
      if (leading_zeroes > 8) {
        mantissa <<= leading_zeroes - 8;
      } else if (leading_zeroes < 8) {
        mantissa >>= 8 - leading_zeroes;
      }
      ret |= (127 - (leading_zeroes - 8) - EXTRA_BITS) << 23;
      ret |= (mantissa & 0x7FFFFF);
    }
    return std::bit_cast<float>(ret);
  }
  constexpr Fixed1 operator+(const Fixed1& other) const { return Fixed1(value + other.value); }
  constexpr Fixed1 operator-(const Fixed1& other) const { return Fixed1(value - other.value); }
  constexpr Fixed1 operator-() const { return Fixed1(-value); }
  constexpr Fixed1 operator*(const Fixed1& other) const {
    int64_t result = (int64_t)value * other.value;
    return Fixed1((int32_t)(result >> N_BITS));
  }
  constexpr Fixed1 operator*(int scale) const { return Fixed1(value * scale); }
  constexpr Fixed1 operator/(const Fixed1& other) const {
    int64_t result = (int64_t)value << N_BITS;
    return Fixed1((int32_t)(result / other.value));
  }
  constexpr auto operator<=>(const Fixed1& other) const = default;
  constexpr auto operator<=>(int32_t other) const { return *this <=> FromInt(other); }

  static constexpr Fixed1 FromRaw(int32_t raw) { return Fixed1(raw); }
  static constexpr Fixed1 FromInt(int32_t i) { return Fixed1(i << N_BITS); }

 private:
  constexpr Fixed1(int32_t i) : value(i) {}
};

// Return the angle in the range [0, 360).
inline float NormalizeDegrees360(float degrees) {
  if ((degrees < 0.f) || (degrees >= 360.f)) {
    return degrees - floorf(degrees / 360.f) * 360.f;
  }
  return degrees;
}

// Return the angle in the range (-180, 180].
inline float NormalizeDegrees180(float degrees) {
  float result = NormalizeDegrees360(degrees);
  if (result > 180.f) {
    result -= 360.f;
  }
  return result;
}

struct SinCos {
  using T = Fixed1;
  T sin, cos;
  constexpr SinCos() : sin(), cos(1.f) {}
  constexpr SinCos(T sin, T cos) : sin(sin), cos(cos) {}
  constexpr static SinCos FromDegrees(float degrees) {
    degrees = NormalizeDegrees360(degrees);
    if (degrees == 0.f) {
      return SinCos(0.f, 1.f);
    } else if (degrees == 180.f) {
      return SinCos(0.f, -1.f);
    } else if (degrees == 90.f) {
      return SinCos(1.f, 0.f);
    } else if (degrees == 270.f) {
      return SinCos(-1.f, 0.f);
    } else if (degrees == 45.f) {
      return SinCos(M_SQRT1_2f, M_SQRT1_2f);
    } else if (degrees == 135.f) {
      return SinCos(M_SQRT1_2f, -M_SQRT1_2f);
    } else if (degrees == 225.f) {
      return SinCos(-M_SQRT1_2f, -M_SQRT1_2f);
    } else if (degrees == 315.f) {
      return SinCos(-M_SQRT1_2f, M_SQRT1_2f);
    }
    return FromRadians(degrees / 180.f * M_PIf);
  }

  constexpr static SinCos FromRadians(float radians) {
    return SinCos(std::sin(radians), std::cos(radians));
  }

  // Initializes SinCos with the angle of the given cartesian vector.
  static SinCos FromVec2(Vec2, float length = NAN);

  float ToDegrees() const { return ToRadians() * 180 / M_PI; }
  float ToDegreesPositive() const { return ToRadiansPositive() * 180 / M_PI; }
  float ToDegreesNegative() const { return ToRadiansNegative() * 180 / M_PI; }

  // Return the angle in the range [0, pi*2).
  float ToRadiansPositive() const {
    if (sin >= 0) {
      return acosf((float)cos);
    } else {
      return -acosf((float)cos) + M_PIf * 2;
    }
  }

  // Return the angle in the range (-pi*2, 0].
  float ToRadiansNegative() const {
    if (sin < 0) {
      return -acosf((float)cos);
    } else {
      return acosf((float)cos) - M_PIf * 2;
    }
  }

  // Return the angle in the range (-pi, pi].
  float ToRadians() const { return atan2f((float)sin, (float)cos); }
  constexpr SinCos operator+(const SinCos& other) const {
    return SinCos(sin * other.cos + cos * other.sin, cos * other.cos - sin * other.sin);
  }
  constexpr SinCos operator-(const SinCos& other) const {
    return SinCos(sin * other.cos - cos * other.sin, cos * other.cos + sin * other.sin);
  }
  constexpr SinCos operator-() const { return SinCos(-sin, cos); }
  constexpr SinCos Opposite() const { return SinCos(-sin, -cos); }
  constexpr SinCos DoubleAngle() const { return SinCos(cos * sin * 2, cos * cos - sin * sin); }

  SkMatrix ToMatrix() const;
  SkMatrix ToMatrix(Vec2 pivot) const;

  void PreRotate(SkMatrix&) const;
  void PreRotate(SkMatrix&, Vec2 pivot) const;

  // Assuming this angle is in the range [0, 360], return its scaled version.
  constexpr SinCos ScalePositive(float s) const {
    return SinCos::FromRadians(ToRadiansPositive() * s);
  }

  // Assuming this angle is in the range [-360, 0], return its scaled version.
  constexpr SinCos ScaleNegative(float s) const {
    return SinCos::FromRadians(ToRadiansNegative() * s);
  }

  constexpr SinCos operator*(float s) const {
    // TODO: find a better way to do this
    if (s == 1.f) {
      return *this;
    } else if (s == -1.f) {
      return -(*this);
    } else if (s == 2.f) {
      return DoubleAngle();
    }
    return SinCos::FromRadians(ToRadians() * s);
  }

  constexpr SinCos ReflectFrom(const SinCos& normal) const {
    // Formula for reflection using polar coordinates is:
    // 2 * normal - ray - 180deg
    return SinCos(sin - (normal.cos * cos + normal.sin * sin) * normal.sin * 2,
                  cos - (normal.cos * cos + normal.sin * sin) * normal.cos * 2);
  }
  constexpr auto operator==(const SinCos& other) const {
    constexpr uint32_t kEpsilon = 1 << (Fixed1::EXTRA_BITS + 1);
    return abs((sin - other.sin).value) <= kEpsilon && abs((cos - other.cos).value) <= kEpsilon;
  }
  // Convert this angle into a 90 degree turn to either left or right.
  constexpr SinCos RightAngle() const { return SinCos(sin >= 0 ? 1.f : -1.f, 0.f); }
};

constexpr SinCos operator""_deg(long double degrees) { return SinCos::FromDegrees(degrees); }
constexpr SinCos operator""_deg(unsigned long long degrees) { return SinCos::FromDegrees(degrees); }
constexpr SinCos operator""_rad(long double radians) { return SinCos::FromRadians(radians); }
constexpr SinCos operator""_rad(unsigned long long radians) { return SinCos::FromRadians(radians); }

}  // namespace maf