// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "ui_leptonica.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRRect.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace automat::ui::leptonica {

float LevelValueToX(const Rect& band, float value, float vmin, float vmax) {
  float t = std::clamp((value - vmin) / std::max(1e-6f, vmax - vmin), 0.f, 1.f);
  return band.left + t * band.Width();
}
float LevelXToValue(const Rect& band, float x, float vmin, float vmax) {
  float t = std::clamp((x - band.left) / std::max(1e-6f, band.Width()), 0.f, 1.f);
  return vmin + t * (vmax - vmin);
}
bool LevelGrabsMarker(const Rect& band, Vec2 p, float value, float vmin, float vmax, float grip) {
  float mx = LevelValueToX(band, value, vmin, vmax);
  if (grip <= 0.f) grip = band.Height() * 0.22f;
  float knob_y = band.top + band.Height() * 0.16f;
  float knob_r = band.Height() * 0.20f;
  bool on_line = std::abs(p.x - mx) <= grip && p.y <= knob_y + knob_r &&
                 p.y >= band.bottom - band.Height() * 0.24f;
  bool on_knob = std::hypot(p.x - mx, p.y - knob_y) <= knob_r + grip * 0.5f;
  return on_line || on_knob;
}

void DrawLevel(SkCanvas& canvas, const Rect& band, float value, float vmin, float vmax,
               const uint32_t* histogram, float max_log_count, bool keep_above,
               bool show_comparator, beta::State state, uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  const bool pressed = state == State::Pressed || state == State::Active;
  const bool hover = state == State::Hover;

  const float H = band.Height();
  const float mx = LevelValueToX(band, value, vmin, vmax);
  const float baseline = band.bottom;
  const SkColor cut_tint = disabled ? 0x14000000 : 0x22101010;
  const SkColor keep_tint = disabled ? 0x10ffffff : 0x26ffffff;

  {
    SkPaint paper;
    paper.setAntiAlias(true);
    paper.setColor(disabled ? kGray : kPaperCream);
    canvas.drawRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), paper);
    canvas.save();
    canvas.clipRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), true);
    SkPaint tl, tr;
    tl.setColor(cut_tint);
    tr.setColor(keep_tint);
    canvas.drawRect(Rect{band.left, band.bottom, mx, band.top}, tl);
    canvas.drawRect(Rect{mx, band.bottom, band.right, band.top}, tr);
    canvas.restore();
  }

  if (histogram && max_log_count > 1.0f) {
    const int N = std::clamp((int)(band.Width() / 0.45_mm), 24, 160);
    auto sample = [&](int i) {
      float f = (float)i / (float)N;
      int bin = std::clamp((int)std::lround(f * 255.f), 0, 255);
      float h = std::log((float)histogram[bin] + 1.0f) / max_log_count;
      return H * 0.10f + h * (H * 0.86f);
    };
    SkPathBuilder mb;
    mb.moveTo(band.left, baseline);
    for (int i = 0; i <= N; ++i) {
      float x = band.left + (float)i / (float)N * band.Width();
      mb.lineTo(x, baseline + sample(i));
    }
    mb.lineTo(band.right, baseline);
    mb.close();
    SkPath mountain = WobblePath(mb.detach(), kWonk * 0.5f, kSeg, Hash2(seed, 7));
    canvas.save();
    canvas.clipRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), true);
    FillPath(canvas, mountain, disabled ? 0xff9a9a9a : 0xff6f7b80);
    Rect keep_half = keep_above ? Rect{mx, band.bottom - H, band.right, band.top + H}
                                : Rect{band.left, band.bottom - H, mx, band.top + H};
    canvas.save();
    canvas.clipRect(keep_half, true);
    FillPath(canvas, mountain, disabled ? 0xffbdbdbd : kCyan);
    canvas.restore();
    SketchyStroke(canvas, mountain, kInk, kStrokeHair, Hash2(seed, 8), 1);
    canvas.restore();
  }

  {
    Rect ramp{band.left, baseline - H * 0.30f, band.right, baseline - H * 0.10f};
    const int steps = 8;
    for (int i = 0; i < steps; ++i) {
      float x0 = ramp.left + ramp.Width() * i / steps;
      float x1 = ramp.left + ramp.Width() * (i + 1) / steps;
      uint8_t g = (uint8_t)std::lround(255.f * i / (steps - 1));
      SkPaint sp;
      sp.setColor(disabled ? SkColorSetARGB(255, 200, 200, 200) : SkColorSetARGB(255, g, g, g));
      canvas.drawRect(Rect{x0, ramp.bottom, x1, ramp.top}, sp);
    }
    canvas.drawPath(WobbleRect(ramp, kWonk * 0.5f, kSeg, Hash2(seed, 3)),
                    InkPaint(kInk, kStrokeHair));
  }

  SketchyStroke(canvas, WonkyRoundRect(band, H * 0.10f, kWonk * 0.6f, Hash2(seed, 1)), kInk,
                kStroke, Hash2(seed, 2), 1);

  const SkColor knob_fill = disabled ? kGray : (pressed ? kRed : kYellow);
  const float knob_r = H * 0.20f;
  const float knob_y = band.top + H * 0.16f;
  const float push = pressed ? H * 0.04f : 0.f;
  SkPath stem =
      WobbleLine({mx, knob_y}, {mx, band.bottom - H * 0.22f}, kWonk * 0.7f, kSeg, Hash2(seed, 11));
  if (!disabled)
    HandShadow(canvas, stem, {kShadowDX * 0.5f, -kShadowDY * 0.5f}, kShadow, Hash2(seed, 12));
  canvas.save();
  canvas.translate(push, -push);
  SketchyStroke(canvas, stem, kInk, kStrokeBold, Hash2(seed, 13), 2);
  SkPath nib = SkPathBuilder()
                   .moveTo(mx - H * 0.10f, band.bottom - H * 0.10f)
                   .lineTo(mx + H * 0.10f, band.bottom - H * 0.10f)
                   .lineTo(mx, band.bottom - H * 0.30f)
                   .close()
                   .detach();
  nib = WobblePath(nib, kWonk * 0.6f, kSeg, Hash2(seed, 14));
  FillPath(canvas, nib, disabled ? kGray : kInk);
  SkPath knob = WobbleEllipse({mx, knob_y}, knob_r, knob_r * 0.94f, kWonk, Hash2(seed, 15), 40);
  MisregFill(canvas, knob, knob_fill, Hash2(seed, 16));
  SketchyStroke(canvas, knob, kInk, kStrokeBold, Hash2(seed, 17), 2);
  if (hover && !disabled) {
    SketchyStroke(canvas, knob, kInk, kStrokeBold * 1.4f, Hash2(seed, 18), 1);
  }
  if (show_comparator && !disabled) {
    float dir = keep_above ? 1.f : -1.f;
    SkPath beak = SkPathBuilder()
                      .moveTo(mx + dir * knob_r * 0.5f, knob_y + knob_r * 0.5f)
                      .lineTo(mx + dir * knob_r * 1.5f, knob_y)
                      .lineTo(mx + dir * knob_r * 0.5f, knob_y - knob_r * 0.5f)
                      .close()
                      .detach();
    beak = WobblePath(beak, kWonk * 0.5f, kSeg, Hash2(seed, 19));
    FillPath(canvas, beak, kGreen);
    SketchyStroke(canvas, beak, kInk, kStroke, Hash2(seed, 20), 1);
  }
  canvas.restore();

  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)std::lround(value));
    float fs = std::clamp(H * 0.34f, 1.5_mm, 5.8_mm);
    float tw = TextWidth(buf, fs);
    float cw = tw + fs * 0.9f, ch = fs * 1.5f;
    float cx = std::clamp(mx, band.left + cw * 0.5f, band.right - cw * 0.5f);
    float cy = knob_y + knob_r + ch * 0.62f;
    Rect chip{cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2};
    SkPath cp = WonkyRoundRect(chip, ch * 0.35f, kWonk, Hash2(seed, 21));
    if (!disabled)
      HandShadow(canvas, cp, {kShadowDX * 0.5f, -kShadowDY * 0.5f}, kShadow, Hash2(seed, 22));
    MisregFill(canvas, cp, disabled ? kGray : kYellow, Hash2(seed, 23));
    SketchyStroke(canvas, cp, kInk, kStroke, Hash2(seed, 24), 1);
    DrawText(canvas, buf, {cx - tw / 2, cy - fs * 0.36f}, fs, kInk, false, 0);
  }

  if (disabled) HatchRect(canvas, band, kInkSoft, H * 0.16f, Hash2(seed, 30));
}

