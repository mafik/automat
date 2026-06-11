#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// Drawing kit for Stochastic Parrot's hand-drawn UI style: thick dark outlines
// with a slight wobble, flat fills that can miss the outline, hard offset
// shadows and a hand-printed font.
//
// All functions draw in pixel coordinates with +Y pointing down. Shape
// randomness comes only from the `seed` parameters: the same seed always
// produces the same shape, so nothing shimmers between frames. Derive seeds
// from stable keys (an object id, a quantized position).

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPath.h>
#include <include/core/SkPoint.h>
#include <include/core/SkRect.h>

#include <cmath>
#include <cstdint>
#include <string_view>

namespace automat::ui::slop {

// ------------------------------------------------------------------ palette --
constexpr SkColor kInk = 0xff1a1a1a;
constexpr SkColor kInkPure = 0xff000000;
constexpr SkColor kInkSoft = 0xff4a4a4a;
constexpr SkColor kPaper = 0xffffffff;
constexpr SkColor kPaperCream = 0xfffffdf0;
constexpr SkColor kRed = 0xffed1c24;
constexpr SkColor kYellow = 0xfffff200;
constexpr SkColor kGreen = 0xff22b14c;
constexpr SkColor kCyan = 0xff00a2e8;
constexpr SkColor kBlue = 0xff3f48cc;
constexpr SkColor kOrange = 0xffff7f27;
constexpr SkColor kPurple = 0xffa349a4;
constexpr SkColor kRose = 0xffff1a8c;
constexpr SkColor kLime = 0xffb5e61d;
constexpr SkColor kSky = 0xff99d9ea;
constexpr SkColor kGold = 0xffffc90e;
constexpr SkColor kGray = 0xffc3c3c3;
constexpr SkColor kGrayDark = 0xff7f7f7f;
constexpr SkColor kShadow = 0x4d000000;

// ------------------------------------------------------------------- tokens --
constexpr float kStroke = 3.0f;
constexpr float kStrokeBold = 5.0f;
constexpr float kStrokeHair = 1.5f;
constexpr float kWonk = 2.2f;  // max outline deviation, px
constexpr float kSeg = 11.0f;  // spacing between wobble control points, px
constexpr float kCornerR = 8.0f;
constexpr float kShadowDX = 4.0f;
constexpr float kShadowDY = 4.0f;
constexpr float kPadS = 4.0f;
constexpr float kPadM = 8.0f;
constexpr float kPadL = 14.0f;
constexpr float kMinTouch = 24.0f;
constexpr float kTitleSize = 20.0f;
constexpr float kBodySize = 15.0f;
constexpr float kMicroSize = 12.0f;

// -------------------------------------------------------------------- noise --
inline uint32_t Hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
inline uint32_t Hash2(uint32_t a, uint32_t b) {
  return Hash(a ^ (Hash(b) + 0x9e3779b9U + (a << 6) + (a >> 2)));
}
inline uint32_t Hash3(uint32_t a, uint32_t b, uint32_t c) { return Hash2(Hash2(a, b), c); }
inline float U01(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); }  // [0,1)
inline float U11(uint32_t h) { return U01(h) * 2.0f - 1.0f; }             // [-1,1]

inline float ValueNoise1D(float t, uint32_t seed, float freq = 1.0f) {
  t *= freq;
  float fi = std::floor(t);
  int i = (int)fi;
  float f = t - fi;
  float u = f * f * (3.0f - 2.0f * f);
  float a = U11(Hash2(seed, (uint32_t)i));
  float b = U11(Hash2(seed, (uint32_t)(i + 1)));
  return a + (b - a) * u;
}

struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 0x1234567u) {}
  uint32_t next() {
    s = Hash(s);
    return s;
  }
  float f01() { return U01(next()); }
  float f11() { return U11(next()); }
};

// Coordinates are rounded so sub-pixel motion doesn't change the seed.
inline uint32_t SeedFromPos(float x, float y, uint32_t id = 0) {
  return Hash3((uint32_t)std::lround(x), (uint32_t)std::lround(y), id * 2654435761u + 1u);
}

// ----------------------------------------------------------- state & style --
enum class State { Default, Hover, Pressed, Active, Disabled, Error };

// Returns kInk or kPaper, whichever reads better on `bg`.
SkColor TextOn(SkColor bg);

// ------------------------------------------------------------- primitives ---
SkPaint InkPaint(SkColor color = kInk, float width = kStroke, bool antialias = true);

// Endpoints are preserved; deviation tapers to zero at the ends of open contours.
SkPath WobbleLine(SkPoint a, SkPoint b, float amp, float seg, uint32_t seed);
SkPath WobbleRect(const SkRect& r, float amp, float seg, uint32_t seed);
SkPath WobbleEllipse(SkPoint center, float rx, float ry, float amp, uint32_t seed,
                     int samples = 48);
SkPath WobblePath(const SkPath& src, float amp, float seg, uint32_t seed);

SkPath WonkyRoundRect(const SkRect& r, float baseRadius, float wobAmp, uint32_t seed);

// Offset silhouette copy of `shape`, not a blur.
void HandShadow(SkCanvas& canvas, const SkPath& shape, SkVector offset, SkColor shadow = kShadow,
                uint32_t seed = 0);

void FillPath(SkCanvas& canvas, const SkPath& path, SkColor color);
// Fill drawn from a re-wobbled, offset copy of `outline`, so the color spills
// slightly past the line.
void MisregFill(SkCanvas& canvas, const SkPath& outline, SkColor fill, uint32_t seed);

