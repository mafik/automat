// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "ui_beta.hpp"

#include <include/core/SkData.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkTypeface.h>
#include <src/base/SkUTF.h>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

#include "../build/generated/embedded.hpp"
#include "font.hpp"
#include "math.hpp"

#pragma comment(lib, "skia")

namespace automat::ui::beta {

// ============================================================ color helpers ==
static SkColor WithAlpha(SkColor c, float a01) {
  return SkColorSetARGB((uint8_t)std::lround(std::clamp(a01, 0.f, 1.f) * 255.f), SkColorGetR(c),
                        SkColorGetG(c), SkColorGetB(c));
}
static SkColor MixColor(SkColor a, SkColor b, float t) {
  float it = 1.f - t;
  return SkColorSetARGB((uint8_t)std::lround(SkColorGetA(a) * it + SkColorGetA(b) * t),
                        (uint8_t)std::lround(SkColorGetR(a) * it + SkColorGetR(b) * t),
                        (uint8_t)std::lround(SkColorGetG(a) * it + SkColorGetG(b) * t),
                        (uint8_t)std::lround(SkColorGetB(a) * it + SkColorGetB(b) * t));
}
static float Luma(SkColor c) {
  return (0.299f * SkColorGetR(c) + 0.587f * SkColorGetG(c) + 0.114f * SkColorGetB(c)) / 255.f;
}
// Stylistic rather than pure contrast: white only on the deep saturated colors.
SkColor TextOn(SkColor bg) {
  if (bg == kRed || bg == kBlue || bg == kGreen || bg == kPurple || bg == kInk || bg == kInkPure ||
      bg == kInkSoft || bg == kRose)
    return kPaper;
  if (bg == kYellow || bg == kCyan || bg == kOrange || bg == kLime || bg == kSky || bg == kGold ||
      bg == kGray || bg == kPaper || bg == kPaperCream)
    return kInk;
  return Luma(bg) < 0.5f ? kPaper : kInk;
}

// ================================================================== fonts ====
// Kindergarten lacks several glyphs (% * > _ ...); NotoSans backstops them,
// selected per codepoint through the fallback chain.
static const std::vector<sk_sp<SkTypeface>>& FontChain() {
  static std::vector<sk_sp<SkTypeface>> chain = [] {
    std::vector<sk_sp<SkTypeface>> v;
    if (auto t = ui::Font::LoadTypeface(embedded::assets_Kindergarten_ttf)) {
      v.push_back(std::move(t));
    }
    if (auto t = ui::Font::LoadTypeface(embedded::assets_NotoSans_wght__ttf)) {
      v.push_back(std::move(t));
    }
    return v;
  }();
  return chain;
}
sk_sp<SkTypeface> BrandTypeface() {
  const auto& chain = FontChain();
  return chain.empty() ? nullptr : chain.front();
}
static sk_sp<SkTypeface> FontForCodepoint(SkUnichar cp) {
  for (const auto& t : FontChain())
    if (t && t->unicharToGlyph(cp) != 0) return t;
  return BrandTypeface();
}
static SkFont MakeFontTf(sk_sp<SkTypeface> tf, float size) {
  SkFont f(std::move(tf), size);
  f.setEdging(SkFont::Edging::kAntiAlias);
  f.setSubpixel(true);
  return f;
}
static SkFont MakeFont(float size) { return MakeFontTf(BrandTypeface(), size); }

// =============================================================== primitives ==
SkPaint InkPaint(SkColor color, float width, bool antialias) {
  SkPaint p;
  p.setAntiAlias(antialias);
  p.setStyle(SkPaint::kStroke_Style);
  p.setStrokeWidth(width);
  p.setStrokeJoin(SkPaint::kRound_Join);
  p.setStrokeCap(SkPaint::kRound_Cap);
  p.setColor(color);
  return p;
}
static SkPaint Filler(SkColor color, bool antialias = true) {
  SkPaint p;
  p.setAntiAlias(antialias);
  p.setStyle(SkPaint::kFill_Style);
  p.setColor(color);
  return p;
}

// Emits points [0, n-1]; the caller appends b. Deviation tapers to zero at the
// ends so adjacent edges meet cleanly.
static void WobbleSegment(SkPoint a, SkPoint b, float amp, float seg, uint32_t seed, float t_offset,
                          std::vector<SkPoint>* out) {
  SkVector d = b - a;
  float len = d.length();
  if (len < 1e-6f) {
    out->push_back(a);
    return;
  }
  SkVector nrm = {-d.fY / len, d.fX / len};
  int n = std::max(1, (int)std::lround(len / seg));
  for (int i = 0; i < n; ++i) {
    float u = (float)i / (float)n;
    SkPoint p = a + d * u;
    float t = t_offset + u;
    float off = ValueNoise1D(t, seed, 1.7f) * amp;
    off += ValueNoise1D(t, seed ^ 0xABCDu, 5.3f) * (amp * 0.35f);
    float taper = std::sin(u * kPi);
    p += nrm * (off * taper);
    out->push_back(p);
  }
}
static SkPath PolyPath(const std::vector<SkPoint>& pts, bool close) {
  SkPathBuilder pb;
  if (pts.empty()) return pb.detach();
  pb.moveTo(pts[0]);
  for (size_t i = 1; i < pts.size(); ++i) pb.lineTo(pts[i]);
  if (close) pb.close();
  return pb.detach();
}

SkPath WobbleLine(Vec2 a, Vec2 b, float amp, float seg, uint32_t seed) {
  std::vector<SkPoint> pts;
  WobbleSegment(a, b, amp, seg, seed, 0.f, &pts);
  pts.push_back(b);
  return PolyPath(pts, false);
}

SkPath WobbleRect(const Rect& r, float amp, float seg, uint32_t seed) {
  SkPoint c[4] = {r.BottomLeftCorner(), r.BottomRightCorner(), r.TopRightCorner(),
                  r.TopLeftCorner()};
  SkPoint cen = r.Center();
  for (int i = 0; i < 4; ++i) {
    SkVector out = c[i] - cen;
    float l = out.length();
    if (l > 1e-6f) out *= (1.f / l);
    c[i] += out * (amp * 0.6f * U01(Hash2(seed, 100u + i)));
  }
  std::vector<SkPoint> pts;
  for (int e = 0; e < 4; ++e) WobbleSegment(c[e], c[(e + 1) & 3], amp, seg, seed, (float)e, &pts);
  return PolyPath(pts, true);
}

SkPath WobbleEllipse(Vec2 center, float rx, float ry, float amp, uint32_t seed, int samples) {
  SkPathBuilder pb;
  float ph1 = U01(Hash2(seed, 1u)) * kPi * 2;
  float ph2 = U01(Hash2(seed, 2u)) * kPi * 2;
  float ph3 = U01(Hash2(seed, 3u)) * kPi * 2;
  float denom = std::max(rx, ry);
  for (int i = 0; i <= samples; ++i) {
    float a = (float)i / samples * kPi * 2;
    float wob =
        std::sin(a * 3.f + ph1) + 0.5f * std::sin(a * 5.f + ph2) + 0.3f * std::sin(a * 2.f + ph3);
    float rscale = 1.0f + (wob / 1.8f) * (amp / denom);
    Vec2 p =
        Vec2(center) + Vec2::Polar(SinCos::FromRadians(a), 1.f) * Vec2(rx * rscale, ry * rscale);
    if (i == 0)
      pb.moveTo(p);
    else
      pb.lineTo(p);
  }
  pb.close();
  return pb.detach();
}

SkPath WobblePath(const SkPath& src, float amp, float seg, uint32_t seed) {
  SkPathBuilder out;
  SkPathMeasure meas(src, false);
  uint32_t contour = 0;
  do {
    float length = meas.getLength();
    if (length < 1e-6f) {
      ++contour;
      continue;
    }
    int n = std::max(2, (int)std::lround(length / seg));
    bool closed = meas.isClosed();
    bool first = true;
    for (int i = 0; i <= n; ++i) {
      float dist = length * (float)i / (float)n;
      SkPoint pos;
      SkVector tan;
      if (!meas.getPosTan(dist, &pos, &tan)) continue;
      SkVector nrm = {-tan.fY, tan.fX};
      float t = (float)i / (float)n;
      float off = ValueNoise1D(t, seed ^ (contour * 131u), 1.7f) * amp;
      float taper = closed ? 1.0f : std::sin(t * kPi);
      pos += nrm * (off * taper);
      if (first) {
        out.moveTo(pos);
        first = false;
      } else
        out.lineTo(pos);
    }
    if (closed) out.close();
    ++contour;
  } while (meas.nextContour());
  return out.detach();
}

SkPath WonkyRoundRect(const Rect& r, float baseRadius, float wobAmp, uint32_t seed) {
  float maxR = 0.45f * std::min(r.Width(), r.Height());
  float rad[4];
  for (int i = 0; i < 4; ++i)
    rad[i] = std::min(maxR, baseRadius * (0.5f + 1.0f * U01(Hash2(seed, 50u + i))));
  const float K = 0.5522847498f;
  SkPoint TL{r.left, r.bottom}, TR{r.right, r.bottom}, BR{r.right, r.top}, BL{r.left, r.top};
  auto edgeBow = [&](SkPoint a, SkPoint b, uint32_t k) -> SkPoint {
    SkPoint mid = (a + b) * 0.5f;
    SkVector d = b - a;
    float l = d.length();
    if (l > 1e-6f) d *= (1.f / l);
    SkVector nrm = {-d.fY, d.fX};
    float bow = U11(Hash2(seed, 70u + k)) * wobAmp;
    return mid + nrm * bow;
  };
  SkPathBuilder pb;
  SkPoint pTopStart{TL.fX + rad[0], TL.fY};
  pb.moveTo(pTopStart);
  SkPoint topEnd{TR.fX - rad[1], TR.fY};
  pb.quadTo(edgeBow(pTopStart, topEnd, 0), topEnd);
  pb.cubicTo(TR.fX - rad[1] * (1 - K), TR.fY, TR.fX, TR.fY + rad[1] * (1 - K), TR.fX,
             TR.fY + rad[1]);
  SkPoint rStart{TR.fX, TR.fY + rad[1]}, rEnd{BR.fX, BR.fY - rad[2]};
  pb.quadTo(edgeBow(rStart, rEnd, 1), rEnd);
  pb.cubicTo(BR.fX, BR.fY - rad[2] * (1 - K), BR.fX - rad[2] * (1 - K), BR.fY, BR.fX - rad[2],
             BR.fY);
  SkPoint bStart{BR.fX - rad[2], BR.fY}, bEnd{BL.fX + rad[3], BL.fY};
  pb.quadTo(edgeBow(bStart, bEnd, 2), bEnd);
  pb.cubicTo(BL.fX + rad[3] * (1 - K), BL.fY, BL.fX, BL.fY - rad[3] * (1 - K), BL.fX,
             BL.fY - rad[3]);
  SkPoint lStart{BL.fX, BL.fY - rad[3]};
  pb.quadTo(edgeBow(lStart, pTopStart, 3), pTopStart);
  pb.close();
  return pb.detach();
}

void HandShadow(SkCanvas& canvas, const SkPath& shape, Vec2 offset, SkColor shadow, uint32_t seed) {
  SkPath s = WobblePath(shape, 0.38_mm, 2.0_mm, Hash2(seed, 0x5AD0u));
  s = s.makeTransform(SkMatrix::Translate(offset.x, offset.y));
  canvas.drawPath(s, Filler(shadow));
}

void FillPath(SkCanvas& canvas, const SkPath& path, SkColor color) {
  canvas.drawPath(path, Filler(color));
}

void MisregFill(SkCanvas& canvas, const SkPath& outline, SkColor fill, uint32_t seed) {
  Rect b{outline.getBounds()};
  SkPoint cen = b.Center();
  float ox = U11(Hash2(seed, 11u)) * (kStroke * 0.95f);
  float oy = -(0.4f + 0.6f * U01(Hash2(seed, 15u))) * (kStroke * 0.95f);
  float sx = 1.0f + U11(Hash2(seed, 13u)) * 0.018f;
  float sy = 1.0f + U11(Hash2(seed, 14u)) * 0.018f;
  SkMatrix m = SkMatrix::Translate(ox, oy);
  m.preTranslate(cen.fX, cen.fY);
  m.preScale(sx, sy);
  m.preTranslate(-cen.fX, -cen.fY);
  SkPath fillPath = WobblePath(outline, kStroke * 1.0f, 1.75_mm, Hash2(seed, 0xF111u));
  fillPath = fillPath.makeTransform(m);
  canvas.drawPath(fillPath, Filler(fill));
}

void SketchyStroke(SkCanvas& canvas, const SkPath& outline, SkColor color, float width,
                   uint32_t seed, int passes) {
  for (int k = 0; k < passes; ++k) {
    uint32_t s = Hash2(seed, 9000u + k);
    float dx = U11(Hash2(s, 1u)) * (width * 0.3f);
    float dy = -U11(Hash2(s, 2u)) * (width * 0.3f);
    SkPaint p = InkPaint(color, width * (k == 0 ? 1.0f : 0.8f));
    p.setAlphaf(k == 0 ? 1.0f : 0.4f);
    SkPath path = WobblePath(outline, width * 0.35f, 1.75_mm, s);
    canvas.save();
    canvas.translate(dx, dy);
    canvas.drawPath(path, p);
    canvas.restore();
  }
}

void ScribbleFill(SkCanvas& canvas, const SkPath& shape, SkColor color, float strokeW,
                  float spacing, uint32_t seed) {
  Rect b{shape.getBounds()};
  canvas.save();
  canvas.clipPath(shape, true);
  SkPaint p = InkPaint(color, strokeW);
  Rng rng(Hash2(seed, 0xC2A1u));
  float angle = 0.5f + rng.f11() * 0.15f;
  float msa = std::sin(angle), mca = std::cos(angle);
  float diag = b.Width() + b.Height();
  for (float d = -diag; d < diag; d += spacing) {
    SkPoint mid{b.CenterX() - msa * d, b.CenterY() + mca * d};
    float aj = angle + rng.f11() * 0.10f;
    float ca = std::cos(aj), sa = std::sin(aj);
    SkPoint a = {mid.fX - ca * diag, mid.fY - sa * diag};
    SkPoint z = {mid.fX + ca * diag, mid.fY + sa * diag};
    SkPath ln = WobbleLine(a, z, strokeW * 0.9f, 2.0_mm,
                           Hash2(seed, (uint32_t)std::lround(d * 7.f / kSeedGrid)));
    p.setStrokeWidth(strokeW * (0.7f + rng.f01() * 0.8f));
    p.setAlphaf(0.45f + 0.35f * rng.f01());
    canvas.drawPath(ln, p);
  }
  canvas.restore();
}

void SprayDisc(SkCanvas& canvas, Vec2 center, float radius, int count, SkColor color, float dotR,
               uint32_t seed) {
  Rng rng(Hash2(seed, 0x59A1u));
  SkPaint p = Filler(color);
  for (int i = 0; i < count; ++i) {
    float rr = radius * std::sqrt(rng.f01()) * (0.4f + 0.6f * rng.f01());
    float a = rng.f01() * kPi * 2;
    Vec2 dot = Vec2(center) + Vec2::Polar(SinCos::FromRadians(a), rr);
    canvas.drawCircle(dot.x, dot.y, dotR, p);
  }
}

void HatchRect(SkCanvas& canvas, const Rect& r, SkColor color, float spacing, uint32_t seed) {
  canvas.save();
  canvas.clipRect(r.sk, true);
  SkPaint p = InkPaint(color, kStrokeHair);
  p.setAlphaf(0.5f);
  for (float x = r.left - r.Height(); x < r.right; x += spacing) {
    SkPath ln = WobbleLine({x, r.top}, {x + r.Height(), r.bottom}, 0.15_mm, 2.0_mm,
                           Hash2(seed, (uint32_t)std::lround(x / kSeedGrid)));
    canvas.drawPath(ln, p);
  }
  canvas.restore();
}

// ================================================================ lettering ==
// Runs of consecutive codepoints that share a fallback font.
template <class Fn>
static void ForEachRun(std::string_view text, Fn&& fn) {
  const char* p = text.data();
  const char* end = p + text.size();
  while (p < end) {
    const char* run_start = p;
    SkUnichar cp = SkUTF::NextUTF8(&p, end);
    if (cp < 0) break;
    sk_sp<SkTypeface> tf = FontForCodepoint(cp);
    for (;;) {
      const char* save = p;
      if (p >= end) break;
      SkUnichar c2 = SkUTF::NextUTF8(&p, end);
      if (c2 < 0 || FontForCodepoint(c2) != tf) {
        p = save;
        break;
      }
    }
    fn(run_start, (size_t)(p - run_start), tf);
  }
}

// Skia rasterizes glyph masks at the SkFont size; a metric size (~0.003 m) gives
// degenerate masks and the glyphs vanish. Rasterize at a sane em and scale the
// canvas down to the requested metric size (as automat::ui::Font::DrawText does).
static constexpr float kRasterEm = 64.f;

float TextWidth(std::string_view text, float size) {
  float w = 0.f;
  ForEachRun(text, [&](const char* s, size_t len, sk_sp<SkTypeface> tf) {
    w += MakeFontTf(std::move(tf), kRasterEm).measureText(s, len, SkTextEncoding::kUTF8);
  });
  return w * (size / kRasterEm);
}

// Lays out `text` at `baseline_left` (metric, +Y up) and paints each glyph with `paint`.
// The canvas is scaled from kRasterEm down to the metric `size` (negative Y so a
// +Y-up baseline reads upright); a stroke width on `paint` is in raster-em units
// like the pen, so it is rescaled here to land at the metric width requested.
static void DrawGlyphs(SkCanvas& canvas, std::string_view text, Vec2 baseline_left, float size,
                       SkPaint paint, bool wonk, uint32_t seed) {
  if (text.empty()) return;
  float s = size / kRasterEm;
  if (paint.getStyle() == SkPaint::kStroke_Style) paint.setStrokeWidth(paint.getStrokeWidth() / s);
  canvas.save();
  canvas.translate(baseline_left.x, baseline_left.y);
  canvas.scale(s, -s);
  if (!wonk) {
    float pen = 0.f;
    ForEachRun(text, [&](const char* str, size_t len, sk_sp<SkTypeface> tf) {
      SkFont f = MakeFontTf(std::move(tf), kRasterEm);
      canvas.drawSimpleText(str, len, SkTextEncoding::kUTF8, pen, 0, f, paint);
      pen += f.measureText(str, len, SkTextEncoding::kUTF8);
    });
    canvas.restore();
    return;
  }
  float pen = 0.f;
  float maxRot = 0.04f;
  float bob = std::min(kRasterEm * 0.045f, 1.8f);  // raster-em units
  const char* p = text.data();
  const char* end = p + text.size();
  uint32_t i = 0;
  while (p < end) {
    const char* cs = p;
    SkUnichar cp = SkUTF::NextUTF8(&p, end);
    if (cp < 0) break;
    size_t len = (size_t)(p - cs);
    SkFont f = MakeFontTf(FontForCodepoint(cp), kRasterEm);
    float adv = f.measureText(cs, len, SkTextEncoding::kUTF8);
    uint32_t h = Hash2(seed, i++);
    float dy = U11(h) * bob;
    float rot = U11(Hash2(h, 7u)) * maxRot;
    float scl = 1.0f + U11(Hash2(h, 9u)) * 0.05f;
    canvas.save();
    canvas.translate(pen + adv * 0.5f, dy);
    canvas.rotate(rot * 180.f / kPi);
    canvas.scale(scl, scl);
    canvas.drawSimpleText(cs, len, SkTextEncoding::kUTF8, -adv * 0.5f, 0, f, paint);
    canvas.restore();
    pen += adv * scl;
  }
  canvas.restore();
}

void DrawText(SkCanvas& canvas, std::string_view text, Vec2 baseline_left, float size,
              SkColor color, bool wonk, uint32_t seed) {
  DrawGlyphs(canvas, text, baseline_left, size, Filler(color), wonk, seed);
}

static Vec2 BaselineIn(std::string_view text, const Rect& b, float size, TextAlign align) {
  SkFont f = MakeFont(kRasterEm);
  SkFontMetrics fm;
  f.getMetrics(&fm);
  float w = TextWidth(text, size);
  float x = b.left;
  if (align == TextAlign::Center)
    x = b.CenterX() - w * 0.5f;
  else if (align == TextAlign::Right)
    x = b.right - w;
  // Metrics are in raster-em units; scale to metric. With +Y up the centering
  // offset keeps the sign of (ascent+descent) once scaled.
  return {x, b.CenterY() + (fm.fAscent + fm.fDescent) * 0.5f * (size / kRasterEm)};
}

void DrawTextIn(SkCanvas& canvas, std::string_view text, const Rect& b, float size, SkColor color,
                TextAlign align, bool wonk, uint32_t seed) {
  DrawGlyphs(canvas, text, BaselineIn(text, b, size, align), size, Filler(color), wonk, seed);
}

static void OutlineTextIn(SkCanvas& canvas, std::string_view text, const Rect& box, float size,
                          SkColor fill, SkColor outline, TextAlign align, bool wonk, uint32_t seed,
                          float outline_w = kStroke) {
  Vec2 baseline = BaselineIn(text, box, size, align);
  DrawGlyphs(canvas, text, baseline, size, InkPaint(outline, outline_w), wonk, seed);
  DrawGlyphs(canvas, text, baseline, size, Filler(fill), wonk, seed);
}

// =================================================================== motifs ==
SkPath StarPath(Vec2 c, float r_outer, float r_inner, uint32_t seed, int points) {
  SkPathBuilder pb;
  float base = -kPi / 2 + U11(Hash2(seed, 1u)) * 0.25f;
  for (int i = 0; i < points * 2; ++i) {
    bool outer = (i & 1) == 0;
    float rr = (outer ? r_outer : r_inner) * (0.85f + 0.30f * U01(Hash2(seed, 10u + i)));
    float a = base + (float)i / (points * 2) * kPi * 2 + U11(Hash2(seed, 30u + i)) * 0.12f;
    Vec2 p = Vec2(c) + Vec2::Polar(SinCos::FromRadians(a), rr);
    if (i == 0)
      pb.moveTo(p);
    else
      pb.lineTo(p);
  }
  pb.close();
  return pb.detach();
}
SkPath BurstPath(Vec2 c, float r_outer, float r_inner, int points, uint32_t seed) {
  return StarPath(c, r_outer, r_inner, seed, points);
}
SkPath SparklePath(Vec2 c, float r_outer, uint32_t seed) {
  return StarPath(c, r_outer, r_outer * 0.30f, seed, 4);
}
SkPath SunPath(Vec2 c, float core_r, float ray_len, int rays, uint32_t seed) {
  SkPathBuilder pb;
  for (int i = 0; i <= 24; ++i) {
    float a = (float)i / 24 * kPi * 2;
    float rr = core_r * (0.95f + 0.10f * U01(Hash2(seed, (uint32_t)i)));
    Vec2 p = Vec2(c) + Vec2::Polar(SinCos::FromRadians(a), rr);
    if (i == 0)
      pb.moveTo(p);
    else
      pb.lineTo(p);
  }
  pb.close();
  for (int i = 0; i < rays; ++i) {
    float a = (float)i / rays * kPi * 2 + U11(Hash2(seed, 50u + i)) * 0.10f;
    float w = 0.12f + 0.05f * U01(Hash2(seed, 70u + i));
    float len = ray_len * (0.7f + 0.6f * U01(Hash2(seed, 90u + i)));
    Vec2 b0 = Vec2(c) + Vec2::Polar(SinCos::FromRadians(a - w), core_r);
    Vec2 b1 = Vec2(c) + Vec2::Polar(SinCos::FromRadians(a + w), core_r);
    Vec2 tip = Vec2(c) + Vec2::Polar(SinCos::FromRadians(a), core_r + len);
    pb.moveTo(b0);
    pb.lineTo(tip);
    pb.lineTo(b1);
    pb.close();
  }
  return pb.detach();
}
SkPath ArrowPath(Vec2 from, Vec2 to, float head_len, float head_half, uint32_t seed) {
  SkVector d = to - from;
  float len = d.length();
  if (len < 1e-6f) return SkPath();
  SkVector dir = d * (1.f / len);
  SkVector nrm{-dir.fY, dir.fX};
  SkPath shaft = WobbleLine(from, to, std::max(0.36_mm, len * 0.06f), 1.46_mm, Hash2(seed, 1u));
  SkPathBuilder pb(shaft);
  SkPoint baseC = to.sk - dir * head_len;
  SkPoint b0 = baseC + nrm * head_half + nrm * (U11(Hash2(seed, 2u)) * head_half * 0.35f);
  SkPoint b1 = baseC - nrm * head_half + nrm * (U11(Hash2(seed, 3u)) * head_half * 0.35f);
  pb.moveTo(b0);
  pb.lineTo(to);
  pb.lineTo(b1);
  return pb.detach();
}
static SkPath HeartPath(SkPoint c, float r, uint32_t seed) {
  SkPathBuilder pb;
  float w = r * (1.0f + U11(Hash2(seed, 1u)) * 0.08f);
  float h = r * (1.05f + U11(Hash2(seed, 2u)) * 0.08f);
  float lobeL = 1.0f + U01(Hash2(seed, 5u)) * 0.28f;
  float lobeR = 0.80f + U01(Hash2(seed, 6u)) * 0.14f;
  float notch = c.fX + U11(Hash2(seed, 7u)) * w * 0.12f;
  SkPoint bottom{c.fX + U11(Hash2(seed, 8u)) * w * 0.06f, c.fY - h};
  pb.moveTo(bottom);
  pb.cubicTo(c.fX - w * 1.3f * lobeL, c.fY - h * 0.1f, c.fX - w * 0.85f * lobeL, c.fY + h * 0.9f,
             notch, c.fY + h * 0.25f);
  pb.cubicTo(c.fX + w * 0.85f * lobeR, c.fY + h * 0.9f, c.fX + w * 1.3f * lobeR, c.fY - h * 0.1f,
             bottom.fX, bottom.fY);
  pb.close();
  return pb.detach();
}

static void FillAndInk(SkCanvas& canvas, const SkPath& path, SkColor fill, float inkW = kStroke) {
  canvas.drawPath(path, Filler(fill));
  canvas.drawPath(path, InkPaint(kInk, inkW));
}

void DrawStar(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed) {
  FillAndInk(canvas, StarPath(c, r, r * 0.45f, seed), fill);
}
void DrawSparkle(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed) {
  FillAndInk(canvas, SparklePath(c, r, seed), fill, kStrokeHair);
}
void DrawSun(SkCanvas& canvas, Vec2 c, float core_r, float ray_len, SkColor fill, uint32_t seed) {
  FillAndInk(canvas, SunPath(c, core_r, ray_len, 11, seed), fill);
}
void DrawHeart(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed) {
  FillAndInk(canvas, HeartPath(c, r, seed), fill);
}
void DrawSmiley(SkCanvas& canvas, Vec2 c, float r, SkColor fill, uint32_t seed) {
  SkPath face = WobbleEllipse(c, r, r * (0.9f + 0.16f * U01(Hash2(seed, 3u))), r * 0.13f,
                              Hash2(seed, 0xFACEu));
  FillAndInk(canvas, face, fill);
  float ex = r * 0.36f, ey = r * 0.28f;
  SkPaint eye = Filler(kInk);
  canvas.drawCircle(c.x - ex, c.y + ey + r * 0.03f, r * 0.12f, eye);
  canvas.drawCircle(c.x + ex * 1.06f, c.y + ey - r * 0.05f, r * 0.085f, eye);
  SkPathBuilder mb;
  mb.moveTo(c.x - r * 0.42f, c.y - r * 0.14f);
  mb.quadTo(c.x + r * 0.05f, c.y - r * 0.54f, c.x + r * 0.42f, c.y - r * 0.18f);
  canvas.drawPath(mb.detach(), InkPaint(kInk, kStroke));
}
void DrawArrow(SkCanvas& canvas, Vec2 from, Vec2 to, SkColor color, float width, uint32_t seed) {
  float len = Length(to - from);
  float hl = std::max(1.2_mm, len * 0.22f), hh = std::max(0.73_mm, len * 0.13f);
  SkPaint p2 = InkPaint(color, width * 0.8f);
  p2.setAlphaf(0.5f);
  canvas.save();
  canvas.translate(0.18_mm, -0.18_mm);
  canvas.drawPath(ArrowPath(from, to, hl, hh, Hash2(seed, 9u)), p2);
  canvas.restore();
  canvas.drawPath(ArrowPath(from, to, hl, hh, seed), InkPaint(color, width));
}

void DrawBetaStamp(SkCanvas& canvas, Vec2 c, float r, float rotation_deg, uint32_t seed,
                   std::string_view label) {
  canvas.save();
  canvas.translate(c.x, c.y);
  canvas.rotate(rotation_deg);
  int pts = r < 3.5_mm ? 9 : 11;
  SkPath burst = BurstPath({0, 0}, r, r * 0.72f, pts, seed);
  HandShadow(canvas, burst, {0.36_mm, -0.36_mm}, kShadow, seed);
  canvas.drawPath(burst, Filler(kRed));
  canvas.drawPath(burst, InkPaint(kInkPure, kStroke));
  Rect box = Rect::MakeCenterZero(r * 1.9f, r * 0.84f);
  float tsize = std::min(r * 0.56f, (r * 1.7f) / std::max<size_t>(1, label.size()) * 1.3f);
  OutlineTextIn(canvas, label, box, tsize, kPaper, kInkPure, TextAlign::Center, true,
                Hash2(seed, 11u));
  canvas.restore();
}

// =============================================================== components ==
static SkColor Desat(SkColor c) { return MixColor(c, kGray, 0.65f); }

// Error indicator that works without color vision.
static void BangChip(SkCanvas& canvas, SkPoint center, float r) {
  uint32_t seed = Hash2((uint32_t)std::lround(center.fX / kSeedGrid),
                        (uint32_t)std::lround(center.fY / kSeedGrid));
  SkPath disc = WobbleEllipse(center, r, r, 0.15_mm, seed);
  HandShadow(canvas, disc, {0.22_mm, -0.22_mm}, kShadow, seed);
  canvas.drawPath(disc, Filler(kRed));
  canvas.drawPath(disc, InkPaint(kInkPure, kStroke));
  Rect b = Rect::MakeCircleR(r).MoveBy({center.fX, center.fY});
  OutlineTextIn(canvas, "!", b, r * 1.5f, kPaper, kInkPure, TextAlign::Center, false, seed);
}

void Panel(SkCanvas& canvas, const Rect& r, std::string_view title, SkColor accent, State state,
           uint32_t seed, bool sticker) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  bool active = state == State::Active || state == State::Hover;
  SkColor body = disabled ? Desat(kPaperCream) : kPaperCream;
  SkColor titleColor = error ? kRed : (disabled ? Desat(accent) : accent);
  SkColor ink = disabled ? kInkSoft : kInk;

