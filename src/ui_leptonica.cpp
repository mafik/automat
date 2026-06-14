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

float LevelValueToX(const SkRect& band, float value, float vmin, float vmax) {
  float t = std::clamp((value - vmin) / std::max(1e-6f, vmax - vmin), 0.f, 1.f);
  return band.fLeft + t * band.width();
}
float LevelXToValue(const SkRect& band, float x, float vmin, float vmax) {
  float t = std::clamp((x - band.fLeft) / std::max(1e-6f, band.width()), 0.f, 1.f);
  return vmin + t * (vmax - vmin);
}
bool LevelGrabsMarker(const SkRect& band, SkPoint p, float value, float vmin, float vmax,
                      float grip) {
  float mx = LevelValueToX(band, value, vmin, vmax);
  if (grip <= 0.f) grip = band.height() * 0.22f;
  float knob_y = band.fBottom + band.height() * 0.16f;
  float knob_r = band.height() * 0.20f;
  bool on_line = std::abs(p.fX - mx) <= grip && p.fY <= knob_y + knob_r &&
                 p.fY >= band.fTop - band.height() * 0.24f;
  bool on_knob = std::hypot(p.fX - mx, p.fY - knob_y) <= knob_r + grip * 0.5f;
  return on_line || on_knob;
}

void DrawLevel(SkCanvas& canvas, const SkRect& band, float value, float vmin, float vmax,
               const uint32_t* histogram, float max_log_count, bool keep_above,
               bool show_comparator, beta::State state, uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  const bool pressed = state == State::Pressed || state == State::Active;
  const bool hover = state == State::Hover;

  const float H = band.height();
  const float mx = LevelValueToX(band, value, vmin, vmax);
  const float baseline = band.fTop;
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
    canvas.drawRect(SkRect::MakeLTRB(band.fLeft, band.fTop, mx, band.fBottom), tl);
    canvas.drawRect(SkRect::MakeLTRB(mx, band.fTop, band.fRight, band.fBottom), tr);
    canvas.restore();
  }

  if (histogram && max_log_count > 1.0f) {
    const int N = std::clamp((int)(band.width() / 0.45_mm), 24, 160);
    auto sample = [&](int i) {
      float f = (float)i / (float)N;
      int bin = std::clamp((int)std::lround(f * 255.f), 0, 255);
      float h = std::log((float)histogram[bin] + 1.0f) / max_log_count;  // 0..1
      return H * 0.10f + h * (H * 0.86f);
    };
    SkPathBuilder mb;
    mb.moveTo(band.fLeft, baseline);
    for (int i = 0; i <= N; ++i) {
      float x = band.fLeft + (float)i / (float)N * band.width();
      mb.lineTo(x, baseline + sample(i));
    }
    mb.lineTo(band.fRight, baseline);
    mb.close();
    SkPath mountain = WobblePath(mb.detach(), kWonk * 0.5f, kSeg, Hash2(seed, 7));
    canvas.save();
    canvas.clipRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), true);
    FillPath(canvas, mountain, disabled ? 0xff9a9a9a : 0xff6f7b80);
    SkRect keep_half = keep_above
                           ? SkRect::MakeLTRB(mx, band.fTop - H, band.fRight, band.fBottom + H)
                           : SkRect::MakeLTRB(band.fLeft, band.fTop - H, mx, band.fBottom + H);
    canvas.save();
    canvas.clipRect(keep_half, true);
    FillPath(canvas, mountain, disabled ? 0xffbdbdbd : kCyan);
    canvas.restore();
    SketchyStroke(canvas, mountain, kInk, kStrokeHair, Hash2(seed, 8), 1);
    canvas.restore();
  }

  {
    SkRect ramp =
        SkRect::MakeLTRB(band.fLeft, baseline - H * 0.30f, band.fRight, baseline - H * 0.10f);
    const int steps = 8;
    for (int i = 0; i < steps; ++i) {
      float x0 = ramp.fLeft + ramp.width() * i / steps;
      float x1 = ramp.fLeft + ramp.width() * (i + 1) / steps;
      uint8_t g = (uint8_t)std::lround(255.f * i / (steps - 1));
      SkPaint sp;
      sp.setColor(disabled ? SkColorSetARGB(255, 200, 200, 200) : SkColorSetARGB(255, g, g, g));
      canvas.drawRect(SkRect::MakeLTRB(x0, ramp.fTop, x1, ramp.fBottom), sp);
    }
    canvas.drawPath(WobbleRect(ramp, kWonk * 0.5f, kSeg, Hash2(seed, 3)),
                    InkPaint(kInk, kStrokeHair));
  }

  SketchyStroke(canvas, WonkyRoundRect(band, H * 0.10f, kWonk * 0.6f, Hash2(seed, 1)), kInk,
                kStroke, Hash2(seed, 2), 1);

  const SkColor knob_fill = disabled ? kGray : (pressed ? kRed : kYellow);
  const float knob_r = H * 0.20f;
  const float knob_y = band.fBottom + H * 0.16f;
  const float push = pressed ? H * 0.04f : 0.f;
  SkPath stem =
      WobbleLine({mx, knob_y}, {mx, band.fTop - H * 0.22f}, kWonk * 0.7f, kSeg, Hash2(seed, 11));
  if (!disabled)
    HandShadow(canvas, stem, {kShadowDX * 0.5f, -kShadowDY * 0.5f}, kShadow, Hash2(seed, 12));
  canvas.save();
  canvas.translate(push, -push);
  SketchyStroke(canvas, stem, kInk, kStrokeBold, Hash2(seed, 13), 2);
  SkPath nib = SkPathBuilder()
                   .moveTo(mx - H * 0.10f, band.fTop - H * 0.10f)
                   .lineTo(mx + H * 0.10f, band.fTop - H * 0.10f)
                   .lineTo(mx, band.fTop - H * 0.30f)
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
    float cx = std::clamp(mx, band.fLeft + cw * 0.5f, band.fRight - cw * 0.5f);
    float cy = knob_y + knob_r + ch * 0.62f;
    SkRect chip = SkRect::MakeXYWH(cx - cw / 2, cy - ch / 2, cw, ch);
    SkPath cp = WonkyRoundRect(chip, ch * 0.35f, kWonk, Hash2(seed, 21));
    if (!disabled)
      HandShadow(canvas, cp, {kShadowDX * 0.5f, -kShadowDY * 0.5f}, kShadow, Hash2(seed, 22));
    MisregFill(canvas, cp, disabled ? kGray : kYellow, Hash2(seed, 23));
    SketchyStroke(canvas, cp, kInk, kStroke, Hash2(seed, 24), 1);
    DrawText(canvas, buf, {cx - tw / 2, cy - fs * 0.36f}, fs, kInk, false, 0);
  }

  if (disabled) HatchRect(canvas, band, kInkSoft, H * 0.16f, Hash2(seed, 30));
}