void DrawWindow(SkCanvas& canvas, const Rect& band, float lo, float hi, float vmin, float vmax,
                const uint32_t* histogram, float max_log_count, beta::State state_lo,
                beta::State state_hi, uint32_t seed) {
  using namespace beta;
  const bool disabled = state_lo == State::Disabled && state_hi == State::Disabled;
  const float H = band.Height();
  const float lox = LevelValueToX(band, lo, vmin, vmax);
  const float hix = LevelValueToX(band, hi, vmin, vmax);
  const float baseline = band.bottom;

  {
    SkPaint paper;
    paper.setAntiAlias(true);
    paper.setColor(disabled ? kGray : kPaperCream);
    canvas.drawRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), paper);
    canvas.save();
    canvas.clipRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), true);
    SkPaint tl, tr;
    tl.setColor(disabled ? 0x14000000 : 0x30101010);
    tr.setColor(disabled ? 0x10ffffff : 0x30ffffff);
    canvas.drawRect(Rect{band.left, band.bottom, lox, band.top}, tl);
    canvas.drawRect(Rect{hix, band.bottom, band.right, band.top}, tr);
    canvas.restore();
  }

  if (histogram && max_log_count > 1.0f) {
    const int N = std::clamp((int)(band.Width() / 0.45_mm), 24, 160);
    auto sample = [&](int i) {
      float f = (float)i / (float)N;
      int bin = std::clamp((int)std::lround(f * 255.f), 0, 255);
      float h = std::log((float)histogram[bin] + 1.0f) / max_log_count;
      return H * 0.10f + h * (H * 0.86f);
    };
    SkPathBuilder mb;
    mb.moveTo(band.left, baseline);
    for (int i = 0; i <= N; ++i) {
      float x = band.left + (float)i / (float)N * band.Width();
      mb.lineTo(x, baseline + sample(i));
    }
    mb.lineTo(band.right, baseline);
    mb.close();
    SkPath mountain = WobblePath(mb.detach(), kWonk * 0.5f, kSeg, Hash2(seed, 7));
    canvas.save();
    canvas.clipRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), true);
    FillPath(canvas, mountain, disabled ? 0xff9a9a9a : 0xff6f7b80);
    canvas.save();
    canvas.clipRect(Rect{lox, band.bottom - H, hix, band.top + H}, true);
    FillPath(canvas, mountain, disabled ? 0xffbdbdbd : kCyan);
    canvas.restore();
    SketchyStroke(canvas, mountain, kInk, kStrokeHair, Hash2(seed, 8), 1);
    canvas.restore();
  }

  {
    Rect ramp{band.left, baseline - H * 0.30f, band.right, baseline - H * 0.10f};
    const int steps = 8;
    for (int i = 0; i < steps; ++i) {
      float x0 = ramp.left + ramp.Width() * i / steps;
      float x1 = ramp.left + ramp.Width() * (i + 1) / steps;
      uint8_t g = (uint8_t)std::lround(255.f * i / (steps - 1));
      SkPaint sp;
      sp.setColor(disabled ? SkColorSetARGB(255, 200, 200, 200) : SkColorSetARGB(255, g, g, g));
      canvas.drawRect(Rect{x0, ramp.bottom, x1, ramp.top}, sp);
    }
    canvas.drawPath(WobbleRect(ramp, kWonk * 0.5f, kSeg, Hash2(seed, 3)),
                    InkPaint(kInk, kStrokeHair));
  }

  SketchyStroke(canvas, WonkyRoundRect(band, H * 0.10f, kWonk * 0.6f, Hash2(seed, 1)), kInk,
                kStroke, Hash2(seed, 2), 1);

  auto marker = [&](float mx, float value, beta::State st, float beak_dir, uint32_t mseed) {
    const bool mdis = st == State::Disabled;
    const bool mpress = st == State::Pressed || st == State::Active;
    const bool mhover = st == State::Hover;
    const SkColor knob_fill = mdis ? kGray : (mpress ? kRed : kYellow);
    const float knob_r = H * 0.20f;
    const float knob_y = band.top + H * 0.16f;
    const float push = mpress ? H * 0.04f : 0.f;
    SkPath stem = WobbleLine({mx, knob_y}, {mx, band.bottom - H * 0.22f}, kWonk * 0.7f, kSeg,
                             Hash2(mseed, 11));
    if (!mdis)
      HandShadow(canvas, stem, {kShadowDX * 0.5f, -kShadowDY * 0.5f}, kShadow, Hash2(mseed, 12));
    canvas.save();
    canvas.translate(push, -push);
    SketchyStroke(canvas, stem, kInk, kStrokeBold, Hash2(mseed, 13), 2);
    SkPath nib = SkPathBuilder()
                     .moveTo(mx - H * 0.10f, band.bottom - H * 0.10f)
                     .lineTo(mx + H * 0.10f, band.bottom - H * 0.10f)
                     .lineTo(mx, band.bottom - H * 0.30f)
                     .close()
                     .detach();
    nib = WobblePath(nib, kWonk * 0.6f, kSeg, Hash2(mseed, 14));
    FillPath(canvas, nib, mdis ? kGray : kInk);
    SkPath knob = WobbleEllipse({mx, knob_y}, knob_r, knob_r * 0.94f, kWonk, Hash2(mseed, 15), 40);
    MisregFill(canvas, knob, knob_fill, Hash2(mseed, 16));
    SketchyStroke(canvas, knob, kInk, kStrokeBold, Hash2(mseed, 17), 2);
    if (mhover && !mdis) {
      SketchyStroke(canvas, knob, kInk, kStrokeBold * 1.4f, Hash2(mseed, 18), 1);
    }
    if (!mdis) {
      SkPath beak = SkPathBuilder()
                        .moveTo(mx + beak_dir * knob_r * 0.5f, knob_y + knob_r * 0.5f)
                        .lineTo(mx + beak_dir * knob_r * 1.5f, knob_y)
                        .lineTo(mx + beak_dir * knob_r * 0.5f, knob_y - knob_r * 0.5f)
                        .close()
                        .detach();
      beak = WobblePath(beak, kWonk * 0.5f, kSeg, Hash2(mseed, 19));
      FillPath(canvas, beak, kGreen);
      SketchyStroke(canvas, beak, kInk, kStroke, Hash2(mseed, 20), 1);
    }
    canvas.restore();
    {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", (int)std::lround(value));
      float fs = std::clamp(H * 0.34f, 1.5_mm, 5.8_mm);
      float tw = TextWidth(buf, fs);
      float cw = tw + fs * 0.9f, ch = fs * 1.5f;
      float cx = std::clamp(mx, band.left + cw * 0.5f, band.right - cw * 0.5f);
      float cy = knob_y + knob_r + ch * 0.62f;
      Rect chip{cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2};
      SkPath cp = WonkyRoundRect(chip, ch * 0.35f, kWonk, Hash2(mseed, 21));
      if (!mdis)
        HandShadow(canvas, cp, {kShadowDX * 0.5f, -kShadowDY * 0.5f}, kShadow, Hash2(mseed, 22));
      MisregFill(canvas, cp, mdis ? kGray : kYellow, Hash2(mseed, 23));
      SketchyStroke(canvas, cp, kInk, kStroke, Hash2(mseed, 24), 1);
      DrawText(canvas, buf, {cx - tw / 2, cy - fs * 0.36f}, fs, kInk, false, 0);
    }
  };
  marker(lox, lo, state_lo, +1.f, Hash2(seed, 100));
  marker(hix, hi, state_hi, -1.f, Hash2(seed, 200));

  if (disabled) HatchRect(canvas, band, kInkSoft, H * 0.16f, Hash2(seed, 30));
}

