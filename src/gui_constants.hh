#pragma once

namespace automat::gui {

constexpr float operator""_mm(long double x) { return x / 1000; }
constexpr float operator""_mm(unsigned long long x) { return x / 1000.f; }

constexpr float operator""_cm(long double x) { return x / 100; }
constexpr float operator""_cm(unsigned long long x) { return x / 100.f; }

static constexpr float kMinimalTouchableSize = 8_mm;
static constexpr float kBorderWidth = 1_mm / 2;
static constexpr float kMargin = 1_mm;
static constexpr float kLetterSize = 3_mm;
static constexpr float kLetterSizeMM = kLetterSize * 1000;

}  // namespace automat::gui