  SkPath base = WonkyRoundRect(r, kCornerR, kWonk, seed);
  HandShadow(canvas, base, {kShadowDX, -kShadowDY}, kShadow, seed);
  MisregFill(canvas, base, body, seed);

  // The title band sits along the top edge.
  float titleH = kTitleSize + 2 * kPadS + 0.44_mm;
  Rect bandRect{r.left, r.top - titleH, r.right, r.top};
  canvas.save();
  canvas.clipPath(base, true);
  canvas.drawPath(WobbleRect(bandRect, kWonk, kSeg, Hash2(seed, 3u)), Filler(titleColor));
  canvas.restore();
  canvas.drawPath(
      WobbleLine({r.left, r.top - titleH}, {r.right, r.top - titleH}, kWonk, kSeg, Hash2(seed, 4u)),
      InkPaint(ink, kStroke));
  DrawStar(canvas, {r.left + kPadL, r.top - titleH * 0.5f}, kTitleSize * 0.42f, kYellow,
           Hash2(seed, 5u));
  // The right inset keeps the title clear of the corner stamp.
  Rect titleBox{r.left + kPadL * 2.0f, r.top - titleH, r.right - 6.7_mm, r.top};
  DrawTextIn(canvas, title, titleBox, kTitleSize, TextOn(titleColor), TextAlign::Left, true,
             Hash2(seed, 6u));

