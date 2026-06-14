#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// Drawing kit for Stochastic Parrot's hand-drawn UI style: thick dark outlines
// with a slight wobble, flat fills that can miss the outline, hard offset
// shadows and a hand-printed font.
//
// All functions draw in metric coordinates with +Y pointing up. Shape
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
#include <functional>
#include <string_view>

#include "object.hpp"
#include "ui_button.hpp"
#include "units.hpp"
#include "widget.hpp"

namespace automat::ui::beta {

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
constexpr float kStroke = 0.45_mm;
constexpr float kStrokeBold = 0.75_mm;
constexpr float kStrokeHair = 0.22_mm;
constexpr float kWonk = 0.32_mm;  // max outline deviation
constexpr float kSeg = 1.6_mm;    // spacing between wobble control points
constexpr float kCornerR = 1.2_mm;
constexpr float kShadowDX = 0.6_mm;
constexpr float kShadowDY = 0.6_mm;
constexpr float kPadS = 0.6_mm;
constexpr float kPadM = 1.2_mm;
constexpr float kPadL = 2.0_mm;
constexpr float kMinTouch = 3.5_mm;
constexpr float kTitleSize = 2.9_mm;
constexpr float kBodySize = 2.2_mm;
constexpr float kMicroSize = 1.75_mm;
// Position quantum for shape seeds, so sub-grid motion doesn't reshuffle wobble.
constexpr float kSeedGrid = 0.15_mm;

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

// Coordinates are quantized to the beta grid so sub-grid motion doesn't change
// the seed.
inline uint32_t SeedFromPos(float x, float y, uint32_t id = 0) {
  return Hash3((uint32_t)std::lround(x / kSeedGrid), (uint32_t)std::lround(y / kSeedGrid),
               id * 2654435761u + 1u);
}

// ----------------------------------------------------------- state & style --
enum class State { Default, Hover, Pressed, Active, Disabled, Error };

// Returns kInk or kPaper, whichever reads better on `bg`.
SkColor TextOn(SkColor bg);

// ------------------------------------------------------------- primitives ---
SkPaint InkPaint(SkColor color = kInk, float width = kStroke, bool antialias = true);

// Endpoints are preserved; deviation tapers to zero at the ends of open contours.
SkPath WobbleLine(Vec2 a, Vec2 b, float amp, float seg, uint32_t seed);
SkPath WobbleRect(const Rect& r, float amp, float seg, uint32_t seed);
SkPath WobbleEllipse(Vec2 center, float rx, float ry, float amp, uint32_t seed, int samples = 48);
SkPath WobblePath(const SkPath& src, float amp, float seg, uint32_t seed);

SkPath WonkyRoundRect(const Rect& r, float baseRadius, float wobAmp, uint32_t seed);

// Offset silhouette copy of `shape`, not a blur.
void HandShadow(SkCanvas& canvas, const SkPath& shape, Vec2 offset, SkColor shadow = kShadow,
                uint32_t seed = 0);

void FillPath(SkCanvas& canvas, const SkPath& path, SkColor color);
// Fill drawn from a re-wobbled, offset copy of `outline`, so the color spills
// slightly past the line.
void MisregFill(SkCanvas& canvas, const SkPath& outline, SkColor fill, uint32_t seed);

void SketchyStroke(SkCanvas& canvas, const SkPath& outline, SkColor color = kInk,
                   float width = kStroke, uint32_t seed = 0, int passes = 2);

void ScribbleFill(SkCanvas& canvas, const SkPath& shape, SkColor color, float strokeW,
                  float spacing, uint32_t seed);
void SprayDisc(SkCanvas& canvas, Vec2 center, float radius, int count, SkColor color, float dotR,
               uint32_t seed);

void HatchRect(SkCanvas& canvas, const Rect& r, SkColor color, float spacing, uint32_t seed);

// --------------------------------------------------------------- lettering --
// `wonk` adds per-glyph baseline bob and rotation; keep it false for numeric
// readouts.
enum class TextAlign { Left, Center, Right };
float TextWidth(std::string_view text, float size);
void DrawText(SkCanvas& canvas, std::string_view text, Vec2 baseline_left, float size,
              SkColor color = kInk, bool wonk = false, uint32_t seed = 0);
void DrawTextIn(SkCanvas& canvas, std::string_view text, const Rect& box, float size,
                SkColor color = kInk, TextAlign align = TextAlign::Center, bool wonk = false,
                uint32_t seed = 0);

// ----------------------------------------------------------------- motifs ---
SkPath StarPath(Vec2 c, float r_outer, float r_inner, uint32_t seed, int points = 5);
SkPath SunPath(Vec2 c, float core_r, float ray_len, int rays, uint32_t seed);
SkPath SparklePath(Vec2 c, float r_outer, uint32_t seed);
SkPath ArrowPath(Vec2 from, Vec2 to, float head_len, float head_half, uint32_t seed);
SkPath BurstPath(Vec2 c, float r_outer, float r_inner, int points, uint32_t seed);

void DrawStar(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed);
void DrawSun(SkCanvas& canvas, Vec2 c, float core_r, float ray_len, SkColor fill, uint32_t seed);
void DrawSparkle(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed);
void DrawSmiley(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed);
void DrawArrow(SkCanvas& canvas, Vec2 from, Vec2 to, SkColor color, float width, uint32_t seed);
void DrawHeart(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed);

void DrawBetaStamp(SkCanvas& canvas, Vec2 c, float r, float rotation_deg, uint32_t seed,
                   std::string_view label = "BETA");

// ------------------------------------------------------------- components ---
void Panel(SkCanvas& canvas, const Rect& r, std::string_view title, SkColor accent = kBlue,
           State state = State::Default, uint32_t seed = 0, bool sticker = true);

void Button(SkCanvas& canvas, const Rect& r, std::string_view label, SkColor color = kGreen,
            State state = State::Default, uint32_t seed = 0);

void Toggle(SkCanvas& canvas, const Rect& r, bool on, State state = State::Default,
            uint32_t seed = 0);

void Checkbox(SkCanvas& canvas, const Rect& r, bool checked, State state = State::Default,
              uint32_t seed = 0);
void Radio(SkCanvas& canvas, Vec2 c, float r, bool selected, State state = State::Default,
           uint32_t seed = 0);

void Slider(SkCanvas& canvas, const Rect& r, float t, State state = State::Default,
            uint32_t seed = 0);  // t in [0,1]

void Knob(SkCanvas& canvas, Vec2 c, float radius, float t, State state = State::Default,
          uint32_t seed = 0);  // t in [0,1]

void Field(SkCanvas& canvas, const Rect& r, std::string_view text, bool focused = false,
           State state = State::Default, uint32_t seed = 0);

void Dropdown(SkCanvas& canvas, const Rect& r, std::string_view value, SkColor accent = kBlue,
              State state = State::Default, uint32_t seed = 0);

void Stepper(SkCanvas& canvas, const Rect& r, std::string_view value, State state = State::Default,
             uint32_t seed = 0);

void Port(SkCanvas& canvas, Vec2 c, float r, bool is_output, SkColor type = kBlue,
          bool connected = true, State state = State::Default, uint32_t seed = 0);

void Cable(SkCanvas& canvas, Vec2 a, Vec2 b, SkColor color = kBlue, uint32_t seed = 0);

void Badge(SkCanvas& canvas, Vec2 c, std::string_view label, SkColor color = kRed,
           float rotation_deg = -8.0f, uint32_t seed = 0);

void ThumbWell(SkCanvas& canvas, const Rect& r, State state = State::Default, uint32_t seed = 0);

void Bubble(SkCanvas& canvas, const Rect& r, std::string_view text, Vec2 tail_to,
            SkColor color = kYellow, uint32_t seed = 0);

void Divider(SkCanvas& canvas, Vec2 a, Vec2 b, uint32_t seed = 0);

void Activity(SkCanvas& canvas, const Rect& r, float t, State state = State::Default,
              uint32_t seed = 0);  // t in [0,1]

void Spinner(SkCanvas& canvas, Vec2 c, float r, float phase,
             uint32_t seed = 0);  // phase in [0,1)

// resize=true draws a corner resize hatch; false draws a move-handle dot cluster.
void Grip(SkCanvas& canvas, const Rect& r, bool resize, uint32_t seed = 0);

void Highlight(SkCanvas& canvas, const Rect& r, SkColor color = kBlue, uint32_t seed = 0);

// -------------------------------------------------------------------- widgets --

// Base for toys of beta-styled objects. Drawing code passes Seed(site) as the
// seed argument: the site constant keeps shapes within one object distinct,
// the owner's address keeps the same site distinct between objects.
struct ObjectToy : automat::ObjectToy {
  using automat::ObjectToy::ObjectToy;
  uint32_t Seed(uint32_t site) const {
    uintptr_t a = reinterpret_cast<uintptr_t>(owner.GetUnsafe());
    uint32_t id = static_cast<uint32_t>(a >> 4) * 2654435761u;  // skip alignment zeros, spread
    return Hash2(site, id ? id : 1u);
  }
};

// The run button of beta-styled objects: a hand-drawn disc with a play
// triangle. It seats ITSELF at its parent's lower center with a slight
// overhang past the border - the spot that matches the "next" connector -
// so objects never position it manually. While `running` it shows a stop
// square on red; while not `enabled` it grays out and ignores clicks.
struct RunButton : Widget {
  Clickable clickable;
  std::function<void()> on_click;
  uint32_t seed;        // per-object wobble; owners pass their Seed(site)
  uint32_t wiggle = 0;  // hover shimmer phase
  bool running = false;
  bool enabled = true;

  constexpr static float kRadius = 4_mm;
  constexpr static float kOverhang = 2_mm;  // how far the disc dips below the border

  // For the owning toy's ArgStart override: a connector that would start at
  // the bottom center must begin at the disc's bottom edge, not under it.
  static Vec2AndDir AdjustArgStart(Vec2AndDir start) {
    start.pos.y -= kOverhang;
    return start;
  }

  RunButton(Widget* parent, std::function<void()> on_click, uint32_t seed = 0);

  StrView Name() const override { return "RunButton"; }
  bool CenteredAtZero() const override { return true; }
  SkPath Shape() const override { return SkPath::Circle(0, 0, kRadius); }
  // Outset so the outline overshoot and the offset shadow aren't clipped.
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }
  void PointerOver(Pointer& p) override { clickable.PointerOver(p); }
  void PointerLeave(Pointer& p) override { clickable.PointerLeave(p); }
  std::unique_ptr<Action> FindAction(Pointer& p, ActionTrigger a) override {
    if (!enabled) return nullptr;
    return clickable.FindAction(p, a);
  }
  Tock Tick(time::Timer& t) override;
  void Draw(SkCanvas& canvas) const override;
};

}  // namespace automat::ui::beta