void SketchyStroke(SkCanvas& canvas, const SkPath& outline, SkColor color = kInk,
                   float width = kStroke, uint32_t seed = 0, int passes = 2);

void ScribbleFill(SkCanvas& canvas, const SkPath& shape, SkColor color, float strokeW,
                  float spacing, uint32_t seed);
void SprayDisc(SkCanvas& canvas, SkPoint center, float radius, int count, SkColor color, float dotR,
               uint32_t seed);

void HatchRect(SkCanvas& canvas, const SkRect& r, SkColor color, float spacing, uint32_t seed);

// --------------------------------------------------------------- lettering --
// `wonk` adds per-glyph baseline bob and rotation; keep it false for numeric
// readouts.
enum class TextAlign { Left, Center, Right };
float TextWidth(std::string_view text, float size);
void DrawText(SkCanvas& canvas, std::string_view text, SkPoint baseline_left, float size,
              SkColor color = kInk, bool wonk = false, uint32_t seed = 0);
void DrawTextIn(SkCanvas& canvas, std::string_view text, const SkRect& box, float size,
                SkColor color = kInk, TextAlign align = TextAlign::Center, bool wonk = false,
                uint32_t seed = 0);

// ----------------------------------------------------------------- motifs ---
SkPath StarPath(SkPoint c, float r_outer, float r_inner, uint32_t seed, int points = 5);
SkPath SunPath(SkPoint c, float core_r, float ray_len, int rays, uint32_t seed);
SkPath SparklePath(SkPoint c, float r_outer, uint32_t seed);
SkPath ArrowPath(SkPoint from, SkPoint to, float head_len, float head_half, uint32_t seed);
SkPath BurstPath(SkPoint c, float r_outer, float r_inner, int points, uint32_t seed);

void DrawStar(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed);
void DrawSun(SkCanvas& canvas, SkPoint c, float core_r, float ray_len, SkColor fill, uint32_t seed);
void DrawSparkle(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed);
void DrawSmiley(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed);
void DrawArrow(SkCanvas& canvas, SkPoint from, SkPoint to, SkColor color, float width,
               uint32_t seed);
void DrawHeart(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed);

void DrawSlopStamp(SkCanvas& canvas, SkPoint c, float r, float rotation_deg, uint32_t seed,
                   std::string_view label = "SLOP");

// ------------------------------------------------------------- components ---
void Panel(SkCanvas& canvas, const SkRect& r, std::string_view title, SkColor accent = kBlue,
           State state = State::Default, uint32_t seed = 0, bool sticker = true);

void Button(SkCanvas& canvas, const SkRect& r, std::string_view label, SkColor color = kGreen,
            State state = State::Default, uint32_t seed = 0);

void Toggle(SkCanvas& canvas, const SkRect& r, bool on, State state = State::Default,
            uint32_t seed = 0);

void Checkbox(SkCanvas& canvas, const SkRect& r, bool checked, State state = State::Default,
              uint32_t seed = 0);
void Radio(SkCanvas& canvas, SkPoint c, float r, bool selected, State state = State::Default,
           uint32_t seed = 0);

void Slider(SkCanvas& canvas, const SkRect& r, float t, State state = State::Default,
            uint32_t seed = 0);  // t in [0,1]

void Knob(SkCanvas& canvas, SkPoint c, float radius, float t, State state = State::Default,
          uint32_t seed = 0);  // t in [0,1]

void Field(SkCanvas& canvas, const SkRect& r, std::string_view text, bool focused = false,
           State state = State::Default, uint32_t seed = 0);

void Dropdown(SkCanvas& canvas, const SkRect& r, std::string_view value, SkColor accent = kBlue,
              State state = State::Default, uint32_t seed = 0);

void Stepper(SkCanvas& canvas, const SkRect& r, std::string_view value,
             State state = State::Default, uint32_t seed = 0);

void Port(SkCanvas& canvas, SkPoint c, float r, bool is_output, SkColor type = kBlue,
          bool connected = true, State state = State::Default, uint32_t seed = 0);

void Cable(SkCanvas& canvas, SkPoint a, SkPoint b, SkColor color = kBlue, uint32_t seed = 0);

void Badge(SkCanvas& canvas, SkPoint c, std::string_view label, SkColor color = kRed,
           float rotation_deg = -8.0f, uint32_t seed = 0);

void ThumbWell(SkCanvas& canvas, const SkRect& r, State state = State::Default, uint32_t seed = 0);

void Bubble(SkCanvas& canvas, const SkRect& r, std::string_view text, SkPoint tail_to,
            SkColor color = kYellow, uint32_t seed = 0);

void Divider(SkCanvas& canvas, SkPoint a, SkPoint b, uint32_t seed = 0);

void Activity(SkCanvas& canvas, const SkRect& r, float t, State state = State::Default,
              uint32_t seed = 0);  // t in [0,1]

void Spinner(SkCanvas& canvas, SkPoint c, float r, float phase,
             uint32_t seed = 0);  // phase in [0,1)

// resize=true draws a corner resize hatch; false draws a move-handle dot cluster.
void Grip(SkCanvas& canvas, const SkRect& r, bool resize, uint32_t seed = 0);

void Highlight(SkCanvas& canvas, const SkRect& r, SkColor color = kBlue, uint32_t seed = 0);

}  // namespace automat::ui::slop