  SketchyStroke(canvas, base, error ? kRed : ink, active ? kStrokeBold : kStroke, seed, 2);

  if (disabled) {
    canvas.save();
    canvas.clipPath(base, true);
    HatchRect(canvas, r, kInkSoft, 1.3_mm, Hash2(seed, 8u));
    canvas.restore();
  }
  if (active && !disabled) Highlight(canvas, r, error ? kRed : kBlue, Hash2(seed, 9u));
  if (sticker)
    DrawBetaStamp(canvas, {r.right - 0.58_mm, r.top - 0.875_mm}, 3.1_mm, -12.f, Hash2(seed, 12u));
  if (error)
    BangChip(canvas, {r.right - (sticker ? 7.3_mm : 2.6_mm), r.top - titleH * 0.5f}, 1.3_mm);
}

void Button(SkCanvas& canvas, const Rect& r, std::string_view label, SkColor color, State state,
            uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool pressed = state == State::Pressed;
  bool hover = state == State::Hover;
  SkColor fill = disabled ? kGray
                          : (pressed ? MixColor(color, kInk, 0.12f)
                                     : (hover ? MixColor(color, kPaper, 0.25f) : color));
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath key = WonkyRoundRect(r, kCornerR, kWonk, seed);
  MisregFill(canvas, key, fill, seed);
  SketchyStroke(canvas, key, ink, kStrokeBold, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 1.2_mm, Hash2(seed, 23u));
  Rect lbl = r.sk.makeInset(kPadM, kPadS);
  DrawTextIn(canvas, label, lbl, kBodySize + 0.15_mm, disabled ? kInkSoft : TextOn(fill),
             TextAlign::Center, true, Hash2(seed, 22u));
  if (!disabled && !pressed) {
    float x0 = r.left + r.Width() * 0.16f, x1 = r.right - r.Width() * 0.16f;
    float yb = r.top - r.Height() * 0.34f, ya = r.top - r.Height() * 0.10f;
    SkPath shine = SkPathBuilder()
                       .moveTo(x0, yb)
                       .quadTo({(x0 + x1) * 0.5f, ya}, {x1, yb})
                       .quadTo({(x0 + x1) * 0.5f, yb - r.Height() * 0.05f}, {x0, yb})
                       .close()
                       .detach();
    SkPaint gloss = Filler(kPaper);
    gloss.setAlphaf(0.6f);
    canvas.drawPath(WobblePath(shine, kWonk * 0.5f, kSeg, Hash2(seed, 25u)), gloss);
  }
}