Rect ConnectivityCell(const Rect& r, int which) {
  float w = r.Width() / 2.f;
  float x = r.left + which * w + (which ? 0.45_mm : 0.f);
  return Rect{x, r.bottom, x + w - 0.45_mm, r.top};
}
int ConnectivityHit(const Rect& r, Vec2 p) {
  if (p.x < r.left || p.x > r.right || p.y < r.bottom || p.y > r.top) return 0;
  return p.x < r.CenterX() ? 4 : 8;
}
void DrawConnectivity(SkCanvas& canvas, const Rect& r, bool eight, beta::State state,
                      uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  for (int which = 0; which < 2; ++which) {
    Rect cell = ConnectivityCell(r, which);
    bool sel = ((which == 1) == eight) && !disabled;
    SkPath cp = WonkyRoundRect(cell, cell.Height() * 0.18f, kWonk * 0.6f, Hash2(seed, which));
    if (sel) HandShadow(canvas, cp, {0.29_mm, -0.29_mm}, kShadow, Hash2(seed, 10u + which));
    MisregFill(canvas, cp, sel ? kPaper : kGray, Hash2(seed, 20u + which));
    SketchyStroke(canvas, cp, kInk, sel ? kStroke : kStrokeHair, Hash2(seed, 30u + which),
                  sel ? 2 : 1);
    Vec2 c = cell.Center();
    float L = cell.Height() * 0.30f;
    SkColor ink = sel ? kInk : kInkSoft;
    int dirs = which == 0 ? 4 : 8;
    for (int d = 0; d < dirs; ++d) {
      float ang = (float)d * (kPi * 2 / dirs);
      Vec2 e = c + Vec2::Polar(-SinCos::FromRadians(ang), L);
      canvas.drawPath(WobbleLine(c, e, 0.15_mm, 0.73_mm, Hash2(seed, 40u + which * 8u + d)),
                      InkPaint(ink, sel ? kStrokeBold : kStroke));
    }
    SkPath body = WobbleEllipse(c, cell.Height() * 0.09f, cell.Height() * 0.09f, kWonk * 0.5f,
                                Hash2(seed, 60u + which), 24);
    FillPath(canvas, body, ink);
    const char* num = which == 0 ? "4" : "8";
    DrawText(canvas, num, {cell.left + 0.6_mm, cell.bottom + 0.6_mm}, cell.Height() * 0.34f, ink,
             false, 0);
    if (sel) Highlight(canvas, cell, kBlue, Hash2(seed, 70));
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, r.Height() * 0.2f, Hash2(seed, 80));
}