void DrawWindow(SkCanvas& canvas, const SkRect& band, float lo, float hi, float vmin, float vmax,
                const uint32_t* histogram, float max_log_count, beta::State state_lo,
                beta::State state_hi, uint32_t seed) {
  using namespace beta;
  const bool disabled = state_lo == State::Disabled && state_hi == State::Disabled;
  const float H = band.height();
  const float lox = LevelValueToX(band, lo, vmin, vmax);
  const float hix = LevelValueToX(band, hi, vmin, vmax);
  const float baseline = band.fTop;

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
    canvas.drawRect(SkRect::MakeLTRB(band.fLeft, band.fTop, lox, band.fBottom), tl);
    canvas.drawRect(SkRect::MakeLTRB(hix, band.fTop, band.fRight, band.fBottom), tr);
    canvas.restore();
  }

  if (histogram && max_log_count > 1.0f) {
    const int N = std::clamp((int)(band.width() / 0.45_mm), 24, 160);
    auto sample = [&](int i) {
      float f = (float)i / (float)N;
      int bin = std::clamp((int)std::lround(f * 255.f), 0, 255);
      float h = std::log((float)histogram[bin] + 1.0f) / max_log_count;
      return H * 0.10f + h * (H * 0.86f);
    };
    SkPathBuilder mb;
    mb.moveTo(band.fLeft, baseline);
    for (int i = 0; i <= N; ++i) {
      float x = band.fLeft + (float)i / (float)N * band.width();
      mb.lineTo(x, baseline + sample(i));
    }
    mb.lineTo(band.fRight, baseline);
    mb.close();
    SkPath mountain = WobblePath(mb.detach(), kWonk * 0.5f, kSeg, Hash2(seed, 7));
    canvas.save();
    canvas.clipRRect(SkRRect::MakeRectXY(band, H * 0.10f, H * 0.10f), true);
    FillPath(canvas, mountain, disabled ? 0xff9a9a9a : 0xff6f7b80);
    canvas.save();
    canvas.clipRect(SkRect::MakeLTRB(lox, band.fTop - H, hix, band.fBottom + H), true);
    FillPath(canvas, mountain, disabled ? 0xffbdbdbd : kCyan);
    canvas.restore();
    SketchyStroke(canvas, mountain, kInk, kStrokeHair, Hash2(seed, 8), 1);
    canvas.restore();
  }

  {
    SkRect ramp =
        SkRect::MakeLTRB(band.fLeft, baseline - H * 0.30f, band.fRight, baseline - H * 0.10f);
    const int steps = 8;
    for (int i = 0; i < steps; ++i) {
      float x0 = ramp.fLeft + ramp.width() * i / steps;
      float x1 = ramp.fLeft + ramp.width() * (i + 1) / steps;
      uint8_t g = (uint8_t)std::lround(255.f * i / (steps - 1));
      SkPaint sp;
      sp.setColor(disabled ? SkColorSetARGB(255, 200, 200, 200) : SkColorSetARGB(255, g, g, g));
      canvas.drawRect(SkRect::MakeLTRB(x0, ramp.fTop, x1, ramp.fBottom), sp);
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
    const float knob_y = band.fBottom + H * 0.16f;
    const float push = mpress ? H * 0.04f : 0.f;
    SkPath stem =
        WobbleLine({mx, knob_y}, {mx, band.fTop - H * 0.22f}, kWonk * 0.7f, kSeg, Hash2(mseed, 11));
    if (!mdis)
      HandShadow(canvas, stem, {kShadowDX * 0.5f, -kShadowDY * 0.5f}, kShadow, Hash2(mseed, 12));
    canvas.save();
    canvas.translate(push, -push);
    SketchyStroke(canvas, stem, kInk, kStrokeBold, Hash2(mseed, 13), 2);
    SkPath nib = SkPathBuilder()
                     .moveTo(mx - H * 0.10f, band.fTop - H * 0.10f)
                     .lineTo(mx + H * 0.10f, band.fTop - H * 0.10f)
                     .lineTo(mx, band.fTop - H * 0.30f)
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
      float cx = std::clamp(mx, band.fLeft + cw * 0.5f, band.fRight - cw * 0.5f);
      float cy = knob_y + knob_r + ch * 0.62f;
      SkRect chip = SkRect::MakeXYWH(cx - cw / 2, cy - ch / 2, cw, ch);
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

SkRect ConnectivityCell(const SkRect& r, int which) {
  float w = r.width() / 2.f;
  return SkRect::MakeXYWH(r.fLeft + which * w + (which ? 0.45_mm : 0.f), r.fTop, w - 0.45_mm,
                          r.height());
}
int ConnectivityHit(const SkRect& r, SkPoint p) {
  if (p.fX < r.fLeft || p.fX > r.fRight || p.fY < r.fTop || p.fY > r.fBottom) return 0;
  return p.fX < r.centerX() ? 4 : 8;
}
void DrawConnectivity(SkCanvas& canvas, const SkRect& r, bool eight, beta::State state,
                      uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  for (int which = 0; which < 2; ++which) {
    SkRect cell = ConnectivityCell(r, which);
    bool sel = ((which == 1) == eight) && !disabled;
    SkPath cp = WonkyRoundRect(cell, cell.height() * 0.18f, kWonk * 0.6f, Hash2(seed, which));
    if (sel) HandShadow(canvas, cp, {0.29_mm, -0.29_mm}, kShadow, Hash2(seed, 10u + which));
    MisregFill(canvas, cp, sel ? kPaper : kGray, Hash2(seed, 20u + which));
    SketchyStroke(canvas, cp, kInk, sel ? kStroke : kStrokeHair, Hash2(seed, 30u + which),
                  sel ? 2 : 1);
    SkPoint c{cell.centerX(), cell.centerY()};
    float L = cell.height() * 0.30f;
    SkColor ink = sel ? kInk : kInkSoft;
    int dirs = which == 0 ? 4 : 8;
    for (int d = 0; d < dirs; ++d) {
      float ang = (float)d * (6.2831853f / dirs);
      SkPoint e{c.fX + L * std::cos(ang), c.fY - L * std::sin(ang)};
      canvas.drawPath(WobbleLine(c, e, 0.15_mm, 0.73_mm, Hash2(seed, 40u + which * 8u + d)),
                      InkPaint(ink, sel ? kStrokeBold : kStroke));
    }
    SkPath body = WobbleEllipse(c, cell.height() * 0.09f, cell.height() * 0.09f, kWonk * 0.5f,
                                Hash2(seed, 60u + which), 24);
    FillPath(canvas, body, ink);
    const char* num = which == 0 ? "4" : "8";
    DrawText(canvas, num, {cell.fLeft + 0.6_mm, cell.fTop + 0.6_mm}, cell.height() * 0.34f, ink,
             false, 0);
    if (sel) Highlight(canvas, cell, kBlue, Hash2(seed, 70));
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, r.height() * 0.2f, Hash2(seed, 80));
}

int RegionHit(const SkRect& mq, SkPoint p, float grip) {
  SkPoint corners[4] = {
      {mq.fLeft, mq.fTop}, {mq.fRight, mq.fTop}, {mq.fLeft, mq.fBottom}, {mq.fRight, mq.fBottom}};
  for (int i = 0; i < 4; ++i)
    if (std::abs(p.fX - corners[i].fX) <= grip && std::abs(p.fY - corners[i].fY) <= grip)
      return 1 + i;
  if (p.fX >= mq.fLeft && p.fX <= mq.fRight && p.fY >= mq.fTop && p.fY <= mq.fBottom) return 5;
  return 0;
}
void DrawRegion(SkCanvas& canvas, const SkRect& fit, const SkRect& mq, beta::State state,
                uint32_t seed) {
  using namespace beta;
  const bool active = state == State::Pressed || state == State::Active;
  {
    SkPaint dim;
    dim.setColor(0x55101010);
    canvas.drawRect(SkRect::MakeLTRB(fit.fLeft, fit.fTop, fit.fRight, mq.fTop), dim);
    canvas.drawRect(SkRect::MakeLTRB(fit.fLeft, mq.fBottom, fit.fRight, fit.fBottom), dim);
    canvas.drawRect(SkRect::MakeLTRB(fit.fLeft, mq.fTop, mq.fLeft, mq.fBottom), dim);
    canvas.drawRect(SkRect::MakeLTRB(mq.fRight, mq.fTop, fit.fRight, mq.fBottom), dim);
  }
  {
    const float dash = 1.3_mm, gap = 0.9_mm;
    SkColor ink = active ? kRed : kInk;
    auto edge = [&](SkPoint a, SkPoint b, uint32_t eseed) {
      float len = std::hypot(b.fX - a.fX, b.fY - a.fY);
      int n = std::max(1, (int)(len / (dash + gap)));
      for (int i = 0; i < n; ++i) {
        float t0 = (i * (dash + gap)) / len, t1 = std::min(1.f, t0 + dash / len);
        SkPoint p0{a.fX + (b.fX - a.fX) * t0, a.fY + (b.fY - a.fY) * t0};
        SkPoint p1{a.fX + (b.fX - a.fX) * t1, a.fY + (b.fY - a.fY) * t1};
        canvas.drawPath(WobbleLine(p0, p1, 0.15_mm, 0.9_mm, Hash2(eseed, (uint32_t)i)),
                        InkPaint(ink, kStroke));
      }
    };
    edge({mq.fLeft, mq.fTop}, {mq.fRight, mq.fTop}, Hash2(seed, 1));
    edge({mq.fRight, mq.fTop}, {mq.fRight, mq.fBottom}, Hash2(seed, 2));
    edge({mq.fRight, mq.fBottom}, {mq.fLeft, mq.fBottom}, Hash2(seed, 3));
    edge({mq.fLeft, mq.fBottom}, {mq.fLeft, mq.fTop}, Hash2(seed, 4));
  }
  SkPoint corners[4] = {
      {mq.fLeft, mq.fTop}, {mq.fRight, mq.fTop}, {mq.fLeft, mq.fBottom}, {mq.fRight, mq.fBottom}};
  for (int i = 0; i < 4; ++i) {
    SkRect g = SkRect::MakeXYWH(corners[i].fX - 0.9_mm, corners[i].fY - 0.9_mm, 1.75_mm, 1.75_mm);
    SkPath gp = WonkyRoundRect(g, 0.36_mm, kWonk * 0.5f, Hash2(seed, 10u + i));
    MisregFill(canvas, gp, active ? kRed : kYellow, Hash2(seed, 20u + i));
    SketchyStroke(canvas, gp, kInk, kStrokeHair, Hash2(seed, 30u + i), 1);
  }
}

bool TransformRingHit(SkPoint c, float radius, SkPoint p, float band) {
  float d = std::hypot(p.fX - c.fX, p.fY - c.fY);
  return std::abs(d - radius) <= band;
}
float TransformRingAngleAt(SkPoint c, SkPoint p) {
  return std::atan2(p.fX - c.fX, p.fY - c.fY) * 57.29578f;
}
void DrawTransformRing(SkCanvas& canvas, SkPoint c, float radius, float angle_deg,
                       beta::State state, uint32_t seed) {
  using namespace beta;
  const bool active = state == State::Pressed || state == State::Active;
  const int kSegs = 36;
  for (int i = 0; i < kSegs; i += 2) {
    float a0 = (float)i / kSegs * 6.2831853f, a1 = (float)(i + 1) / kSegs * 6.2831853f;
    SkPoint p0{c.fX + radius * std::sin(a0), c.fY + radius * std::cos(a0)};
    SkPoint p1{c.fX + radius * std::sin(a1), c.fY + radius * std::cos(a1)};
    canvas.drawPath(WobbleLine(p0, p1, 0.15_mm, 1.0_mm, Hash2(seed, (uint32_t)i)),
                    InkPaint(kInkSoft, kStrokeHair));
  }
  for (int q = 0; q < 4; ++q) {
    float a = q * 1.5707963f;
    SkPoint t0{c.fX + (radius - 0.73_mm) * std::sin(a), c.fY + (radius - 0.73_mm) * std::cos(a)};
    SkPoint t1{c.fX + (radius + 0.73_mm) * std::sin(a), c.fY + (radius + 0.73_mm) * std::cos(a)};
    canvas.drawPath(WobbleLine(t0, t1, 0.12_mm, 0.73_mm, Hash2(seed, 50u + q)),
                    InkPaint(kInk, kStrokeHair));
  }
  float rad = angle_deg * 0.017453293f;
  SkPoint k{c.fX + radius * std::sin(rad), c.fY + radius * std::cos(rad)};
  canvas.drawPath(WobbleLine(c, k, 0.15_mm, 1.3_mm, Hash2(seed, 60)),
                  InkPaint(kInkSoft, kStrokeHair));
  SkPath knob = WobbleEllipse(k, 1.1_mm, 1.0_mm, kWonk * 0.6f, Hash2(seed, 61), 28);
  MisregFill(canvas, knob, active ? kRed : kYellow, Hash2(seed, 62));
  SketchyStroke(canvas, knob, kInk, kStrokeBold, Hash2(seed, 63), 2);
}

bool PolarityHit(const SkRect& r, SkPoint p) {
  return p.fX >= r.fLeft - 0.6_mm && p.fX <= r.fRight + 0.6_mm && p.fY >= r.fTop - 0.6_mm &&
         p.fY <= r.fBottom + 0.6_mm;
}
void DrawPolarity(SkCanvas& canvas, const SkRect& r, bool bright_hot, beta::State state,
                  uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  float rad = r.height() / 2.f;
  SkPoint dark{r.fLeft + rad, r.centerY()}, bright{r.fRight - rad, r.centerY()};
  canvas.drawPath(
      WobbleLine({dark.fX + rad * 0.6f, r.centerY()}, {bright.fX - rad * 0.6f, r.centerY()},
                 0.15_mm, 0.9_mm, Hash2(seed, 1)),
      InkPaint(disabled ? kInkSoft : kInk, kStroke));
  SkPath dp = WobbleEllipse(dark, rad * 0.78f, rad * 0.74f, kWonk * 0.5f, Hash2(seed, 2), 30);
  FillPath(canvas, dp, disabled ? kGray : kInk);
  SketchyStroke(canvas, dp, kInk, kStrokeHair, Hash2(seed, 3), 1);
  SkPath bp = WobbleEllipse(bright, rad * 0.78f, rad * 0.74f, kWonk * 0.5f, Hash2(seed, 4), 30);
  MisregFill(canvas, bp, disabled ? kGray : kPaper, Hash2(seed, 5));
  SketchyStroke(canvas, bp, kInk, kStrokeHair, Hash2(seed, 6), 1);
  if (!disabled) {
    SkPoint hot = bright_hot ? bright : dark;
    SkPath halo = WobbleEllipse(hot, rad * 1.15f, rad * 1.1f, kWonk * 0.6f, Hash2(seed, 7), 34);
    canvas.drawPath(halo, InkPaint(kYellow, kStrokeBold));
    canvas.drawPath(WobblePath(halo, kWonk * 0.4f, kSeg, Hash2(seed, 8)),
                    InkPaint(kInk, kStrokeHair));
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, r.height() * 0.3f, Hash2(seed, 9));
}

SkRect PaletteCell(const SkRect& r, int i) {
  float cw = r.width() / 5.f, ch = r.height() / 2.f;
  int row = i / 5;
  return SkRect::MakeXYWH(r.fLeft + (i % 5) * cw + 0.22_mm, r.fBottom - (row + 1) * ch + 0.22_mm,
                          cw - 0.45_mm, ch - 0.45_mm);
}
int PaletteHit(const SkRect& r, SkPoint p) {
  if (p.fX < r.fLeft || p.fX > r.fRight || p.fY < r.fTop || p.fY > r.fBottom) return -1;
  int col = std::clamp((int)((p.fX - r.fLeft) / (r.width() / 5.f)), 0, 4);
  int row = std::clamp((int)((r.fBottom - p.fY) / (r.height() / 2.f)), 0, 1);
  return row * 5 + col;
}
void DrawPalette(SkCanvas& canvas, const SkRect& r, int selected, beta::State state,
                 uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  for (int i = 0; i < kPaletteCount; ++i) {
    SkRect cell = PaletteCell(r, i);
    SkPath cp = WonkyRoundRect(cell, 0.36_mm, kWonk * 0.4f, Hash2(seed, (uint32_t)i));
    MisregFill(canvas, cp, disabled ? kGray : kPaletteColors[i], Hash2(seed, 20u + i));
    SketchyStroke(canvas, cp, kInk, i == selected ? kStroke : kStrokeHair, Hash2(seed, 40u + i),
                  i == selected ? 2 : 1);
    if (i == selected && !disabled) Highlight(canvas, cell, kBlue, Hash2(seed, 60));
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, r.height() * 0.25f, Hash2(seed, 80));
}

void DrawDepthChip(SkCanvas& canvas, const SkRect& r, int depth, bool has_cmap, beta::State state,
                   uint32_t seed) {
  using namespace beta;
  const bool disabled = state == State::Disabled;
  SkPath chip = WonkyRoundRect(r, r.height() * 0.22f, kWonk * 0.5f, Hash2(seed, 1));
  MisregFill(canvas, chip, disabled ? kGray : kPaper, Hash2(seed, 2));
  SketchyStroke(canvas, chip, kInk, kStrokeHair, Hash2(seed, 3), 1);
  SkColor ink = disabled ? kInkSoft : kInk;
  float h = r.height();
  SkRect g = SkRect::MakeXYWH(r.fLeft + h * 0.18f, r.fTop + h * 0.22f, h * 0.56f, h * 0.56f);
  if (depth == 1) {
    float c = g.width() / 2;
    SkPaint pb;
    pb.setAntiAlias(true);
    pb.setColor(ink);
    canvas.drawRect(SkRect::MakeXYWH(g.fLeft, g.fBottom - c, c, c), pb);
    canvas.drawRect(SkRect::MakeXYWH(g.fLeft + c, g.fTop, c, c), pb);
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
      canvas.drawRect(
          SkRect::MakeXYWH(g.fLeft + i * g.width() / 4, g.fTop, g.width() / 4, g.height()), ps);
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
      SkRect cell = SkRect::MakeXYWH(g.fLeft, g.fBottom - (i + 1) * g.height() / 3, g.width(),
                                     g.height() / 3 - 0.15_mm);
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
    float dr = g.width() * 0.30f;
    SkPoint centers[3] = {{g.fLeft + dr, g.fBottom - dr},
                          {g.fRight - dr, g.fBottom - dr * 1.4f},
                          {g.centerX(), g.fTop + dr}};
    for (int i = 0; i < 3; ++i) {
      SkPaint pd;
      pd.setAntiAlias(true);
      pd.setColor(disabled ? kGray : cs[i]);
      canvas.drawCircle(centers[i].fX, centers[i].fY, dr, pd);
    }
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", depth);
  float fs = h * 0.52f;
  DrawText(canvas, buf, {g.fRight + h * 0.18f, r.fTop + h * 0.26f}, fs, ink, false, 0);
}

void StampGridMetrics(const SkRect& rect, int w, int h, float& cell, float& gx, float& gy) {
  cell = std::min(rect.width() / std::max(1, w), rect.height() / std::max(1, h));
  gx = rect.fLeft + (rect.width() - cell * w) / 2;
  gy = rect.fTop + (rect.height() - cell * h) / 2;
}

bool StampCellAt(const SkRect& rect, SkPoint p, int w, int h, int& cx, int& cy) {
  float cell, gx, gy;
  StampGridMetrics(rect, w, h, cell, gx, gy);
  if (p.fX < gx || p.fX > gx + cell * w || p.fY < gy || p.fY > gy + cell * h) return false;
  cx = std::clamp((int)((p.fX - gx) / cell), 0, w - 1);
  cy = std::clamp((int)((gy + cell * h - p.fY) / cell), 0, h - 1);
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

void DrawStamp(SkCanvas& canvas, const SkRect& rect, const uint8_t* cells, int w, int h, int ox,
               int oy, beta::State state, uint32_t seed) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  float cell, gx, gy;
  StampGridMetrics(rect, w, h, cell, gx, gy);

  for (int j = 0; j < h; ++j) {
    for (int i = 0; i < w; ++i) {
      int v = cells ? cells[j * w + i] : 0;
      uint32_t cs = Hash3(seed, (uint32_t)i, (uint32_t)j);
      SkRect c =
          SkRect::MakeXYWH(gx + i * cell + cell * 0.09f, gy + (h - 1 - j) * cell + cell * 0.09f,
                           cell * 0.82f, cell * 0.82f);
      SkPath cp = WonkyRoundRect(c, cell * 0.16f, kWonk * 0.5f, cs);
      if (v == 1) {
        MisregFill(canvas, cp, disabled ? kGray : kCyan, cs);
        SketchyStroke(canvas, cp, kInk, kStroke, cs, 1);
      } else if (v == 2) {
        SketchyStroke(canvas, cp, disabled ? kGray : kInkSoft, kStrokeHair, cs, 1);
        float in = cell * 0.2f;
        canvas.drawPath(WobbleLine({c.fLeft + in, c.fTop + in}, {c.fRight - in, c.fBottom - in},
                                   kWonk * 0.4f, kSeg, Hash2(cs, 7)),
                        InkPaint(disabled ? kGray : kRed, kStroke));
        canvas.drawPath(WobbleLine({c.fRight - in, c.fTop + in}, {c.fLeft + in, c.fBottom - in},
                                   kWonk * 0.4f, kSeg, Hash2(cs, 8)),
                        InkPaint(disabled ? kGray : kRed, kStroke));
      } else {
        SketchyStroke(canvas, cp, disabled ? kGray : kInkSoft, kStrokeHair, cs, 1);
      }
    }
  }
  if (ox >= 0 && oy >= 0 && ox < w && oy < h) {
    SkPoint oc = {gx + (ox + 0.5f) * cell, gy + (h - 1 - oy + 0.5f) * cell};
    SkPath ring = WobbleEllipse(oc, cell * 0.20f, cell * 0.20f, kWonk * 0.5f, Hash2(seed, 41), 22);
    SketchyStroke(canvas, ring, disabled ? kGrayDark : kRed, kStrokeBold, Hash2(seed, 42), 2);
  }
}

float ModeWheelAngle(int k, int n) { return 1.57079633f - k * (6.28318531f / n); }

int ModeWheelHit(SkPoint c, float radius, SkPoint p, int n) {
  float dx = p.fX - c.fX, dy = p.fY - c.fY;
  if (std::hypot(dx, dy) > radius * 1.7f) return -1;
  float ang = std::atan2(dy, dx);
  int best = 0;
  float bestd = 1e9f;
  for (int k = 0; k < n; ++k) {
    float d = std::abs(std::remainder(ang - ModeWheelAngle(k, n), 6.28318531f));
    if (d < bestd) {
      bestd = d;
      best = k;
    }
  }
  return best;
}

void DrawModeWheel(SkCanvas& canvas, SkPoint c, float radius, const char* const* labels, int n,
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
    SkPoint gc = {c.fX - std::cos(ga) * radius * 0.42f, c.fY - std::sin(ga) * radius * 0.42f};
    SkMatrix m = SkMatrix::Translate(gc.fX, gc.fY);
    m.preScale(radius * 0.34f, radius * 0.34f);
    SkPaint ink;
    ink.setAntiAlias(true);
    ink.setColor(disabled ? kGrayDark : kInk);
    canvas.drawPath(glyphs[selected].makeTransform(m), ink);
  }

  float fs = radius * 0.42f;
  for (int k = 0; k < n; ++k) {
    float a = ModeWheelAngle(k, n);
    SkPoint dir = {std::cos(a), std::sin(a)};
    bool sel = k == selected;
    canvas.drawPath(WobbleLine({c.fX + dir.fX * radius * 0.62f, c.fY + dir.fY * radius * 0.62f},
                               {c.fX + dir.fX * radius * 0.95f, c.fY + dir.fY * radius * 0.95f},
                               kWonk * 0.5f, kSeg, Hash2(seed, k + 1)),
                    InkPaint(sel ? kRed : kInkSoft, sel ? kStrokeBold : kStroke));
    SkPoint lp = {c.fX + dir.fX * (radius + fs * 1.5f), c.fY + dir.fY * (radius + fs * 0.9f)};
    float tw = TextWidth(labels[k], fs);
    // Labels near the bottom pole slide outward so they don't collide under the dial.
    if (std::abs(dir.fY) > 0.5f && std::abs(dir.fX) > 0.15f)
      lp.fX += (dir.fX > 0 ? 1.f : -1.f) * tw * 0.35f;
    DrawText(canvas, labels[k], {lp.fX - tw / 2, lp.fY - fs * 0.34f}, fs,
             disabled ? kGray : (sel ? kInk : kInkSoft), false, 0);
  }
  float sa = ModeWheelAngle(selected, n);
  SkPoint sd = {std::cos(sa), std::sin(sa)};
  canvas.drawPath(WobbleLine(c, {c.fX + sd.fX * radius * 0.7f, c.fY + sd.fY * radius * 0.7f}, kWonk,
                             kSeg, Hash2(seed, 91)),
                  InkPaint(disabled ? kGrayDark : kRed, kStrokeBold));
  SkPath hub = WobbleEllipse(c, radius * 0.15f, radius * 0.15f, kWonk * 0.5f, Hash2(seed, 92), 16);
  MisregFill(canvas, hub, disabled ? kGrayDark : kYellow, Hash2(seed, 93));
  SketchyStroke(canvas, hub, kInk, kStroke, Hash2(seed, 94), 1);
}

void DrawCurveLUT(SkCanvas& canvas, const SkRect& rect, const uint8_t* lut, beta::State state,
                  uint32_t seed) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  float w = rect.width(), h = rect.height();
  SkPaint paper;
  paper.setAntiAlias(true);
  paper.setColor(disabled ? kGray : kPaperCream);
  canvas.drawRect(rect, paper);
  SkPaint grid = InkPaint(0x28000000, kStrokeHair);
  for (int i = 1; i < 4; ++i) {
    float gx = rect.fLeft + w * i / 4, gy = rect.fTop + h * i / 4;
    canvas.drawLine(gx, rect.fTop, gx, rect.fBottom, grid);
    canvas.drawLine(rect.fLeft, gy, rect.fRight, gy, grid);
  }
  canvas.drawLine(rect.fLeft, rect.fTop, rect.fRight, rect.fBottom,
                  InkPaint(disabled ? kGrayDark : kInkSoft, kStrokeHair));
  if (lut) {
    const int N = std::clamp((int)(w / 0.6_mm), 24, 128);
    SkPathBuilder cb;
    for (int k = 0; k <= N; ++k) {
      int i = std::clamp(k * 255 / N, 0, 255);
      float x = rect.fLeft + w * i / 255.f;
      float y = rect.fTop + h * lut[i] / 255.f;
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

float DialAngleAt(SkPoint c, SkPoint p) {
  float a = std::atan2(p.fX - c.fX, p.fY - c.fY) * 57.29578f;
  if (a < 0) a += 360.f;
  return a;
}
void DrawDial(SkCanvas& canvas, SkPoint c, float radius, float angle_deg, beta::State state,
              uint32_t seed) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  for (int d = 0; d < 360; d += 30) {
    float rad = d * 0.0174533f;
    SkPoint dir = {std::sin(rad), std::cos(rad)};
    bool major = (d % 90 == 0);
    float t0 = major ? 0.80f : 0.90f;
    canvas.drawPath(WobbleLine({c.fX + dir.fX * radius * t0, c.fY + dir.fY * radius * t0},
                               {c.fX + dir.fX * radius * 0.99f, c.fY + dir.fY * radius * 0.99f},
                               kWonk * 0.4f, kSeg, Hash2(seed, (uint32_t)d)),
                    InkPaint(disabled ? kGray : kInkSoft, major ? kStroke : kStrokeHair));
  }
  float arad = angle_deg * 0.0174533f;
  SkPoint nd = {std::sin(arad), std::cos(arad)};
  SkPath needle = WobbleLine(c, {c.fX + nd.fX * radius * 0.84f, c.fY + nd.fY * radius * 0.84f},
                             kWonk, kSeg, Hash2(seed, 0x91));
  if (!disabled) SketchyStroke(canvas, needle, kRed, kStrokeBold, Hash2(seed, 0x92), 2);
  SkPath hub =
      WobbleEllipse(c, radius * 0.10f, radius * 0.10f, kWonk * 0.5f, Hash2(seed, 0x93), 14);
  MisregFill(canvas, hub, disabled ? kGrayDark : kYellow, Hash2(seed, 0x94));
  SketchyStroke(canvas, hub, kInk, kStroke, Hash2(seed, 0x95), 1);
}

int ChannelTapAt(const SkRect& r, SkPoint p, int n) {
  if (p.fY < r.fTop || p.fY > r.fBottom || p.fX < r.fLeft || p.fX > r.fRight) return -1;
  return std::clamp((int)((p.fX - r.fLeft) / (r.width() / n)), 0, n - 1);
}
void DrawChannelTap(SkCanvas& canvas, const SkRect& r, int selected, beta::State state,
                    uint32_t seed, const char* const* labels, const SkColor* colors, int n) {
  using namespace beta;
  bool disabled = state == State::Disabled;
  static constexpr SkColor kDefCols[4] = {0xff8a8a8a, kRed, kGreen, kBlue};
  static const char* const kDefLabs[4] = {"LUM", "R", "G", "B"};
  const SkColor* cols = colors ? colors : kDefCols;
  const char* const* labs = labels ? labels : kDefLabs;
  float cw = r.width() / n;
  for (int i = 0; i < n; ++i) {
    SkRect c = SkRect::MakeXYWH(r.fLeft + i * cw + cw * 0.08f, r.fTop, cw * 0.84f, r.height());
    bool sel = (i == selected);
    SkColor fill = disabled ? kGray : cols[i];
    SkPath cp = WonkyRoundRect(c, c.height() * 0.22f, kWonk * 0.6f, Hash2(seed, (uint32_t)i));
    MisregFill(canvas, cp, fill, Hash2(seed, (uint32_t)(i + 10)));
    if (!sel) {
      SkPaint wash;
      wash.setColor(0xb0f4efe4);
      canvas.drawPath(cp, wash);
    }
    SketchyStroke(canvas, cp, kInk, sel ? kStrokeBold : kStroke, Hash2(seed, (uint32_t)(i + 20)),
                  sel ? 2 : 1);
    DrawTextIn(canvas, labs[i], c, c.height() * 0.42f, sel ? TextOn(fill) : kInkSoft,
               TextAlign::Center, false, 0);
    if (sel && !disabled)
      canvas.drawPath(WobblePath(cp, kWonk, kSeg, Hash2(seed, (uint32_t)(i + 30))),
                      InkPaint(kYellow, kStroke));
  }
}

}  // namespace automat::ui::leptonica