void Toggle(SkCanvas& canvas, const Rect& r, bool on, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  float h = r.Height();
  SkColor track = disabled ? kGray : (error ? kOrange : (on ? kGreen : kGray));
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath cap = WonkyRoundRect(r, h * 0.5f, kWonk, seed);
  HandShadow(canvas, cap, {0.36_mm, -0.36_mm}, kShadow, seed);
  MisregFill(canvas, cap, track, seed);
  SketchyStroke(canvas, cap, ink, kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 1.0_mm, Hash2(seed, 33u));
  float kr = h * 0.5f - kPadS * 0.6f;
  float kx = on ? r.right - kr - kPadS : r.left + kr + kPadS;
  SkPoint kc{kx, r.CenterY()};
  SkPath knob = WobbleEllipse(kc, kr, kr, 0.26_mm, Hash2(seed, 31u));
  HandShadow(canvas, knob, {0.22_mm, -0.22_mm}, kShadow, seed);
  FillAndInk(canvas, knob, (on && !disabled) ? kYellow : kPaper);
  Rect lbl = on ? Rect{r.left, r.bottom, kx - kr, r.top} : Rect{kx + kr, r.bottom, r.right, r.top};
  DrawTextIn(canvas, on ? "ON" : "OFF", lbl, kMicroSize, (on && !disabled) ? kPaper : ink,
             TextAlign::Center, false, Hash2(seed, 32u));
  if (state == State::Hover && !disabled) Highlight(canvas, r, kBlue, Hash2(seed, 35u));
}