int RegionHit(const Rect& mq, Vec2 p, float grip) {
  Vec2 corners[4] = {
      {mq.left, mq.top}, {mq.right, mq.top}, {mq.left, mq.bottom}, {mq.right, mq.bottom}};
  for (int i = 0; i < 4; ++i)
    if (std::abs(p.x - corners[i].x) <= grip && std::abs(p.y - corners[i].y) <= grip) return 1 + i;
  if (p.x >= mq.left && p.x <= mq.right && p.y >= mq.bottom && p.y <= mq.top) return 5;
  return 0;
}
void DrawRegion(SkCanvas& canvas, const Rect& fit, const Rect& mq, beta::State state,
                uint32_t seed) {
  using namespace beta;
  const bool active = state == State::Pressed || state == State::Active;
  {
    SkPaint dim;
    dim.setColor(0x55101010);
    canvas.drawRect(Rect{fit.left, fit.bottom, fit.right, mq.bottom}, dim);
    canvas.drawRect(Rect{fit.left, mq.top, fit.right, fit.top}, dim);
    canvas.drawRect(Rect{fit.left, mq.bottom, mq.left, mq.top}, dim);
    canvas.drawRect(Rect{mq.right, mq.bottom, fit.right, mq.top}, dim);
  }
  {
    const float dash = 1.3_mm, gap = 0.9_mm;
    SkColor ink = active ? kRed : kInk;
    auto edge = [&](Vec2 a, Vec2 b, uint32_t eseed) {
      float len = std::hypot(b.x - a.x, b.y - a.y);
      int n = std::max(1, (int)(len / (dash + gap)));
      for (int i = 0; i < n; ++i) {
        float t0 = (i * (dash + gap)) / len, t1 = std::min(1.f, t0 + dash / len);
        Vec2 p0{a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0};
        Vec2 p1{a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1};
        canvas.drawPath(WobbleLine(p0, p1, 0.15_mm, 0.9_mm, Hash2(eseed, (uint32_t)i)),
                        InkPaint(ink, kStroke));
      }
    };
    edge({mq.left, mq.top}, {mq.right, mq.top}, Hash2(seed, 1));
    edge({mq.right, mq.top}, {mq.right, mq.bottom}, Hash2(seed, 2));
    edge({mq.right, mq.bottom}, {mq.left, mq.bottom}, Hash2(seed, 3));
    edge({mq.left, mq.bottom}, {mq.left, mq.top}, Hash2(seed, 4));
  }
  Vec2 corners[4] = {
      {mq.left, mq.top}, {mq.right, mq.top}, {mq.left, mq.bottom}, {mq.right, mq.bottom}};
  for (int i = 0; i < 4; ++i) {
    Rect g{corners[i].x - 0.9_mm, corners[i].y - 0.9_mm, corners[i].x + 0.85_mm,
           corners[i].y + 0.85_mm};
    SkPath gp = WonkyRoundRect(g, 0.36_mm, kWonk * 0.5f, Hash2(seed, 10u + i));
    MisregFill(canvas, gp, active ? kRed : kYellow, Hash2(seed, 20u + i));
    SketchyStroke(canvas, gp, kInk, kStrokeHair, Hash2(seed, 30u + i), 1);
  }
}

