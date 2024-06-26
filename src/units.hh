#pragma once

constexpr float operator""_mm(long double x) { return x / 1000; }
constexpr float operator""_mm(unsigned long long x) { return x / 1000.f; }

constexpr float operator""_cm(long double x) { return x / 100; }
constexpr float operator""_cm(unsigned long long x) { return x / 100.f; }