void Checkbox(SkCanvas& canvas, const Rect& r, bool checked, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  SkColor ink = disabled ? kInkSoft : (error ? kRed : kInk);
  SkPath box = WonkyRoundRect(r, 0.44_mm, kWonk * 0.8f, seed);
  HandShadow(canvas, box, {0.29_mm, -0.29_mm}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, ink, kStroke, seed, 2);
  if (checked && !disabled) {
    SkPaint p = InkPaint(kGreen, kStroke + 0.15_mm);
    SkPath a = WobbleLine({r.left + r.Width() * 0.18f, r.CenterY() - r.Height() * 0.02f},
                          {r.CenterX() - r.Width() * 0.02f, r.bottom - r.Height() * 0.18f}, 0.22_mm,
                          1.46_mm, Hash2(seed, 41u));
    SkPath b = WobbleLine({r.CenterX() - r.Width() * 0.02f, r.bottom - r.Height() * 0.18f},
                          {r.right + r.Width() * 0.22f, r.top + r.Height() * 0.18f}, 0.22_mm,
                          1.46_mm, Hash2(seed, 42u));
    canvas.drawPath(a, p);
    canvas.drawPath(b, p);
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, 0.875_mm, Hash2(seed, 43u));
  if (state == State::Hover && !disabled) Highlight(canvas, r, kBlue, Hash2(seed, 45u));
}

void Radio(SkCanvas& canvas, Vec2 c, float r, bool selected, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath ring = WobbleEllipse(c, r, r, 0.18_mm, Hash2(seed, 51u));
  HandShadow(canvas, ring, {0.29_mm, -0.29_mm}, kShadow, seed);
  FillAndInk(canvas, ring, disabled ? kGray : kPaper);
  if (selected) {
    SkPoint off{c.x + U11(Hash2(seed, 52u)) * r * 0.12f, c.y + U11(Hash2(seed, 53u)) * r * 0.12f};
    FillAndInk(canvas, WobbleEllipse(off, r * 0.45f, r * 0.45f, 0.12_mm, Hash2(seed, 54u)),
               disabled ? kInkSoft : kBlue, kStrokeHair);
  }
  if (state == State::Hover && !disabled) {
    Highlight(canvas, Rect::MakeCircleR(r).MoveBy({c.x, c.y}), kBlue, Hash2(seed, 55u));
  }
}