bool TransformRingHit(Vec2 c, float radius, Vec2 p, float band) {
  float d = std::hypot(p.x - c.x, p.y - c.y);
  return std::abs(d - radius) <= band;
}
float TransformRingAngleAt(Vec2 c, Vec2 p) { return std::atan2(p.x - c.x, p.y - c.y) * 57.29578f; }
void DrawTransformRing(SkCanvas& canvas, Vec2 c, float radius, float angle_deg, beta::State state,
                       uint32_t seed) {
  using namespace beta;
  const bool active = state == State::Pressed || state == State::Active;
  const int kSegs = 36;
  for (int i = 0; i < kSegs; i += 2) {
    SinCos a0 = SinCos::FromDegrees(360.f * i / kSegs);
    SinCos a1 = SinCos::FromDegrees(360.f * (i + 1) / kSegs);
    Vec2 p0 = c + Vec2((float)a0.sin, (float)a0.cos) * radius;
    Vec2 p1 = c + Vec2((float)a1.sin, (float)a1.cos) * radius;
    canvas.drawPath(WobbleLine(p0, p1, 0.15_mm, 1.0_mm, Hash2(seed, (uint32_t)i)),
                    InkPaint(kInkSoft, kStrokeHair));
  }
  for (int q = 0; q < 4; ++q) {
    SinCos a = SinCos::FromDegrees(90.f * q);
    Vec2 dir{(float)a.sin, (float)a.cos};
    Vec2 t0 = c + dir * (radius - 0.73_mm);
    Vec2 t1 = c + dir * (radius + 0.73_mm);
    canvas.drawPath(WobbleLine(t0, t1, 0.12_mm, 0.73_mm, Hash2(seed, 50u + q)),
                    InkPaint(kInk, kStrokeHair));
  }
  SinCos ka = SinCos::FromDegrees(angle_deg);
  Vec2 k = c + Vec2((float)ka.sin, (float)ka.cos) * radius;
  canvas.drawPath(WobbleLine(c, k, 0.15_mm, 1.3_mm, Hash2(seed, 60)),
                  InkPaint(kInkSoft, kStrokeHair));
  SkPath knob = WobbleEllipse(k, 1.1_mm, 1.0_mm, kWonk * 0.6f, Hash2(seed, 61), 28);
  MisregFill(canvas, knob, active ? kRed : kYellow, Hash2(seed, 62));
  SketchyStroke(canvas, knob, kInk, kStrokeBold, Hash2(seed, 63), 2);
}

bool PolarityHit(const Rect& r, Vec2 p) {
  return p.x >= r.left - 0.6_mm && p.x <= r.right + 0.6_mm && p.y >= r.bottom - 0.6_mm &&
         p.y <= r.top + 0.6_mm;
}
void DrawPolarity(SkCanvas& canvas, const Rect& r, bool bright_hot, beta::State state,
                  uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  float rad = r.Height() / 2.f;
  Vec2 dark{r.left + rad, r.CenterY()}, bright{r.right - rad, r.CenterY()};
  canvas.drawPath(WobbleLine({dark.x + rad * 0.6f, r.CenterY()},
                             {bright.x - rad * 0.6f, r.CenterY()}, 0.15_mm, 0.9_mm, Hash2(seed, 1)),
                  InkPaint(disabled ? kInkSoft : kInk, kStroke));
  SkPath dp = WobbleEllipse(dark, rad * 0.78f, rad * 0.74f, kWonk * 0.5f, Hash2(seed, 2), 30);
  FillPath(canvas, dp, disabled ? kGray : kInk);
  SketchyStroke(canvas, dp, kInk, kStrokeHair, Hash2(seed, 3), 1);
  SkPath bp = WobbleEllipse(bright, rad * 0.78f, rad * 0.74f, kWonk * 0.5f, Hash2(seed, 4), 30);
  MisregFill(canvas, bp, disabled ? kGray : kPaper, Hash2(seed, 5));
  SketchyStroke(canvas, bp, kInk, kStrokeHair, Hash2(seed, 6), 1);
  if (!disabled) {
    Vec2 hot = bright_hot ? bright : dark;
    SkPath halo = WobbleEllipse(hot, rad * 1.15f, rad * 1.1f, kWonk * 0.6f, Hash2(seed, 7), 34);
    canvas.drawPath(halo, InkPaint(kYellow, kStrokeBold));
    canvas.drawPath(WobblePath(halo, kWonk * 0.4f, kSeg, Hash2(seed, 8)),
                    InkPaint(kInk, kStrokeHair));
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, r.Height() * 0.3f, Hash2(seed, 9));
}

Rect PaletteCell(const Rect& r, int i) {
  float cw = r.Width() / 5.f, ch = r.Height() / 2.f;
  int row = i / 5;
  float x = r.left + (i % 5) * cw + 0.22_mm;
  float y = r.top - (row + 1) * ch + 0.22_mm;
  return Rect{x, y, x + cw - 0.45_mm, y + ch - 0.45_mm};
}
int PaletteHit(const Rect& r, Vec2 p) {
  if (p.x < r.left || p.x > r.right || p.y < r.bottom || p.y > r.top) return -1;
  int col = std::clamp((int)((p.x - r.left) / (r.Width() / 5.f)), 0, 4);
  int row = std::clamp((int)((r.top - p.y) / (r.Height() / 2.f)), 0, 1);
  return row * 5 + col;
}
void DrawPalette(SkCanvas& canvas, const Rect& r, int selected, beta::State state, uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  for (int i = 0; i < kPaletteCount; ++i) {
    Rect cell = PaletteCell(r, i);
    SkPath cp = WonkyRoundRect(cell, 0.36_mm, kWonk * 0.4f, Hash2(seed, (uint32_t)i));
    MisregFill(canvas, cp, disabled ? kGray : kPaletteColors[i], Hash2(seed, 20u + i));
    SketchyStroke(canvas, cp, kInk, i == selected ? kStroke : kStrokeHair, Hash2(seed, 40u + i),
                  i == selected ? 2 : 1);
    if (i == selected && !disabled) Highlight(canvas, cell, kBlue, Hash2(seed, 60));
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, r.Height() * 0.25f, Hash2(seed, 80));
}

