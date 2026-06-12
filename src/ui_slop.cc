// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "ui_slop.hh"

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

#include "../build/generated/embedded.hh"
#include "font.hh"

#pragma comment(lib, "skia")

namespace automat::ui::slop {

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
  if (len < 1e-3f) {
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
    float taper = std::sin(u * 3.14159265f);
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

SkPath WobbleLine(SkPoint a, SkPoint b, float amp, float seg, uint32_t seed) {
  std::vector<SkPoint> pts;
  WobbleSegment(a, b, amp, seg, seed, 0.f, &pts);
  pts.push_back(b);
  return PolyPath(pts, false);
}

SkPath WobbleRect(const SkRect& r, float amp, float seg, uint32_t seed) {
  SkPoint c[4] = {
      {r.fLeft, r.fTop}, {r.fRight, r.fTop}, {r.fRight, r.fBottom}, {r.fLeft, r.fBottom}};
  SkPoint cen = {r.centerX(), r.centerY()};
  for (int i = 0; i < 4; ++i) {
    SkVector out = c[i] - cen;
    float l = out.length();
    if (l > 1e-3f) out *= (1.f / l);
    c[i] += out * (amp * 0.6f * U01(Hash2(seed, 100u + i)));
  }
  std::vector<SkPoint> pts;
  for (int e = 0; e < 4; ++e) WobbleSegment(c[e], c[(e + 1) & 3], amp, seg, seed, (float)e, &pts);
  return PolyPath(pts, true);
}

SkPath WobbleEllipse(SkPoint center, float rx, float ry, float amp, uint32_t seed, int samples) {
  SkPathBuilder pb;
  float ph1 = U01(Hash2(seed, 1u)) * 6.2831853f;
  float ph2 = U01(Hash2(seed, 2u)) * 6.2831853f;
  float ph3 = U01(Hash2(seed, 3u)) * 6.2831853f;
  float denom = std::max(rx, ry);
  for (int i = 0; i <= samples; ++i) {
    float a = (float)i / samples * 6.2831853f;
    float wob =
        std::sin(a * 3.f + ph1) + 0.5f * std::sin(a * 5.f + ph2) + 0.3f * std::sin(a * 2.f + ph3);
    float rscale = 1.0f + (wob / 1.8f) * (amp / denom);
    SkPoint p = {center.fX + std::cos(a) * rx * rscale, center.fY + std::sin(a) * ry * rscale};
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
    if (length < 1e-3f) {
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
      float taper = closed ? 1.0f : std::sin(t * 3.14159265f);
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

SkPath WonkyRoundRect(const SkRect& r, float baseRadius, float wobAmp, uint32_t seed) {
  float maxR = 0.45f * std::min(r.width(), r.height());
  float rad[4];
  for (int i = 0; i < 4; ++i)
    rad[i] = std::min(maxR, baseRadius * (0.5f + 1.0f * U01(Hash2(seed, 50u + i))));
  const float K = 0.5522847498f;
  SkPoint TL{r.fLeft, r.fTop}, TR{r.fRight, r.fTop}, BR{r.fRight, r.fBottom},
      BL{r.fLeft, r.fBottom};
  auto edgeBow = [&](SkPoint a, SkPoint b, uint32_t k) -> SkPoint {
    SkPoint mid = (a + b) * 0.5f;
    SkVector d = b - a;
    float l = d.length();
    if (l > 1e-3f) d *= (1.f / l);
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

void HandShadow(SkCanvas& canvas, const SkPath& shape, SkVector offset, SkColor shadow,
                uint32_t seed) {
  SkPath s = WobblePath(shape, 2.6f, 14.f, Hash2(seed, 0x5AD0u));
  s = s.makeTransform(SkMatrix::Translate(offset.fX, offset.fY));
  canvas.drawPath(s, Filler(shadow));
}

void FillPath(SkCanvas& canvas, const SkPath& path, SkColor color) {
  canvas.drawPath(path, Filler(color));
}

void MisregFill(SkCanvas& canvas, const SkPath& outline, SkColor fill, uint32_t seed) {
  SkRect b = outline.getBounds();
  SkPoint cen{b.centerX(), b.centerY()};
  float ox = U11(Hash2(seed, 11u)) * (kStroke * 1.9f);
  float oy = (0.4f + 0.6f * U01(Hash2(seed, 15u))) * (kStroke * 1.9f);
  float sx = 1.0f + U11(Hash2(seed, 13u)) * 0.035f;
  float sy = 1.0f + U11(Hash2(seed, 14u)) * 0.035f;
  SkMatrix m = SkMatrix::Translate(ox, oy);
  m.preTranslate(cen.fX, cen.fY);
  m.preScale(sx, sy);
  m.preTranslate(-cen.fX, -cen.fY);
  SkPath fillPath = WobblePath(outline, kStroke * 1.0f, 12.f, Hash2(seed, 0xF111u));
  fillPath = fillPath.makeTransform(m);
  canvas.drawPath(fillPath, Filler(fill));
}

void SketchyStroke(SkCanvas& canvas, const SkPath& outline, SkColor color, float width,
                   uint32_t seed, int passes) {
  for (int k = 0; k < passes; ++k) {
    uint32_t s = Hash2(seed, 9000u + k);
    float dx = U11(Hash2(s, 1u)) * (width * 0.3f);
    float dy = U11(Hash2(s, 2u)) * (width * 0.3f);
    SkPaint p = InkPaint(color, width * (k == 0 ? 1.0f : 0.8f));
    p.setAlphaf(k == 0 ? 1.0f : 0.4f);
    SkPath path = WobblePath(outline, width * 0.35f, 12.f, s);
    canvas.save();
    canvas.translate(dx, dy);
    canvas.drawPath(path, p);
    canvas.restore();
  }
}

void ScribbleFill(SkCanvas& canvas, const SkPath& shape, SkColor color, float strokeW,
                  float spacing, uint32_t seed) {
  SkRect b = shape.getBounds();
  canvas.save();
  canvas.clipPath(shape, true);
  SkPaint p = InkPaint(color, strokeW);
  Rng rng(Hash2(seed, 0xC2A1u));
  float angle = 0.5f + rng.f11() * 0.15f;
  float msa = std::sin(angle), mca = std::cos(angle);
  float diag = b.width() + b.height();
  for (float d = -diag; d < diag; d += spacing) {
    SkPoint mid{b.centerX() - msa * d, b.centerY() + mca * d};
    float aj = angle + rng.f11() * 0.10f;
    float ca = std::cos(aj), sa = std::sin(aj);
    SkPoint a = {mid.fX - ca * diag, mid.fY - sa * diag};
    SkPoint z = {mid.fX + ca * diag, mid.fY + sa * diag};
    SkPath ln = WobbleLine(a, z, strokeW * 0.9f, 14.f, Hash2(seed, (uint32_t)std::lround(d * 7.f)));
    p.setStrokeWidth(strokeW * (0.7f + rng.f01() * 0.8f));
    p.setAlphaf(0.45f + 0.35f * rng.f01());
    canvas.drawPath(ln, p);
  }
  canvas.restore();
}

void SprayDisc(SkCanvas& canvas, SkPoint center, float radius, int count, SkColor color, float dotR,
               uint32_t seed) {
  Rng rng(Hash2(seed, 0x59A1u));
  SkPaint p = Filler(color);
  for (int i = 0; i < count; ++i) {
    float rr = radius * std::sqrt(rng.f01()) * (0.4f + 0.6f * rng.f01());
    float a = rng.f01() * 6.2831853f;
    canvas.drawCircle(center.fX + std::cos(a) * rr, center.fY + std::sin(a) * rr, dotR, p);
  }
}

void HatchRect(SkCanvas& canvas, const SkRect& r, SkColor color, float spacing, uint32_t seed) {
  canvas.save();
  canvas.clipRect(r, true);
  SkPaint p = InkPaint(color, kStrokeHair);
  p.setAlphaf(0.5f);
  for (float x = r.fLeft - r.height(); x < r.fRight; x += spacing) {
    SkPath ln = WobbleLine({x, r.fBottom}, {x + r.height(), r.fTop}, 1.0f, 14.f,
                           Hash2(seed, (uint32_t)std::lround(x)));
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

float TextWidth(std::string_view text, float size) {
  float w = 0.f;
  ForEachRun(text, [&](const char* s, size_t len, sk_sp<SkTypeface> tf) {
    w += MakeFontTf(std::move(tf), size).measureText(s, len, SkTextEncoding::kUTF8);
  });
  return w;
}

void DrawText(SkCanvas& canvas, std::string_view text, SkPoint baseline_left, float size,
              SkColor color, bool wonk, uint32_t seed) {
  if (text.empty()) return;
  SkPaint paint = Filler(color);
  if (!wonk) {
    float pen = baseline_left.fX;
    ForEachRun(text, [&](const char* s, size_t len, sk_sp<SkTypeface> tf) {
      SkFont f = MakeFontTf(std::move(tf), size);
      canvas.drawSimpleText(s, len, SkTextEncoding::kUTF8, pen, baseline_left.fY, f, paint);
      pen += f.measureText(s, len, SkTextEncoding::kUTF8);
    });
    return;
  }
  float pen = baseline_left.fX;
  float maxRot = 0.04f;
  float bob = std::min(size * 0.045f, 1.8f);
  const char* p = text.data();
  const char* end = p + text.size();
  uint32_t i = 0;
  while (p < end) {
    const char* cs = p;
    SkUnichar cp = SkUTF::NextUTF8(&p, end);
    if (cp < 0) break;
    size_t len = (size_t)(p - cs);
    SkFont f = MakeFontTf(FontForCodepoint(cp), size);
    float adv = f.measureText(cs, len, SkTextEncoding::kUTF8);
    uint32_t h = Hash2(seed, i++);
    float dy = U11(h) * bob;
    float rot = U11(Hash2(h, 7u)) * maxRot;
    float scl = 1.0f + U11(Hash2(h, 9u)) * 0.05f;
    canvas.save();
    canvas.translate(pen + adv * 0.5f, baseline_left.fY + dy);
    canvas.rotate(rot * 57.2958f);
    canvas.scale(scl, scl);
    canvas.drawSimpleText(cs, len, SkTextEncoding::kUTF8, -adv * 0.5f, 0, f, paint);
    canvas.restore();
    pen += adv * scl;
  }
}

void DrawTextIn(SkCanvas& canvas, std::string_view text, const SkRect& box, float size,
                SkColor color, TextAlign align, bool wonk, uint32_t seed) {
  SkFont f = MakeFont(size);
  SkFontMetrics fm;
  f.getMetrics(&fm);
  float w = TextWidth(text, size);
  float x = box.fLeft;
  if (align == TextAlign::Center)
    x = box.centerX() - w * 0.5f;
  else if (align == TextAlign::Right)
    x = box.fRight - w;
  float y = box.centerY() - (fm.fAscent + fm.fDescent) * 0.5f;
  DrawText(canvas, text, {x, y}, size, color, wonk, seed);
}

static void HaloTextIn(SkCanvas& canvas, std::string_view text, const SkRect& box, float size,
                       SkColor fill, SkColor halo, TextAlign align, bool wonk, uint32_t seed) {
  for (int i = 0; i < 8; ++i) {
    float a = i / 8.f * 6.2831853f;
    canvas.save();
    canvas.translate(std::cos(a) * 1.7f, std::sin(a) * 1.7f);
    DrawTextIn(canvas, text, box, size, halo, align, wonk, seed);
    canvas.restore();
  }
  DrawTextIn(canvas, text, box, size, fill, align, wonk, seed);
}

// =================================================================== motifs ==
SkPath StarPath(SkPoint c, float r_outer, float r_inner, uint32_t seed, int points) {
  SkPathBuilder pb;
  float base = -1.5707963f + U11(Hash2(seed, 1u)) * 0.25f;
  for (int i = 0; i < points * 2; ++i) {
    bool outer = (i & 1) == 0;
    float rr = (outer ? r_outer : r_inner) * (0.85f + 0.30f * U01(Hash2(seed, 10u + i)));
    float a = base + (float)i / (points * 2) * 6.2831853f + U11(Hash2(seed, 30u + i)) * 0.12f;
    SkPoint p{c.fX + std::cos(a) * rr, c.fY + std::sin(a) * rr};
    if (i == 0)
      pb.moveTo(p);
    else
      pb.lineTo(p);
  }
  pb.close();
  return pb.detach();
}
SkPath BurstPath(SkPoint c, float r_outer, float r_inner, int points, uint32_t seed) {
  return StarPath(c, r_outer, r_inner, seed, points);
}
SkPath SparklePath(SkPoint c, float r_outer, uint32_t seed) {
  return StarPath(c, r_outer, r_outer * 0.30f, seed, 4);
}
SkPath SunPath(SkPoint c, float core_r, float ray_len, int rays, uint32_t seed) {
  SkPathBuilder pb;
  for (int i = 0; i <= 24; ++i) {
    float a = (float)i / 24 * 6.2831853f;
    float rr = core_r * (0.95f + 0.10f * U01(Hash2(seed, (uint32_t)i)));
    SkPoint p{c.fX + std::cos(a) * rr, c.fY + std::sin(a) * rr};
    if (i == 0)
      pb.moveTo(p);
    else
      pb.lineTo(p);
  }
  pb.close();
  for (int i = 0; i < rays; ++i) {
    float a = (float)i / rays * 6.2831853f + U11(Hash2(seed, 50u + i)) * 0.10f;
    float w = 0.12f + 0.05f * U01(Hash2(seed, 70u + i));
    float len = ray_len * (0.7f + 0.6f * U01(Hash2(seed, 90u + i)));
    SkPoint b0{c.fX + std::cos(a - w) * core_r, c.fY + std::sin(a - w) * core_r};
    SkPoint b1{c.fX + std::cos(a + w) * core_r, c.fY + std::sin(a + w) * core_r};
    SkPoint tip{c.fX + std::cos(a) * (core_r + len), c.fY + std::sin(a) * (core_r + len)};
    pb.moveTo(b0);
    pb.lineTo(tip);
    pb.lineTo(b1);
    pb.close();
  }
  return pb.detach();
}
SkPath ArrowPath(SkPoint from, SkPoint to, float head_len, float head_half, uint32_t seed) {
  SkVector d = to - from;
  float len = d.length();
  if (len < 1e-3f) return SkPath();
  SkVector dir = d * (1.f / len);
  SkVector nrm{-dir.fY, dir.fX};
  SkPath shaft = WobbleLine(from, to, std::max(2.5f, len * 0.06f), 10.f, Hash2(seed, 1u));
  SkPathBuilder pb(shaft);
  SkPoint baseC = to - dir * head_len;
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
  SkPoint bottom{c.fX + U11(Hash2(seed, 8u)) * w * 0.06f, c.fY + h};
  pb.moveTo(bottom);
  pb.cubicTo(c.fX - w * 1.3f * lobeL, c.fY + h * 0.1f, c.fX - w * 0.85f * lobeL, c.fY - h * 0.9f,
             notch, c.fY - h * 0.25f);
  pb.cubicTo(c.fX + w * 0.85f * lobeR, c.fY - h * 0.9f, c.fX + w * 1.3f * lobeR, c.fY + h * 0.1f,
             bottom.fX, bottom.fY);
  pb.close();
  return pb.detach();
}

static void FillAndInk(SkCanvas& canvas, const SkPath& path, SkColor fill, float inkW = kStroke) {
  canvas.drawPath(path, Filler(fill));
  canvas.drawPath(path, InkPaint(kInk, inkW));
}

void DrawStar(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed) {
  FillAndInk(canvas, StarPath(c, r, r * 0.45f, seed), fill);
}
void DrawSparkle(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed) {
  FillAndInk(canvas, SparklePath(c, r, seed), fill, kStrokeHair);
}
void DrawSun(SkCanvas& canvas, SkPoint c, float core_r, float ray_len, SkColor fill,
             uint32_t seed) {
  FillAndInk(canvas, SunPath(c, core_r, ray_len, 11, seed), fill);
}
void DrawHeart(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed) {
  FillAndInk(canvas, HeartPath(c, r, seed), fill);
}
void DrawSmiley(SkCanvas& canvas, SkPoint c, float r, SkColor fill, uint32_t seed) {
  SkPath face = WobbleEllipse(c, r, r * (0.9f + 0.16f * U01(Hash2(seed, 3u))), r * 0.13f,
                              Hash2(seed, 0xFACEu));
  FillAndInk(canvas, face, fill);
  float ex = r * 0.36f, ey = r * 0.28f;
  SkPaint eye = Filler(kInk);
  canvas.drawCircle(c.fX - ex, c.fY - ey - r * 0.03f, r * 0.12f, eye);
  canvas.drawCircle(c.fX + ex * 1.06f, c.fY - ey + r * 0.05f, r * 0.085f, eye);
  SkPathBuilder mb;
  mb.moveTo(c.fX - r * 0.42f, c.fY + r * 0.14f);
  mb.quadTo(c.fX + r * 0.05f, c.fY + r * 0.54f, c.fX + r * 0.42f, c.fY + r * 0.18f);
  canvas.drawPath(mb.detach(), InkPaint(kInk, kStroke));
}
void DrawArrow(SkCanvas& canvas, SkPoint from, SkPoint to, SkColor color, float width,
               uint32_t seed) {
  float len = (to - from).length();
  float hl = std::max(8.f, len * 0.22f), hh = std::max(5.f, len * 0.13f);
  SkPaint p2 = InkPaint(color, width * 0.8f);
  p2.setAlphaf(0.5f);
  canvas.save();
  canvas.translate(1.2f, 1.2f);
  canvas.drawPath(ArrowPath(from, to, hl, hh, Hash2(seed, 9u)), p2);
  canvas.restore();
  canvas.drawPath(ArrowPath(from, to, hl, hh, seed), InkPaint(color, width));
}

void DrawSlopStamp(SkCanvas& canvas, SkPoint c, float r, float rotation_deg, uint32_t seed,
                   std::string_view label) {
  canvas.save();
  canvas.translate(c.fX, c.fY);
  canvas.rotate(rotation_deg);
  int pts = r < 24 ? 9 : 11;
  SkPath burst = BurstPath({0, 0}, r, r * 0.72f, pts, seed);
  HandShadow(canvas, burst, {2.5f, 2.5f}, kShadow, seed);
  canvas.drawPath(burst, Filler(kRed));
  canvas.drawPath(burst, InkPaint(kInkPure, kStroke));
  SkRect box = SkRect::MakeXYWH(-r * 0.95f, -r * 0.42f, r * 1.9f, r * 0.84f);
  float tsize = std::min(r * 0.56f, (r * 1.7f) / std::max<size_t>(1, label.size()) * 1.3f);
  HaloTextIn(canvas, label, box, tsize, kPaper, kInkPure, TextAlign::Center, true,
             Hash2(seed, 11u));
  canvas.restore();
}

// =============================================================== components ==
static SkColor Desat(SkColor c) { return MixColor(c, kGray, 0.65f); }

// Error indicator that works without color vision.
static void BangChip(SkCanvas& canvas, SkPoint center, float r) {
  uint32_t seed = Hash2((uint32_t)std::lround(center.fX), (uint32_t)std::lround(center.fY));
  SkPath disc = WobbleEllipse(center, r, r, 1.0f, seed);
  HandShadow(canvas, disc, {1.5f, 1.5f}, kShadow, seed);
  canvas.drawPath(disc, Filler(kRed));
  canvas.drawPath(disc, InkPaint(kInkPure, kStroke));
  SkRect b = SkRect::MakeXYWH(center.fX - r, center.fY - r, 2 * r, 2 * r);
  HaloTextIn(canvas, "!", b, r * 1.5f, kPaper, kInkPure, TextAlign::Center, false, seed);
}

void Panel(SkCanvas& canvas, const SkRect& r, std::string_view title, SkColor accent, State state,
           uint32_t seed, bool sticker) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  bool active = state == State::Active || state == State::Hover;
  SkColor body = disabled ? Desat(kPaperCream) : kPaperCream;
  SkColor titleColor = error ? kRed : (disabled ? Desat(accent) : accent);
  SkColor ink = disabled ? kInkSoft : kInk;

  SkPath base = WonkyRoundRect(r, kCornerR, kWonk, seed);
  HandShadow(canvas, base, {kShadowDX, kShadowDY}, kShadow, seed);
  MisregFill(canvas, base, body, seed);

  float titleH = kTitleSize + 2 * kPadS + 3;
  SkRect bandRect = SkRect::MakeLTRB(r.fLeft, r.fTop, r.fRight, r.fTop + titleH);
  canvas.save();
  canvas.clipPath(base, true);
  canvas.drawPath(WobbleRect(bandRect, kWonk, kSeg, Hash2(seed, 3u)), Filler(titleColor));
  canvas.restore();
  canvas.drawPath(WobbleLine({r.fLeft, r.fTop + titleH}, {r.fRight, r.fTop + titleH}, kWonk, kSeg,
                             Hash2(seed, 4u)),
                  InkPaint(ink, kStroke));
  DrawStar(canvas, {r.fLeft + kPadL, r.fTop + titleH * 0.5f}, kTitleSize * 0.42f, kYellow,
           Hash2(seed, 5u));
  // The right inset keeps the title clear of the corner stamp.
  SkRect titleBox =
      SkRect::MakeLTRB(r.fLeft + kPadL * 2.0f, r.fTop, r.fRight - 46, r.fTop + titleH);
  DrawTextIn(canvas, title, titleBox, kTitleSize, TextOn(titleColor), TextAlign::Left, true,
             Hash2(seed, 6u));

  SketchyStroke(canvas, base, error ? kRed : ink, active ? kStrokeBold : kStroke, seed, 2);

  if (disabled) {
    canvas.save();
    canvas.clipPath(base, true);
    HatchRect(canvas, r, kInkSoft, 9.f, Hash2(seed, 8u));
    canvas.restore();
  }
  if (active && !disabled) Highlight(canvas, r, error ? kRed : kBlue, Hash2(seed, 9u));
  if (sticker) DrawSlopStamp(canvas, {r.fRight - 4, r.fTop + 6}, 21.f, -12.f, Hash2(seed, 12u));
  if (error) BangChip(canvas, {r.fRight - (sticker ? 50.f : 18.f), r.fTop + titleH * 0.5f}, 9.f);
}

void Button(SkCanvas& canvas, const SkRect& r, std::string_view label, SkColor color, State state,
            uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool pressed = state == State::Pressed;
  bool hover = state == State::Hover;
  SkColor fill = disabled ? kGray
                          : (pressed ? MixColor(color, kInk, 0.12f)
                                     : (hover ? MixColor(color, kPaper, 0.10f) : color));
  SkColor ink = disabled ? kInkSoft : kInk;
  float push = pressed ? 4.f : 0.f;
  SkPath key = WonkyRoundRect(r, kCornerR, kWonk, seed);
  if (!disabled && !pressed) HandShadow(canvas, key, {kShadowDX, kShadowDY}, kShadow, seed);
  canvas.save();
  canvas.translate(push, push);
  MisregFill(canvas, key, fill, seed);
  SketchyStroke(canvas, key, ink, kStrokeBold, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 8.f, Hash2(seed, 23u));
  if (hover && !disabled) {
    canvas.drawPath(WobblePath(key, kWonk, kSeg, Hash2(seed, 21u)), InkPaint(kYellow, kStrokeHair));
  }
  SkRect lbl = r.makeInset(kPadM, kPadS);
  DrawTextIn(canvas, label, lbl, kBodySize + 1, disabled ? kInkSoft : TextOn(fill),
             TextAlign::Center, true, Hash2(seed, 22u));
  canvas.restore();
}

void Toggle(SkCanvas& canvas, const SkRect& r, bool on, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  float h = r.height();
  SkColor track = disabled ? kGray : (error ? kOrange : (on ? kGreen : kGray));
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath cap = WonkyRoundRect(r, h * 0.5f, kWonk, seed);
  HandShadow(canvas, cap, {2.5f, 2.5f}, kShadow, seed);
  MisregFill(canvas, cap, track, seed);
  SketchyStroke(canvas, cap, ink, kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 7.f, Hash2(seed, 33u));
  float kr = h * 0.5f - kPadS * 0.6f;
  float kx = on ? r.fRight - kr - kPadS : r.fLeft + kr + kPadS;
  SkPoint kc{kx, r.centerY()};
  SkPath knob = WobbleEllipse(kc, kr, kr, 1.8f, Hash2(seed, 31u));
  HandShadow(canvas, knob, {1.5f, 1.5f}, kShadow, seed);
  FillAndInk(canvas, knob, (on && !disabled) ? kYellow : kPaper);
  SkRect lbl = on ? SkRect::MakeLTRB(r.fLeft, r.fTop, kx - kr, r.fBottom)
                  : SkRect::MakeLTRB(kx + kr, r.fTop, r.fRight, r.fBottom);
  DrawTextIn(canvas, on ? "ON" : "OFF", lbl, kMicroSize, (on && !disabled) ? kPaper : ink,
             TextAlign::Center, false, Hash2(seed, 32u));
}

void Checkbox(SkCanvas& canvas, const SkRect& r, bool checked, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  SkColor ink = disabled ? kInkSoft : (error ? kRed : kInk);
  SkPath box = WonkyRoundRect(r, 3.f, kWonk * 0.8f, seed);
  HandShadow(canvas, box, {2.f, 2.f}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, ink, kStroke, seed, 2);
  if (checked && !disabled) {
    SkPaint p = InkPaint(kGreen, kStroke + 1.f);
    SkPath a = WobbleLine({r.fLeft + r.width() * 0.18f, r.centerY() + r.height() * 0.02f},
                          {r.centerX() - r.width() * 0.02f, r.fBottom + r.height() * 0.18f}, 1.5f,
                          10.f, Hash2(seed, 41u));
    SkPath b = WobbleLine({r.centerX() - r.width() * 0.02f, r.fBottom + r.height() * 0.18f},
                          {r.fRight + r.width() * 0.22f, r.fTop - r.height() * 0.18f}, 1.5f, 10.f,
                          Hash2(seed, 42u));
    canvas.drawPath(a, p);
    canvas.drawPath(b, p);
  }
  if (disabled) HatchRect(canvas, r, kInkSoft, 6.f, Hash2(seed, 43u));
}

void Radio(SkCanvas& canvas, SkPoint c, float r, bool selected, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath ring = WobbleEllipse(c, r, r, 1.2f, Hash2(seed, 51u));
  HandShadow(canvas, ring, {2.f, 2.f}, kShadow, seed);
  FillAndInk(canvas, ring, disabled ? kGray : kPaper);
  if (selected) {
    SkPoint off{c.fX + U11(Hash2(seed, 52u)) * r * 0.12f, c.fY + U11(Hash2(seed, 53u)) * r * 0.12f};
    FillAndInk(canvas, WobbleEllipse(off, r * 0.45f, r * 0.45f, 0.8f, Hash2(seed, 54u)),
               disabled ? kInkSoft : kBlue, kStrokeHair);
  }
}

void Slider(SkCanvas& canvas, const SkRect& r, float t, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  t = std::clamp(t, 0.f, 1.f);
  float y = r.centerY();
  SkPoint a{r.fLeft + kPadM, y}, b{r.fRight - kPadM, y};
  SkColor ink = disabled ? kInkSoft : kInk;
  canvas.drawPath(WobbleLine(a, b, kWonk, kSeg, Hash2(seed, 61u)), InkPaint(ink, kStrokeBold));
  float tx = a.fX + (b.fX - a.fX) * t;
  SkColor fill = disabled ? kGray : kCyan;
  canvas.drawPath(WobbleLine(a, {tx, y}, kWonk, kSeg, Hash2(seed, 62u)),
                  InkPaint(fill, kStrokeBold));
  for (SkPoint e : {a, b}) {
    canvas.drawCircle(e.fX, e.fY, 3.f, Filler(kPaper));
    canvas.drawCircle(e.fX, e.fY, 3.f, InkPaint(ink, kStrokeHair));
  }
  SkPoint tc{tx, y};
  float gr = r.height() * 0.32f;
  SkPath gem = StarPath(tc, gr, gr * 0.62f, Hash2(seed, 63u), 4);
  HandShadow(canvas, gem, {2.f, 2.f}, kShadow, seed);
  FillAndInk(canvas, gem, disabled ? kGray : kYellow);
}

void Knob(SkCanvas& canvas, SkPoint c, float radius, float t, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  t = std::clamp(t, 0.f, 1.f);
  SkColor ink = disabled ? kInkSoft : kInk;
  float sweep = 270.f * 3.14159265f / 180.f;
  float start = 2.356194f;  // 135 deg; the 90 deg gap faces down
  float ang = start + sweep * t;
  for (int i = 0; i <= 10; ++i) {
    float jt = U11(Hash2(seed, 200u + i));
    float a = start + sweep * (i / 10.f) + jt * 0.045f;
    float ro = radius + 4 + jt * 1.6f;
    SkPoint p{c.fX + std::cos(a) * ro, c.fY + std::sin(a) * ro};
    canvas.drawCircle(p.fX, p.fY, 1.0f + 0.8f * U01(Hash2(seed, 210u + i)), Filler(ink));
  }
  SkPath body = WobbleEllipse(c, radius, radius, 2.2f, Hash2(seed, 71u));
  HandShadow(canvas, body, {3.f, 3.f}, kShadow, seed);
  FillAndInk(canvas, body, disabled ? kGray : kPaperCream, kStrokeBold);
  if (!disabled) {
    SkPaint arc = InkPaint(kYellow, kStrokeBold);
    std::vector<SkPoint> pts;
    int n = std::max(2, (int)(t * 24));
    for (int i = 0; i <= n; ++i) {
      float a = start + sweep * t * (i / (float)n);
      pts.push_back({c.fX + std::cos(a) * (radius * 0.72f), c.fY + std::sin(a) * (radius * 0.72f)});
    }
    canvas.drawPath(PolyPath(pts, false), arc);
  }
  SkPoint tip{c.fX + std::cos(ang) * radius * 0.82f, c.fY + std::sin(ang) * radius * 0.82f};
  canvas.drawPath(WobbleLine(c, tip, 1.0f, 10.f, Hash2(seed, 72u)), InkPaint(ink, kStrokeBold));
  canvas.drawCircle(tip.fX, tip.fY, kStroke, Filler(ink));
  canvas.drawCircle(c.fX, c.fY, kStrokeHair, Filler(ink));
}

void Field(SkCanvas& canvas, const SkRect& r, std::string_view text, bool focused, State state,
           uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool error = state == State::Error;
  SkColor ink = error ? kRed : (focused ? kBlue : (disabled ? kInkSoft : kInk));
  SkPath box = WonkyRoundRect(r, 3.f, kWonk * 0.7f, seed);
  HandShadow(canvas, box, {2.f, 2.f}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, ink, focused ? kStrokeBold : kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 7.f, Hash2(seed, 83u));
  SkRect inner = r.makeInset(kPadM, kPadS);
  float tw = TextWidth(text, kBodySize);
  DrawText(canvas, text, {inner.fLeft, r.centerY() + kBodySize * 0.35f}, kBodySize,
           disabled ? kInkSoft : kInk, false, seed);
  if (focused) {
    float cx = inner.fLeft + tw + 3;
    canvas.drawPath(
        WobbleLine({cx, inner.fTop + 1}, {cx, inner.fBottom - 1}, 1.0f, 8.f, Hash2(seed, 81u)),
        InkPaint(kInk, kStroke));
  }
  if (error) {
    float sy = r.centerY() + kBodySize * 0.6f;
    float sw = std::max(24.f, TextWidth(text, kBodySize));
    canvas.drawPath(
        WobbleLine({inner.fLeft, sy}, {inner.fLeft + sw, sy}, 2.2f, 5.f, Hash2(seed, 82u)),
        InkPaint(kRed, kStroke));
    BangChip(canvas, {r.fRight - 14, r.centerY()}, 8.5f);
  }
}

void Dropdown(SkCanvas& canvas, const SkRect& r, std::string_view value, SkColor accent,
              State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  bool hover = state == State::Hover;
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath box = WonkyRoundRect(r, 3.f, kWonk * 0.7f, seed);
  HandShadow(canvas, box, {2.f, 2.f}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, (hover && !disabled) ? kBlue : ink,
                (hover && !disabled) ? kStrokeBold : kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 7.f, Hash2(seed, 9u));
  float kw = r.height();
  SkRect key = SkRect::MakeLTRB(r.fRight - kw, r.fTop, r.fRight, r.fBottom);
  if (!disabled) {
    SkPath kp = WonkyRoundRect(key.makeInset(2.f, 2.f), 3.f, kWonk * 0.6f, Hash2(seed, 2u));
    MisregFill(canvas, kp, accent, Hash2(seed, 2u));
    canvas.drawPath(kp, InkPaint(ink, kStroke));
  }
  SkPoint cc{key.centerX(), key.centerY()};
  float ch = r.height() * 0.16f;
  SkColor chevC = disabled ? kInkSoft : TextOn(accent);
  canvas.drawPath(WobbleLine({cc.fX - ch, cc.fY - ch * 0.5f}, {cc.fX, cc.fY + ch * 0.7f}, 0.8f, 6.f,
                             Hash2(seed, 3u)),
                  InkPaint(chevC, kStroke));
  canvas.drawPath(WobbleLine({cc.fX, cc.fY + ch * 0.7f}, {cc.fX + ch, cc.fY - ch * 0.5f}, 0.8f, 6.f,
                             Hash2(seed, 4u)),
                  InkPaint(chevC, kStroke));
  SkRect lbl = SkRect::MakeLTRB(r.fLeft + kPadM, r.fTop, key.fLeft - kPadS, r.fBottom);
  DrawTextIn(canvas, value, lbl, kBodySize, disabled ? kInkSoft : kInk, TextAlign::Left, false,
             seed);
}

void Stepper(SkCanvas& canvas, const SkRect& r, std::string_view value, State state,
             uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkColor ink = disabled ? kInkSoft : kInk;
  SkPath box = WonkyRoundRect(r, 3.f, kWonk * 0.7f, seed);
  HandShadow(canvas, box, {2.f, 2.f}, kShadow, seed);
  MisregFill(canvas, box, disabled ? kGray : kPaper, seed);
  SketchyStroke(canvas, box, ink, kStroke, seed, 2);
  if (disabled) HatchRect(canvas, r, kInkSoft, 7.f, Hash2(seed, 9u));
  float kw = std::min(r.height(), r.width() * 0.3f);
  SkRect minusK = SkRect::MakeLTRB(r.fLeft, r.fTop, r.fLeft + kw, r.fBottom);
  SkRect plusK = SkRect::MakeLTRB(r.fRight - kw, r.fTop, r.fRight, r.fBottom);
  canvas.drawPath(WobbleLine({minusK.fRight, r.fTop + 3}, {minusK.fRight, r.fBottom - 3}, 1.f, 8.f,
                             Hash2(seed, 1u)),
                  InkPaint(ink, kStrokeHair));
  canvas.drawPath(WobbleLine({plusK.fLeft, r.fTop + 3}, {plusK.fLeft, r.fBottom - 3}, 1.f, 8.f,
                             Hash2(seed, 2u)),
                  InkPaint(ink, kStrokeHair));
  SkColor minus = disabled ? kInkSoft : kRed;
  SkColor plus = disabled ? kInkSoft : kGreen;
  float s = r.height() * 0.22f;
  SkPoint mc{minusK.centerX(), minusK.centerY()}, pc{plusK.centerX(), plusK.centerY()};
  canvas.drawPath(WobbleLine({mc.fX - s, mc.fY}, {mc.fX + s, mc.fY}, 1.f, 6.f, Hash2(seed, 3u)),
                  InkPaint(minus, kStrokeBold));
  canvas.drawPath(WobbleLine({pc.fX - s, pc.fY}, {pc.fX + s, pc.fY}, 1.f, 6.f, Hash2(seed, 4u)),
                  InkPaint(plus, kStrokeBold));
  canvas.drawPath(WobbleLine({pc.fX, pc.fY - s}, {pc.fX, pc.fY + s}, 1.f, 6.f, Hash2(seed, 5u)),
                  InkPaint(plus, kStrokeBold));
  SkRect val = SkRect::MakeLTRB(minusK.fRight, r.fTop, plusK.fLeft, r.fBottom);
  DrawTextIn(canvas, value, val, kBodySize, disabled ? kInkSoft : kInk, TextAlign::Center, false,
             seed);
}

void Port(SkCanvas& canvas, SkPoint c, float r, bool is_output, SkColor type, bool connected,
          State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkColor fill = is_output ? (disabled ? kGray : type) : kPaper;
  SkColor ink = disabled ? kInkSoft : kInk;
  if (is_output) {
    SkPath nub = ArrowPath({c.fX + r * 0.2f, c.fY}, {c.fX + r * 1.7f, c.fY}, r * 0.7f, r * 0.6f,
                           Hash2(seed, 91u));
    canvas.drawPath(nub, InkPaint(ink, kStrokeHair));
  }
  SkPath disc = WobbleEllipse(c, r, r, 1.0f, Hash2(seed, 92u));
  if (!connected) {
    canvas.drawPath(disc, Filler(WithAlpha(fill, 0.45f)));
    canvas.drawPath(disc, InkPaint(ink, kStrokeHair));
  } else {
    FillAndInk(canvas, disc, fill);
  }
}

void Cable(SkCanvas& canvas, SkPoint a, SkPoint b, SkColor color, uint32_t seed) {
  float dx = b.fX - a.fX;
  float sag = std::min(40.f, std::abs(dx) * 0.2f) + 18.f;
  SkPoint c1{a.fX + dx * 0.33f, std::max(a.fY, b.fY) + sag};
  SkPoint c2{a.fX + dx * 0.66f, std::max(a.fY, b.fY) + sag};
  SkPathBuilder pb;
  pb.moveTo(a);
  pb.cubicTo(c1, c2, b);
  SkPath droop = WobblePath(pb.detach(), 2.0f, 16.f, seed);
  canvas.drawPath(droop, InkPaint(kInk, kStrokeBold + 2.5f));
  canvas.drawPath(droop, InkPaint(color, kStrokeBold));
  canvas.drawCircle(a.fX, a.fY, kStroke, Filler(kInk));
  canvas.drawCircle(b.fX, b.fY, kStroke, Filler(kInk));
}

void Badge(SkCanvas& canvas, SkPoint c, std::string_view label, SkColor color, float rotation_deg,
           uint32_t seed) {
  float w = std::max(28.f, TextWidth(label, kMicroSize) + 2 * kPadM);
  float h = kMicroSize + 2 * kPadS + 2;
  canvas.save();
  canvas.translate(c.fX, c.fY);
  canvas.rotate(rotation_deg);
  SkRect box = SkRect::MakeXYWH(-w * 0.5f, -h * 0.5f, w, h);
  SkPath pill = WonkyRoundRect(box, h * 0.5f, kWonk * 0.6f, seed);
  HandShadow(canvas, pill, {2.f, 2.f}, kShadow, seed);
  MisregFill(canvas, pill, color, seed);
  SketchyStroke(canvas, pill, kInk, kStroke, seed, 1);
  DrawTextIn(canvas, label, box, kMicroSize, TextOn(color), TextAlign::Center, false,
             Hash2(seed, 1u));
  canvas.restore();
}

void ThumbWell(SkCanvas& canvas, const SkRect& r, State state, uint32_t seed) {
  bool disabled = state == State::Disabled;
  SkPath frame = WonkyRoundRect(r, 4.f, kWonk, seed);
  HandShadow(canvas, frame, {kShadowDX, kShadowDY}, kShadow, seed);
  MisregFill(canvas, frame, disabled ? kGray : kPaperCream, seed);
  SketchyStroke(canvas, frame, kInk, kStrokeBold, seed, 2);
  SkRect inner = r.makeInset(5, 5);
  canvas.drawPath(WobbleRect(inner, kWonk * 0.6f, kSeg, Hash2(seed, 2u)),
                  InkPaint(kInk, kStrokeHair));
  canvas.save();
  canvas.clipPath(WobbleRect(inner, kWonk * 0.6f, kSeg, Hash2(seed, 2u)), true);
  DrawSun(canvas, {inner.fRight - inner.width() * 0.26f, inner.fTop + inner.height() * 0.30f},
          inner.width() * 0.09f, inner.width() * 0.07f, kGold, Hash2(seed, 3u));
  SkPathBuilder m;
  float gy = inner.fTop + inner.height() * 0.72f;
  m.moveTo(inner.fLeft, gy);
  m.lineTo(inner.fLeft + inner.width() * 0.28f, inner.fTop + inner.height() * 0.40f);
  m.lineTo(inner.fLeft + inner.width() * 0.52f, gy);
  m.lineTo(inner.fLeft + inner.width() * 0.74f, inner.fTop + inner.height() * 0.48f);
  m.lineTo(inner.fRight, gy);
  m.lineTo(inner.fRight, inner.fBottom);
  m.lineTo(inner.fLeft, inner.fBottom);
  m.close();
  SkPath mountains = m.detach();
  canvas.drawPath(mountains, Filler(kLime));
  canvas.drawPath(mountains, InkPaint(kInk, kStroke));
  canvas.restore();
  DrawTextIn(canvas, "NO IMG",
             SkRect::MakeLTRB(inner.fLeft, inner.fBottom - 22, inner.fRight, inner.fBottom),
             kMicroSize, kInk, TextAlign::Center, true, Hash2(seed, 4u));
}

void Bubble(SkCanvas& canvas, const SkRect& r, std::string_view text, SkPoint tail_to,
            SkColor color, uint32_t seed) {
  SkPath body = WonkyRoundRect(r, kCornerR, kWonk, seed);
  HandShadow(canvas, body, {kShadowDX, kShadowDY}, kShadow, seed);
  SkPoint base{std::clamp(tail_to.fX, r.fLeft + 10.f, r.fRight - 10.f), r.fBottom};
  SkPathBuilder tail;
  tail.moveTo(base.fX - 9, r.fBottom - 2);
  tail.lineTo(tail_to.fX, tail_to.fY);
  tail.lineTo(base.fX + 9, r.fBottom - 2);
  tail.close();
  SkPath tailP = tail.detach();
  canvas.drawPath(tailP, Filler(color));
  MisregFill(canvas, body, color, seed);
  canvas.drawPath(tailP, InkPaint(kInk, kStroke));
  SketchyStroke(canvas, body, kInk, kStroke, seed, 1);
  DrawTextIn(canvas, text, r.makeInset(kPadM, kPadS), kBodySize, kInk, TextAlign::Center, false,
             Hash2(seed, 1u));
}

void Divider(SkCanvas& canvas, SkPoint a, SkPoint b, uint32_t seed) {
  SkVector d = b - a;
  float l = d.length();
  if (l > 1e-3f) d *= (1.f / l);
  SkPoint a2 = a - d * 4.f, b2 = b + d * 4.f;
  canvas.drawPath(WobbleLine(a2, b2, kWonk, kSeg, seed), InkPaint(kInk, kStroke));
  SkPoint mid{(a.fX + b.fX) * 0.5f, (a.fY + b.fY) * 0.5f};
  DrawStar(canvas, mid, 5.f, kYellow, Hash2(seed, 1u));
}

void Activity(SkCanvas& canvas, const SkRect& r, float t, State state, uint32_t seed) {
  bool error = state == State::Error;
  t = std::clamp(t, 0.f, 1.f);
  SkPath track = WonkyRoundRect(r, r.height() * 0.5f, kWonk * 0.6f, seed);
  MisregFill(canvas, track, kPaper, seed);
  SketchyStroke(canvas, track, kInk, kStroke, seed, 1);
  if (t > 0.01f) {
    SkRect fillR = SkRect::MakeLTRB(r.fLeft, r.fTop, r.fLeft + r.width() * t, r.fBottom);
    SkPath fillP =
        WonkyRoundRect(fillR.makeInset(2, 2), r.height() * 0.4f, kWonk * 0.5f, Hash2(seed, 1u));
    SkColor c = error ? kRed : (t >= 0.999f ? kGreen : kOrange);
    canvas.drawPath(fillP, Filler(c));
    canvas.drawPath(fillP, InkPaint(kInk, kStrokeHair));
  }
  char pct[8];
  std::snprintf(pct, sizeof(pct), "%d%%", (int)std::lround(t * 100));
  float cw = TextWidth(pct, kMicroSize) + 12;
  SkRect chip = SkRect::MakeXYWH(r.centerX() - cw * 0.5f, r.centerY() - 9, cw, 18);
  SkPath chipP = WonkyRoundRect(chip, 5, 1.2f, Hash2(seed, 4u));
  canvas.drawPath(chipP, Filler(kPaper));
  canvas.drawPath(chipP, InkPaint(kInk, kStrokeHair));
  DrawTextIn(canvas, pct, chip, kMicroSize, kInk, TextAlign::Center, false, Hash2(seed, 3u));
}

void Spinner(SkCanvas& canvas, SkPoint c, float r, float phase, uint32_t seed) {
  canvas.save();
  canvas.translate(c.fX, c.fY);
  canvas.rotate(phase * 360.f);
  DrawSun(canvas, {0, 0}, r * 0.52f, r * 0.5f, kYellow, seed);
  canvas.restore();
  for (int i = 0; i < 2; ++i) {
    float a = (phase + i * 0.5f) * 6.2831853f;
    DrawSparkle(canvas, {c.fX + std::cos(a) * r * 1.15f, c.fY + std::sin(a) * r * 1.15f}, r * 0.18f,
                kRose, Hash2(seed, 40u + i));
  }
}

void Grip(SkCanvas& canvas, const SkRect& r, bool resize, uint32_t seed) {
  if (resize) {
    SkPaint p = InkPaint(kInkSoft, kStroke - 0.5f);
    for (int i = 0; i < 3; ++i) {
      float o = (i + 1) * (std::min(r.width(), r.height()) / 3.2f);
      canvas.drawPath(WobbleLine({r.fRight - o, r.fBottom + 2}, {r.fRight + 2, r.fBottom - o}, 1.f,
                                 6.f, Hash2(seed, (uint32_t)i)),
                      p);
    }
  } else {
    for (int gx = 0; gx < 2; ++gx)
      for (int gy = 0; gy < 3; ++gy) {
        uint32_t h = Hash2(seed, (uint32_t)(gx * 3 + gy));
        float x = r.centerX() + (gx - 0.5f) * 7.f + U11(h) * 1.2f;
        float yy = r.centerY() + (gy - 1.f) * 7.f + U11(Hash2(h, 9u)) * 1.2f;
        canvas.drawCircle(x, yy, 1.7f, Filler(kInkSoft));
      }
  }
}

void Highlight(SkCanvas& canvas, const SkRect& r, SkColor color, uint32_t seed) {
  SkRect o = r.makeOutset(5, 5);
  SkPath ring = WonkyRoundRect(o, kCornerR + 4, kWonk, Hash2(seed, 1u));
  SkPathMeasure meas(ring, true);
  float len = meas.getLength();
  SkPaint p = InkPaint(color, kStroke);
  float dash = 9.f, gap = 6.f;
  for (float d = 0; d < len; d += dash + gap) {
    SkPathBuilder seg;
    if (meas.getSegment(d, std::min(len, d + dash), &seg, true)) canvas.drawPath(seg.detach(), p);
  }
}

// ----------------------------------------------------------------- widgets ---

RunButton::RunButton(Widget* parent, std::function<void()> on_click)
    : Widget(parent), clickable(*this), on_click(std::move(on_click)) {
  clickable.activate = [this](Pointer&) {
    if (enabled && this->on_click) this->on_click();
  };
}

animation::Phase RunButton::Tick(time::Timer& t) {
  if (parent) {
    // Lower center, dipping slightly past the border - aligned with where the
    // "next" connector leaves the object.
    SkRect bounds = parent->Shape().getBounds();
    local_to_parent = SkM44::Translate(bounds.centerX(), bounds.fTop + kRadius - kOverhang);
  }
  return clickable.Tick(t);
}

void RunButton::Draw(SkCanvas& canvas) const {
  bool hover = clickable.pointers_over > 0;
  bool pressed = clickable.pointers_pressing > 0;
  float glow = std::clamp(clickable.highlight, 0.f, 1.f);

  // The kit draws in pixels with +Y down; bridge from the metric canvas.
  constexpr float kPxToMetric = 7_cm / 480.f;
  canvas.save();
  canvas.scale(kPxToMetric, -kPxToMetric);
  auto PX = [&](float m) { return m / kPxToMetric; };

  constexpr uint32_t kSeed = 0x60D;
  float r = PX(kRadius);

  SkColor base = !enabled ? kGray : running ? kRed : kGreen;
  SkColor fill = pressed && enabled ? MixColor(base, kInk, 0.12f) : base;
  SkPath body = WobbleEllipse({0, 0}, r, r * 0.97f, kWonk, kSeed, 56);

  float push = pressed ? PX(0.6_mm) : 0.f;
  if (!pressed && enabled) {
    HandShadow(canvas, body, {kShadowDX, kShadowDY}, kShadow, kSeed);
  }
  canvas.save();
  canvas.translate(push, push);

  MisregFill(canvas, body, fill, kSeed);
  SketchyStroke(canvas, body, !enabled ? kInkSoft : kInk, kStroke, kSeed, 2);
  if (!enabled) {
    canvas.save();
    canvas.clipPath(body, true);
    HatchRect(canvas, body.getBounds(), kInkSoft, 8.f, Hash2(kSeed, 0x44));
    canvas.restore();
  }

  if (hover && glow > 0.02f && enabled) {
    SkPaint trace = InkPaint(kYellow, kStroke);
    trace.setAlphaf(glow);
    canvas.drawPath(WobblePath(body, kWonk, kSeg, Hash2(kSeed, 0x21u)), trace);
  }

  SkPath symbol;
  if (running) {  // stop square
    float s = r * 0.52f;
    symbol = SkPath::Rect(SkRect::MakeLTRB(-s, -s, s, s));
  } else {  // play triangle
    float tw = r * 0.62f;
    symbol = SkPathBuilder()
                 .moveTo(-tw * 0.62f, -tw)
                 .lineTo(tw, 0)
                 .lineTo(-tw * 0.62f, tw)
                 .close()
                 .detach();
  }
  symbol = WobblePath(symbol, kWonk * 0.9f, kSeg, Hash2(kSeed, 0x37u));
  FillPath(canvas, symbol, !enabled ? kGray : kPaper);
  SketchyStroke(canvas, symbol, !enabled ? kInkSoft : kInk, kStroke, Hash2(kSeed, 0x38u), 1);

  canvas.restore();
  canvas.restore();
}

}  // namespace automat::ui::slop