void Slider(SkCanvas& canvas, const Rect& r, float t, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  t = std::clamp(t, 0.f, 1.f);
  float y = r.CenterY();
  SkPoint a{r.left + kPadM, y}, b{r.right - kPadM, y};
  SkColor ink = disabled ? kInkSoft : kInk;
  canvas.drawPath(WobbleLine(a, b, kWonk, kSeg, Hash2(seed, 61u)), InkPaint(ink, kStrokeBold));
  float tx = a.fX + (b.fX - a.fX) * t;
  SkColor fill = disabled ? kGray : kCyan;
  canvas.drawPath(WobbleLine(a, {tx, y}, kWonk, kSeg, Hash2(seed, 62u)),
                  InkPaint(fill, kStrokeBold));
  for (SkPoint e : {a, b}) {
    canvas.drawCircle(e.fX, e.fY, 0.44_mm, Filler(kPaper));
    canvas.drawCircle(e.fX, e.fY, 0.44_mm, InkPaint(ink, kStrokeHair));
  }
  SkPoint tc{tx, y};
  float gr = r.Height() * 0.32f;
  SkPath gem = StarPath(tc, gr, gr * 0.62f, Hash2(seed, 63u), 4);
  HandShadow(canvas, gem, {0.29_mm, -0.29_mm}, kShadow, seed);
  FillAndInk(canvas, gem, disabled ? kGray : kYellow);
}

void Knob(SkCanvas& canvas, Vec2 c, float radius, float t, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  t = std::clamp(t, 0.f, 1.f);
  SkColor ink = disabled ? kInkSoft : kInk;
  float sweep = 270.f / 180.f * kPi;
  float start = 135.f / 180.f * kPi;  // the 90 deg gap faces down
  float ang = start + sweep * t;
  for (int i = 0; i <= 10; ++i) {
    float jt = U11(Hash2(seed, 200u + i));
    float a = start + sweep * (i / 10.f) + jt * 0.045f;
    float ro = radius + 0.58_mm + jt * 0.23_mm;
    Vec2 p = Vec2(c) + Vec2::Polar(-SinCos::FromRadians(a), ro);
    canvas.drawCircle(p.x, p.y, 0.15_mm + 0.12_mm * U01(Hash2(seed, 210u + i)), Filler(ink));
  }
  SkPath body = WobbleEllipse(c, radius, radius, 0.32_mm, Hash2(seed, 71u));
  HandShadow(canvas, body, {0.44_mm, -0.44_mm}, kShadow, seed);
  FillAndInk(canvas, body, disabled ? kGray : kPaperCream, kStrokeBold);
  if (!disabled) {
    SkPaint arc = InkPaint(kYellow, kStrokeBold);
    std::vector<SkPoint> pts;
    int n = std::max(2, (int)(t * 24));
    for (int i = 0; i <= n; ++i) {
      float a = start + sweep * t * (i / (float)n);
      pts.push_back(Vec2(c) + Vec2::Polar(-SinCos::FromRadians(a), radius * 0.72f));
    }
    canvas.drawPath(PolyPath(pts, false), arc);
  }
  Vec2 tip = Vec2(c) + Vec2::Polar(-SinCos::FromRadians(ang), radius * 0.82f);
  canvas.drawPath(WobbleLine(c, tip, 0.15_mm, 1.46_mm, Hash2(seed, 72u)),
                  InkPaint(ink, kStrokeBold));
  canvas.drawCircle(tip.x, tip.y, kStroke + 0.22_mm, Filler(disabled ? kGray : kYellow));
  canvas.drawCircle(tip.x, tip.y, kStroke + 0.22_mm, InkPaint(ink, kStrokeHair));
  canvas.drawCircle(c.x, c.y, kStrokeHair, Filler(ink));
}

void Field(SkCanvas& canvas, const Rect& r, std::string_view text, bool focused, State state,
           uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  SkColor ink = error ? kRed : (focused ? kBlue : (disabled ? kInkSoft : kInk));
  SkPath box = WonkyRoundRect(r, 0.44_mm, kWonk * 0.7f, seed);
  HandShadow(canvas, box, {0.29_mm, -0.29_mm}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, ink, focused ? kStrokeBold : kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 1.0_mm, Hash2(seed, 83u));
  Rect inner{r.sk.makeInset(kPadM, kPadS)};
  float tw = TextWidth(text, kBodySize);
  DrawText(canvas, text, {inner.left, r.CenterY() - kBodySize * 0.35f}, kBodySize,
           disabled ? kInkSoft : kInk, false, seed);
  if (focused) {
    float cx = inner.left + tw + 0.44_mm;
    canvas.drawPath(WobbleLine({cx, inner.bottom + 0.15_mm}, {cx, inner.top - 0.15_mm}, 0.15_mm,
                               1.2_mm, Hash2(seed, 81u)),
                    InkPaint(kInk, kStroke));
  }
  if (error) {
    float sy = r.CenterY() - kBodySize * 0.6f;
    float sw = std::max(3.5_mm, TextWidth(text, kBodySize));
    canvas.drawPath(
        WobbleLine({inner.left, sy}, {inner.left + sw, sy}, 0.32_mm, 0.73_mm, Hash2(seed, 82u)),
        InkPaint(kRed, kStroke));
    BangChip(canvas, {r.right - 2.0_mm, r.CenterY()}, 1.2_mm);
  }
}

void Dropdown(SkCanvas& canvas, const Rect& r, std::string_view value, SkColor accent, State state,
              uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool hover = state == State::Hover;
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath box = WonkyRoundRect(r, 0.44_mm, kWonk * 0.7f, seed);
  HandShadow(canvas, box, {0.29_mm, -0.29_mm}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, (hover && !disabled) ? kBlue : ink,
                (hover && !disabled) ? kStrokeBold : kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 1.0_mm, Hash2(seed, 9u));
  float kw = r.Height();
  Rect key{r.right - kw, r.bottom, r.right, r.top};
  if (!disabled) {
    SkPath kp = WonkyRoundRect(key.Inset(0.29_mm), 0.44_mm, kWonk * 0.6f, Hash2(seed, 2u));
    MisregFill(canvas, kp, accent, Hash2(seed, 2u));
    canvas.drawPath(kp, InkPaint(ink, kStroke));
  }
  SkPoint cc = key.Center();
  float ch = r.Height() * 0.16f;
  SkColor chevC = disabled ? kInkSoft : TextOn(accent);
  canvas.drawPath(WobbleLine({cc.fX - ch, cc.fY + ch * 0.5f}, {cc.fX, cc.fY - ch * 0.7f}, 0.12_mm,
                             0.875_mm, Hash2(seed, 3u)),
                  InkPaint(chevC, kStroke));
  canvas.drawPath(WobbleLine({cc.fX, cc.fY - ch * 0.7f}, {cc.fX + ch, cc.fY + ch * 0.5f}, 0.12_mm,
                             0.875_mm, Hash2(seed, 4u)),
                  InkPaint(chevC, kStroke));
  Rect lbl{r.left + kPadM, r.bottom, key.left - kPadS, r.top};
  DrawTextIn(canvas, value, lbl, kBodySize, disabled ? kInkSoft : kInk, TextAlign::Left, false,
             seed);
}

void Stepper(SkCanvas& canvas, const Rect& r, std::string_view value, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath box = WonkyRoundRect(r, 0.44_mm, kWonk * 0.7f, seed);
  HandShadow(canvas, box, {0.29_mm, -0.29_mm}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, ink, kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 1.0_mm, Hash2(seed, 9u));
  float kw = std::min(r.Height(), r.Width() * 0.3f);
  Rect minusK{r.left, r.bottom, r.left + kw, r.top};
  Rect plusK{r.right - kw, r.bottom, r.right, r.top};
  canvas.drawPath(WobbleLine({minusK.right, r.bottom + 0.44_mm}, {minusK.right, r.top - 0.44_mm},
                             0.15_mm, 1.2_mm, Hash2(seed, 1u)),
                  InkPaint(ink, kStrokeHair));
  canvas.drawPath(WobbleLine({plusK.left, r.bottom + 0.44_mm}, {plusK.left, r.top - 0.44_mm},
                             0.15_mm, 1.2_mm, Hash2(seed, 2u)),
                  InkPaint(ink, kStrokeHair));
  SkColor minus = disabled ? kInkSoft : kRed;
  SkColor plus = disabled ? kInkSoft : kGreen;
  float s = r.Height() * 0.22f;
  SkPoint mc = minusK.Center(), pc = plusK.Center();
  canvas.drawPath(
      WobbleLine({mc.fX - s, mc.fY}, {mc.fX + s, mc.fY}, 0.15_mm, 0.875_mm, Hash2(seed, 3u)),
      InkPaint(minus, kStrokeBold));
  canvas.drawPath(
      WobbleLine({pc.fX - s, pc.fY}, {pc.fX + s, pc.fY}, 0.15_mm, 0.875_mm, Hash2(seed, 4u)),
      InkPaint(plus, kStrokeBold));
  canvas.drawPath(
      WobbleLine({pc.fX, pc.fY - s}, {pc.fX, pc.fY + s}, 0.15_mm, 0.875_mm, Hash2(seed, 5u)),
      InkPaint(plus, kStrokeBold));
  Rect val{minusK.right, r.bottom, plusK.left, r.top};
  DrawTextIn(canvas, value, val, kBodySize, disabled ? kInkSoft : kInk, TextAlign::Center, false,
             seed);
}