void DrawDepthChip(SkCanvas& canvas, const Rect& r, int depth, bool has_cmap, beta::State state,
                   uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  SkPath chip = WonkyRoundRect(r, r.Height() * 0.22f, kWonk * 0.5f, Hash2(seed, 1));
  MisregFill(canvas, chip, disabled ? kGray : kPaper, Hash2(seed, 2));
  SketchyStroke(canvas, chip, kInk, kStrokeHair, Hash2(seed, 3), 1);
  SkColor ink = disabled ? kInkSoft : kInk;
  float h = r.Height();
  Rect g{r.left + h * 0.18f, r.bottom + h * 0.22f, r.left + h * 0.18f + h * 0.56f,
         r.bottom + h * 0.22f + h * 0.56f};
  if (depth == 1) {
    float c = g.Width() / 2;
    SkPaint pb;
    pb.setAntiAlias(true);
    pb.setColor(ink);
    canvas.drawRect(Rect{g.left, g.top - c, g.left + c, g.top}, pb);
    canvas.drawRect(Rect{g.left + c, g.bottom, g.left + 2 * c, g.bottom + c}, pb);
    SkPaint pr;
    pr.setAntiAlias(true);
    pr.setStyle(SkPaint::kStroke_Style);
    pr.setStrokeWidth(0.17_mm);
    pr.setColor(ink);
    canvas.drawRect(g, pr);
  } else if (depth <= 8 && !has_cmap) {
    for (int i = 0; i < 4; ++i) {
      uint8_t v = (uint8_t)(255 - i * 70);
      SkPaint ps;
      ps.setAntiAlias(true);
      ps.setColor(disabled ? kGray : SkColorSetARGB(255, v, v, v));
      float x = g.left + i * g.Width() / 4;
      canvas.drawRect(Rect{x, g.bottom, x + g.Width() / 4, g.top}, ps);
    }
    SkPaint pr;
    pr.setAntiAlias(true);
    pr.setStyle(SkPaint::kStroke_Style);
    pr.setStrokeWidth(0.17_mm);
    pr.setColor(ink);
    canvas.drawRect(g, pr);
  } else if (has_cmap) {
    const SkColor cs[3] = {kRed, kYellow, kCyan};
    for (int i = 0; i < 3; ++i) {
      SkPaint ps;
      ps.setAntiAlias(true);
      ps.setColor(disabled ? kGray : cs[i]);
      float y = g.top - (i + 1) * g.Height() / 3;
      Rect cell{g.left, y, g.left + g.Width(), y + g.Height() / 3 - 0.15_mm};
      canvas.drawRect(cell, ps);
    }
    SkPaint pr;
    pr.setAntiAlias(true);
    pr.setStyle(SkPaint::kStroke_Style);
    pr.setStrokeWidth(0.17_mm);
    pr.setColor(ink);
    canvas.drawRect(g, pr);
  } else {
    const SkColor cs[3] = {0xffed1c24, 0xff22b14c, 0xff3f48cc};
    float dr = g.Width() * 0.30f;
    Vec2 centers[3] = {
        {g.left + dr, g.top - dr}, {g.right - dr, g.top - dr * 1.4f}, {g.CenterX(), g.bottom + dr}};
    for (int i = 0; i < 3; ++i) {
      SkPaint pd;
      pd.setAntiAlias(true);
      pd.setColor(disabled ? kGray : cs[i]);
      canvas.drawCircle(centers[i].x, centers[i].y, dr, pd);
    }
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", depth);
  float fs = h * 0.52f;
  DrawText(canvas, buf, {g.right + h * 0.18f, r.bottom + h * 0.26f}, fs, ink, false, 0);
}

void StampGridMetrics(const Rect& rect, int w, int h, float& cell, float& gx, float& gy) {
  cell = std::min(rect.Width() / std::max(1, w), rect.Height() / std::max(1, h));
  gx = rect.left + (rect.Width() - cell * w) / 2;
  gy = rect.bottom + (rect.Height() - cell * h) / 2;
}

bool StampCellAt(const Rect& rect, Vec2 p, int w, int h, int& cx, int& cy) {
  float cell, gx, gy;
  StampGridMetrics(rect, w, h, cell, gx, gy);
  if (p.x < gx || p.x > gx + cell * w || p.y < gy || p.y > gy + cell * h) return false;
  cx = std::clamp((int)((p.x - gx) / cell), 0, w - 1);
  cy = std::clamp((int)((gy + cell * h - p.y) / cell), 0, h - 1);
  return true;
}

void StampPresetBrick(uint8_t* cells, int w, int h) {
  for (int i = 0; i < w * h; ++i) cells[i] = 1;
}
void StampPresetCross(uint8_t* cells, int w, int h) {
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) cells[j * w + i] = (i == w / 2 || j == h / 2) ? 1 : 0;
}
void StampPresetDisk(uint8_t* cells, int w, int h) {
  float cx = (w - 1) / 2.f, cy = (h - 1) / 2.f, r = std::min(w, h) / 2.f + 0.25f;
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) cells[j * w + i] = (std::hypot(i - cx, j - cy) <= r) ? 1 : 0;
}

void DrawStamp(SkCanvas& canvas, const Rect& rect, const uint8_t* cells, int w, int h, int ox,
               int oy, beta::State state, uint32_t seed) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  float cell, gx, gy;
  StampGridMetrics(rect, w, h, cell, gx, gy);

  for (int j = 0; j < h; ++j) {
    for (int i = 0; i < w; ++i) {
      int v = cells ? cells[j * w + i] : 0;
      uint32_t cs = Hash3(seed, (uint32_t)i, (uint32_t)j);
      float cx0 = gx + i * cell + cell * 0.09f, cy0 = gy + (h - 1 - j) * cell + cell * 0.09f;
      Rect c{cx0, cy0, cx0 + cell * 0.82f, cy0 + cell * 0.82f};
      SkPath cp = WonkyRoundRect(c, cell * 0.16f, kWonk * 0.5f, cs);
      if (v == 1) {
        MisregFill(canvas, cp, disabled ? kGray : kCyan, cs);
        SketchyStroke(canvas, cp, kInk, kStroke, cs, 1);
      } else if (v == 2) {
        SketchyStroke(canvas, cp, disabled ? kGray : kInkSoft, kStrokeHair, cs, 1);
        float in = cell * 0.2f;
        canvas.drawPath(WobbleLine({c.left + in, c.bottom + in}, {c.right - in, c.top - in},
                                   kWonk * 0.4f, kSeg, Hash2(cs, 7)),
                        InkPaint(disabled ? kGray : kRed, kStroke));
        canvas.drawPath(WobbleLine({c.right - in, c.bottom + in}, {c.left + in, c.top - in},
                                   kWonk * 0.4f, kSeg, Hash2(cs, 8)),
                        InkPaint(disabled ? kGray : kRed, kStroke));
      } else {
        SketchyStroke(canvas, cp, disabled ? kGray : kInkSoft, kStrokeHair, cs, 1);
      }
    }
  }
  if (ox >= 0 && oy >= 0 && ox < w && oy < h) {
    Vec2 oc = {gx + (ox + 0.5f) * cell, gy + (h - 1 - oy + 0.5f) * cell};
    SkPath ring = WobbleEllipse(oc, cell * 0.20f, cell * 0.20f, kWonk * 0.5f, Hash2(seed, 41), 22);
    SketchyStroke(canvas, ring, disabled ? kGrayDark : kRed, kStrokeBold, Hash2(seed, 42), 2);
  }
}

float ModeWheelAngle(int k, int n) { return kPi / 2 - k * (kPi * 2 / n); }

int ModeWheelHit(Vec2 c, float radius, Vec2 p, int n) {
  float dx = p.x - c.x, dy = p.y - c.y;
  if (std::hypot(dx, dy) > radius * 1.7f) return -1;
  float ang = std::atan2(dy, dx);
  int best = 0;
  float bestd = 1e9f;
  for (int k = 0; k < n; ++k) {
    float d = std::abs(std::remainder(ang - ModeWheelAngle(k, n), kPi * 2));
    if (d < bestd) {
      bestd = d;
      best = k;
    }
  }
  return best;
}

void DrawModeWheel(SkCanvas& canvas, Vec2 c, float radius, const char* const* labels, int n,
                   int selected, beta::State state, uint32_t seed, const SkPath* glyphs) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  SkPath dial = WobbleEllipse(c, radius, radius * 0.96f, kWonk, seed, 48);
  if (!disabled) HandShadow(canvas, dial, {kShadowDX * 0.6f, -kShadowDY * 0.6f}, kShadow, seed);
  MisregFill(canvas, dial, disabled ? kGray : kPaperCream, seed);
  SketchyStroke(canvas, dial, kInk, kStrokeBold, seed, 2);
  if (glyphs && selected >= 0 && selected < n && !glyphs[selected].isEmpty()) {
    // Opposite the needle, so the pointer doesn't cross the glyph.
    float ga = ModeWheelAngle(selected, n);
    Vec2 gc = c - Vec2::Polar(SinCos::FromRadians(ga), radius * 0.42f);
    SkMatrix m = SkMatrix::Translate(gc.x, gc.y);
    m.preScale(radius * 0.34f, radius * 0.34f);
    SkPaint ink;
    ink.setAntiAlias(true);
    ink.setColor(disabled ? kGrayDark : kInk);
    canvas.drawPath(glyphs[selected].makeTransform(m), ink);
  }

  float fs = radius * 0.42f;
  for (int k = 0; k < n; ++k) {
    float a = ModeWheelAngle(k, n);
    Vec2 dir = {std::cos(a), std::sin(a)};
    bool sel = k == selected;
    canvas.drawPath(WobbleLine(c + dir * (radius * 0.62f), c + dir * (radius * 0.95f), kWonk * 0.5f,
                               kSeg, Hash2(seed, k + 1)),
                    InkPaint(sel ? kRed : kInkSoft, sel ? kStrokeBold : kStroke));
    Vec2 lp = {c.x + dir.x * (radius + fs * 1.5f), c.y + dir.y * (radius + fs * 0.9f)};
    float tw = TextWidth(labels[k], fs);
    // Labels near the bottom pole slide outward so they don't collide under the dial.
    if (std::abs(dir.y) > 0.5f && std::abs(dir.x) > 0.15f)
      lp.x += (dir.x > 0 ? 1.f : -1.f) * tw * 0.35f;
    DrawText(canvas, labels[k], {lp.x - tw / 2, lp.y - fs * 0.34f}, fs,
             disabled ? kGray : (sel ? kInk : kInkSoft), false, 0);
  }
  float sa = ModeWheelAngle(selected, n);
  canvas.drawPath(WobbleLine(c, c + Vec2::Polar(SinCos::FromRadians(sa), radius * 0.7f), kWonk,
                             kSeg, Hash2(seed, 91)),
                  InkPaint(disabled ? kGrayDark : kRed, kStrokeBold));
  SkPath hub = WobbleEllipse(c, radius * 0.15f, radius * 0.15f, kWonk * 0.5f, Hash2(seed, 92), 16);
  MisregFill(canvas, hub, disabled ? kGrayDark : kYellow, Hash2(seed, 93));
  SketchyStroke(canvas, hub, kInk, kStroke, Hash2(seed, 94), 1);
}