void Port(SkCanvas& canvas, Vec2 c, float r, bool is_output, SkColor type, bool connected,
          State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkColor fill = is_output ? (disabled ? kGray : type) : kPaper;
  SkColor ink = disabled ? kInkSoft : kInk;
  if (is_output) {
    SkPath nub = ArrowPath({c.x + r * 0.2f, c.y}, {c.x + r * 1.7f, c.y}, r * 0.7f, r * 0.6f,
                           Hash2(seed, 91u));
    canvas.drawPath(nub, InkPaint(ink, kStrokeHair));
  }
  SkPath disc = WobbleEllipse(c, r, r, 0.15_mm, Hash2(seed, 92u));
  if (!connected) {
    canvas.drawPath(disc, Filler(WithAlpha(fill, 0.45f)));
    canvas.drawPath(disc, InkPaint(ink, kStrokeHair));
  } else {
    FillAndInk(canvas, disc, fill);
  }
}

void Cable(SkCanvas& canvas, Vec2 a, Vec2 b, SkColor color, uint32_t seed) {
  float dx = b.x - a.x;
  float sag = std::min(5.8_mm, std::abs(dx) * 0.2f) + 2.6_mm;
  // +Y up: the cable droops toward smaller Y, below the lower endpoint.
  SkPoint c1{a.x + dx * 0.33f, std::min(a.y, b.y) - sag};
  SkPoint c2{a.x + dx * 0.66f, std::min(a.y, b.y) - sag};
  SkPathBuilder pb;
  pb.moveTo(a);
  pb.cubicTo(c1, c2, b);
  SkPath droop = WobblePath(pb.detach(), 0.29_mm, 2.3_mm, seed);
  canvas.drawPath(droop, InkPaint(kInk, kStrokeBold + 0.36_mm));
  canvas.drawPath(droop, InkPaint(color, kStrokeBold));
  canvas.drawCircle(a.x, a.y, kStroke, Filler(kInk));
  canvas.drawCircle(b.x, b.y, kStroke, Filler(kInk));
}

void Badge(SkCanvas& canvas, Vec2 c, std::string_view label, SkColor color, float rotation_deg,
           uint32_t seed) {
  float w = std::max(4.1_mm, TextWidth(label, kMicroSize) + 2 * kPadM);
  float h = kMicroSize + 2 * kPadS + 0.29_mm;
  canvas.save();
  canvas.translate(c.x, c.y);
  canvas.rotate(rotation_deg);
  Rect box = Rect::MakeCenterZero(w, h);
  SkPath pill = WonkyRoundRect(box, h * 0.5f, kWonk * 0.6f, seed);
  HandShadow(canvas, pill, {0.29_mm, -0.29_mm}, kShadow, seed);
  MisregFill(canvas, pill, color, seed);
  SketchyStroke(canvas, pill, kInk, kStroke, seed, 1);
  DrawTextIn(canvas, label, box, kMicroSize, TextOn(color), TextAlign::Center, false,
             Hash2(seed, 1u));
  canvas.restore();
}

void ThumbWell(SkCanvas& canvas, const Rect& r, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkPath frame = WonkyRoundRect(r, 0.58_mm, kWonk, seed);
  HandShadow(canvas, frame, {kShadowDX, -kShadowDY}, kShadow, seed);
  MisregFill(canvas, frame, disabled ? kGray : kPaperCream, seed);
  SketchyStroke(canvas, frame, kInk, kStrokeBold, seed, 2);
  Rect inner = r.Inset(0.73_mm);
  canvas.drawPath(WobbleRect(inner, kWonk * 0.6f, kSeg, Hash2(seed, 2u)),
                  InkPaint(kInk, kStrokeHair));
  canvas.save();
  canvas.clipPath(WobbleRect(inner, kWonk * 0.6f, kSeg, Hash2(seed, 2u)), true);
  // The sun sits near the top; the mountains rise from a baseline up to their
  // peaks and fill down to the bottom edge.
  DrawSun(canvas, {inner.right - inner.Width() * 0.26f, inner.top - inner.Height() * 0.30f},
          inner.Width() * 0.09f, inner.Width() * 0.07f, kGold, Hash2(seed, 3u));
  SkPathBuilder m;
  float gy = inner.top - inner.Height() * 0.72f;
  m.moveTo(inner.left, gy);
  m.lineTo(inner.left + inner.Width() * 0.28f, inner.top - inner.Height() * 0.40f);
  m.lineTo(inner.left + inner.Width() * 0.52f, gy);
  m.lineTo(inner.left + inner.Width() * 0.74f, inner.top - inner.Height() * 0.48f);
  m.lineTo(inner.right, gy);
  m.lineTo(inner.right, inner.bottom);
  m.lineTo(inner.left, inner.bottom);
  m.close();
  SkPath mountains = m.detach();
  canvas.drawPath(mountains, Filler(kLime));
  canvas.drawPath(mountains, InkPaint(kInk, kStroke));
  canvas.restore();
  DrawTextIn(canvas, "NO IMG", Rect{inner.left, inner.bottom, inner.right, inner.bottom + 3.2_mm},
             kMicroSize, kInk, TextAlign::Center, true, Hash2(seed, 4u));
}

void Bubble(SkCanvas& canvas, const Rect& r, std::string_view text, Vec2 tail_to, SkColor color,
            uint32_t seed) {
  SkPath body = WonkyRoundRect(r, kCornerR, kWonk, seed);
  HandShadow(canvas, body, {kShadowDX, -kShadowDY}, kShadow, seed);
  // The tail leaves the bubble's bottom edge.
  SkPoint base{std::clamp(tail_to.x, r.left + 1.46_mm, r.right - 1.46_mm), r.bottom};
  SkPathBuilder tail;
  tail.moveTo(base.fX - 1.3_mm, r.bottom + 0.29_mm);
  tail.lineTo(tail_to.x, tail_to.y);
  tail.lineTo(base.fX + 1.3_mm, r.bottom + 0.29_mm);
  tail.close();
  SkPath tailP = tail.detach();
  canvas.drawPath(tailP, Filler(color));
  MisregFill(canvas, body, color, seed);
  canvas.drawPath(tailP, InkPaint(kInk, kStroke));
  SketchyStroke(canvas, body, kInk, kStroke, seed, 1);
  DrawTextIn(canvas, text, r.sk.makeInset(kPadM, kPadS), kBodySize, kInk, TextAlign::Center, false,
             Hash2(seed, 1u));
}

void Divider(SkCanvas& canvas, Vec2 a, Vec2 b, uint32_t seed) {
  SkVector d = b - a;
  float l = d.length();
  if (l > 1e-6f) d *= (1.f / l);
  SkPoint a2 = a.sk - d * 0.58_mm, b2 = b.sk + d * 0.58_mm;
  canvas.drawPath(WobbleLine(a2, b2, kWonk, kSeg, seed), InkPaint(kInk, kStroke));
  SkPoint mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
  DrawStar(canvas, mid, 0.73_mm, kYellow, Hash2(seed, 1u));
}

void Activity(SkCanvas& canvas, const Rect& r, float t, State state, uint32_t seed) {
  bool error = state == State::Error;
  t = std::clamp(t, 0.f, 1.f);
  SkPath track = WonkyRoundRect(r, r.Height() * 0.5f, kWonk * 0.6f, seed);
  MisregFill(canvas, track, kPaper, seed);
  SketchyStroke(canvas, track, kInk, kStroke, seed, 1);
  if (t > 0.01f) {
    Rect fillR{r.left, r.bottom, r.left + r.Width() * t, r.top};
    SkPath fillP =
        WonkyRoundRect(fillR.Inset(0.29_mm), r.Height() * 0.4f, kWonk * 0.5f, Hash2(seed, 1u));
    SkColor c = error ? kRed : (t >= 0.999f ? kGreen : kOrange);
    canvas.drawPath(fillP, Filler(c));
    canvas.drawPath(fillP, InkPaint(kInk, kStrokeHair));
  }
  char pct[8];
  std::snprintf(pct, sizeof(pct), "%d%%", (int)std::lround(t * 100));
  float cw = TextWidth(pct, kMicroSize) + 1.75_mm;
  Rect chip = Rect::MakeCenter(r.Center(), cw, 2.6_mm);
  SkPath chipP = WonkyRoundRect(chip, 0.73_mm, 0.18_mm, Hash2(seed, 4u));
  canvas.drawPath(chipP, Filler(kPaper));
  canvas.drawPath(chipP, InkPaint(kInk, kStrokeHair));
  DrawTextIn(canvas, pct, chip, kMicroSize, kInk, TextAlign::Center, false, Hash2(seed, 3u));
}

void Spinner(SkCanvas& canvas, Vec2 c, float r, float phase, uint32_t seed) {
  canvas.save();
  canvas.translate(c.x, c.y);
  canvas.rotate(phase * 360.f);
  DrawSun(canvas, {0, 0}, r * 0.52f, r * 0.5f, kYellow, seed);
  canvas.restore();
  for (int i = 0; i < 2; ++i) {
    Vec2 p = Vec2(c) + Vec2::Polar(SinCos::FromRadians((phase + i * 0.5f) * kPi * 2), r * 1.15f);
    DrawSparkle(canvas, p, r * 0.18f, kRose, Hash2(seed, 40u + i));
  }
}

void Grip(SkCanvas& canvas, const Rect& r, bool resize, uint32_t seed) {
  if (resize) {
    SkPaint p = InkPaint(kInkSoft, kStroke - 0.073_mm);
    for (int i = 0; i < 3; ++i) {
      float o = (i + 1) * (std::min(r.Width(), r.Height()) / 3.2f);
      // The resize hatch sits at the bottom-right corner.
      canvas.drawPath(
          WobbleLine({r.right - o, r.bottom - 0.29_mm}, {r.right + 0.29_mm, r.bottom + o}, 0.15_mm,
                     0.875_mm, Hash2(seed, (uint32_t)i)),
          p);
    }
  } else {
    for (int gx = 0; gx < 2; ++gx)
      for (int gy = 0; gy < 3; ++gy) {
        uint32_t h = Hash2(seed, (uint32_t)(gx * 3 + gy));
        float x = r.CenterX() + (gx - 0.5f) * 1.0_mm + U11(h) * 0.18_mm;
        float yy = r.CenterY() + (gy - 1.f) * 1.0_mm + U11(Hash2(h, 9u)) * 0.18_mm;
        canvas.drawCircle(x, yy, 0.25_mm, Filler(kInkSoft));
      }
  }
}

void Highlight(SkCanvas& canvas, const Rect& r, SkColor color, uint32_t seed) {
  Rect o = r.Outset(0.73_mm);
  SkPath ring = WonkyRoundRect(o, kCornerR + 0.58_mm, kWonk, Hash2(seed, 1u));
  SkPathMeasure meas(ring, true);
  float len = meas.getLength();
  SkPaint p = InkPaint(color, kStroke);
  float dash = 1.3_mm, gap = 0.875_mm;
  for (float d = 0; d < len; d += dash + gap) {
    SkPathBuilder seg;
    if (meas.getSegment(d, std::min(len, d + dash), &seg, true)) canvas.drawPath(seg.detach(), p);
  }
}

// ----------------------------------------------------------------- widgets ---

RunButton::RunButton(Widget* parent, std::function<void()> on_click, uint32_t seed)
    : Widget(parent), clickable(*this), on_click(std::move(on_click)), seed(seed) {
  clickable.activate = [this](Pointer&) {
    if (enabled && this->on_click) this->on_click();
  };
}

ui::Tock RunButton::Tick(time::Timer& t) {
  if (parent) {
    // Lower center, dipping slightly past the border - aligned with where the
    // "next" connector leaves the object.
    Rect bounds{parent->Shape().getBounds()};
    local_to_parent = SkM44::Translate(bounds.CenterX(), bounds.bottom + kRadius - kOverhang);
  }
  Tock tock = clickable.Tick(t);
  if (enabled && clickable.pointers_over > 0 && clickable.pointers_pressing == 0) {
    wiggle = static_cast<uint32_t>(t.NowSeconds() * 5.0);
    tock.draw = true;
    tock.next_tick = min(tock.next_tick, t.now + 200ms);
  }
  return tock;
}

void RunButton::Draw(SkCanvas& canvas) const {
  bool pressed = clickable.pointers_pressing > 0;
  bool hover = clickable.pointers_over > 0;

  bool shimmer = enabled && hover && !pressed;
  const uint32_t kSeed = shimmer ? Hash3(0x60D, seed, wiggle) : Hash2(0x60D, seed);
  float r = kRadius;

  SkColor base = !enabled ? kGray : running ? kRed : kGreen;
  SkColor fill = !enabled  ? base
                 : pressed ? MixColor(base, kInk, 0.12f)
                 : hover   ? MixColor(base, kPaper, 0.25f)
                           : base;
  SkPath body = WobbleEllipse({0, 0}, r, r * 0.97f, kWonk, kSeed, 56);

  MisregFill(canvas, body, fill, kSeed);
  SketchyStroke(canvas, body, !enabled ? kInkSoft : kInk, kStroke, kSeed, 2);
  if (!enabled) {
    canvas.save();
    canvas.clipPath(body, true);
    HatchRect(canvas, body.getBounds(), kInkSoft, 1.2_mm, Hash2(kSeed, 0x44));
    canvas.restore();
  }

  SkPath symbol;
  if (running) {  // stop square
    float s = r * 0.46f;
    symbol = SkPath::Rect(Rect{-s, -s, s, s});
  } else {  // play triangle, pointing right (symmetric about the X axis)
    float tw = r * 0.52f;
    symbol = SkPathBuilder()
                 .moveTo(-tw * 0.62f, -tw)
                 .lineTo(tw, 0)
                 .lineTo(-tw * 0.62f, tw)
                 .close()
                 .detach();
  }
  // Half wonk: full strength mangles the small glyphs.
  symbol = WobblePath(symbol, kWonk * 0.45f, kSeg, Hash2(kSeed, 0x37u));
  FillPath(canvas, symbol, !enabled ? kGray : kPaper);
  SketchyStroke(canvas, symbol, !enabled ? kInkSoft : kInk, kStroke, Hash2(kSeed, 0x38u), 1);

  if (enabled && !pressed) {
    // +Y up: the shine arcs across the top of the disc, at larger Y.
    SkPath shine = SkPathBuilder()
                       .moveTo(-r * 0.45f, r * 0.60f)
                       .quadTo({0, r * 0.95f}, {r * 0.45f, r * 0.60f})
                       .quadTo({0, r * 0.60f}, {-r * 0.45f, r * 0.60f})
                       .close()
                       .detach();
    SkPaint gloss = Filler(kPaper);
    gloss.setAlphaf(0.75f);
    canvas.drawPath(WobblePath(shine, kWonk * 0.5f, kSeg, Hash2(kSeed, 0x5Fu)), gloss);
  }
}

}  // namespace automat::ui::beta