void DrawCurveLUT(SkCanvas& canvas, const Rect& rect, const uint8_t* lut, beta::State state,
                  uint32_t seed) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  float w = rect.Width(), h = rect.Height();
  SkPaint paper;
  paper.setAntiAlias(true);
  paper.setColor(disabled ? kGray : kPaperCream);
  canvas.drawRect(rect, paper);
  SkPaint grid = InkPaint(0x28000000, kStrokeHair);
  for (int i = 1; i < 4; ++i) {
    float gx = rect.left + w * i / 4, gy = rect.bottom + h * i / 4;
    canvas.drawLine(gx, rect.bottom, gx, rect.top, grid);
    canvas.drawLine(rect.left, gy, rect.right, gy, grid);
  }
  canvas.drawLine(rect.left, rect.bottom, rect.right, rect.top,
                  InkPaint(disabled ? kGrayDark : kInkSoft, kStrokeHair));
  if (lut) {
    const int N = std::clamp((int)(w / 0.6_mm), 24, 128);
    SkPathBuilder cb;
    for (int k = 0; k <= N; ++k) {
      int i = std::clamp(k * 255 / N, 0, 255);
      float x = rect.left + w * i / 255.f;
      float y = rect.bottom + h * lut[i] / 255.f;
      if (k == 0)
        cb.moveTo(x, y);
      else
        cb.lineTo(x, y);
    }
    SkPath curve = WobblePath(cb.detach(), kWonk * 0.4f, kSeg, Hash2(seed, 7));
    SketchyStroke(canvas, curve, disabled ? kGrayDark : kCyan, kStrokeBold, Hash2(seed, 8), 2);
  }
  SketchyStroke(canvas, WobbleRect(rect, kWonk * 0.6f, kSeg, Hash2(seed, 1)), kInk, kStroke,
                Hash2(seed, 2), 1);
}

float DialAngleAt(Vec2 c, Vec2 p) {
  float a = std::atan2(p.x - c.x, p.y - c.y) * 57.29578f;
  if (a < 0) a += 360.f;
  return a;
}
void DrawDial(SkCanvas& canvas, Vec2 c, float radius, float angle_deg, beta::State state,
              uint32_t seed) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  for (int d = 0; d < 360; d += 30) {
    SinCos a = SinCos::FromDegrees(d);
    Vec2 dir{(float)a.sin, (float)a.cos};
    bool major = (d % 90 == 0);
    float t0 = major ? 0.80f : 0.90f;
    canvas.drawPath(WobbleLine(c + dir * (radius * t0), c + dir * (radius * 0.99f), kWonk * 0.4f,
                               kSeg, Hash2(seed, (uint32_t)d)),
                    InkPaint(disabled ? kGray : kInkSoft, major ? kStroke : kStrokeHair));
  }
  SinCos na = SinCos::FromDegrees(angle_deg);
  SkPath needle = WobbleLine(c, c + Vec2((float)na.sin, (float)na.cos) * (radius * 0.84f), kWonk,
                             kSeg, Hash2(seed, 0x91));
  if (!disabled) SketchyStroke(canvas, needle, kRed, kStrokeBold, Hash2(seed, 0x92), 2);
  SkPath hub =
      WobbleEllipse(c, radius * 0.10f, radius * 0.10f, kWonk * 0.5f, Hash2(seed, 0x93), 14);
  MisregFill(canvas, hub, disabled ? kGrayDark : kYellow, Hash2(seed, 0x94));
  SketchyStroke(canvas, hub, kInk, kStroke, Hash2(seed, 0x95), 1);
}

int ChannelTapAt(const Rect& r, Vec2 p, int n) {
  if (p.y < r.bottom || p.y > r.top || p.x < r.left || p.x > r.right) return -1;
  return std::clamp((int)((p.x - r.left) / (r.Width() / n)), 0, n - 1);
}
void DrawChannelTap(SkCanvas& canvas, const Rect& r, int selected, beta::State state, uint32_t seed,
                    const char* const* labels, const SkColor* colors, int n) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  static constexpr SkColor kDefCols[4] = {0xff8a8a8a, kRed, kGreen, kBlue};
  static const char* const kDefLabs[4] = {"LUM", "R", "G", "B"};
  const SkColor* cols = colors ? colors : kDefCols;
  const char* const* labs = labels ? labels : kDefLabs;
  float cw = r.Width() / n;
  for (int i = 0; i < n; ++i) {
    float x = r.left + i * cw + cw * 0.08f;
    Rect c{x, r.bottom, x + cw * 0.84f, r.top};
    bool sel = (i == selected);
    SkColor fill = disabled ? kGray : cols[i];
    SkPath cp = WonkyRoundRect(c, c.Height() * 0.22f, kWonk * 0.6f, Hash2(seed, (uint32_t)i));
    MisregFill(canvas, cp, fill, Hash2(seed, (uint32_t)(i + 10)));
    if (!sel) {
      SkPaint wash;
      wash.setColor(0xb0f4efe4);
      canvas.drawPath(cp, wash);
    }
    SketchyStroke(canvas, cp, kInk, sel ? kStrokeBold : kStroke, Hash2(seed, (uint32_t)(i + 20)),
                  sel ? 2 : 1);
    DrawTextIn(canvas, labs[i], c, c.Height() * 0.42f, sel ? TextOn(fill) : kInkSoft,
               TextAlign::Center, false, 0);
    if (sel && !disabled)
      canvas.drawPath(WobblePath(cp, kWonk, kSeg, Hash2(seed, (uint32_t)(i + 30))),
                      InkPaint(kYellow, kStroke));
  }
}

}  // namespace automat::ui::leptonica
