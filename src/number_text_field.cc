// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "number_text_field.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/effects/SkGradientShader.h>

#include "font.hh"
#include "gui_constants.hh"
#include "text_field.hh"

namespace automat::gui {

NumberTextField::NumberTextField(Widget& parent, float width)
    : gui::TextField(parent, &text, width), text("0") {}

SkRRect NumberTextField::ShapeRRect() const {
  return SkRRect::MakeRectXY(SkRect::MakeXYWH(0, 0, width, kHeight), kHeight / 2, kHeight / 2);
}

// First bottom then top
static constexpr SkColor kNumberBackgroundColors[2] = {"#bec8b7"_color, "#dee3db"_color};

static const SkPaint kNumberTextBackgroundPaint = []() {
  SkPoint pts[2] = {{0, 0}, {0, TextField::kHeight}};
  sk_sp<SkShader> shader =
      SkGradientShader::MakeLinear(pts, kNumberBackgroundColors, nullptr, 2, SkTileMode::kClamp);
  SkPaint paint;
  paint.setShader(shader);
  paint.setAntiAlias(true);
  return paint;
}();

const SkPaint& NumberTextField::GetBackgroundPaint() const { return kNumberTextBackgroundPaint; }

static const SkPaint kNumberTextBorderPaint = []() {
  SkPaint paint;
  SkPoint pts[2] = {{0, 0}, {0, TextField::kHeight}};
  SkColor colors[2] = {0xffffffff, 0xff000000};
  sk_sp<SkShader> shader =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(shader);
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(kBorderWidth);
  paint.setBlendMode(SkBlendMode::kOverlay);
  return paint;
}();

void NumberTextField::DrawBackground(SkCanvas& canvas, SkRRect rrect) {
  rrect.inset(kBorderWidth, kBorderWidth);
  SkPoint pts[2] = {{0, rrect.getBounds().top()}, {0, rrect.getBounds().bottom()}};
  sk_sp<SkShader> shader =
      SkGradientShader::MakeLinear(pts, kNumberBackgroundColors, nullptr, 2, SkTileMode::kClamp);
  SkPaint paint;
  paint.setShader(shader);
  paint.setAntiAlias(true);

  canvas.save();
  canvas.clipRRect(rrect);
  canvas.drawPaint(paint);

  // Inner shadows
  SkPath path = SkPath::RRect(rrect);
  path.toggleInverseFillType();
  SkPaint shadow_paint;
  shadow_paint.setColor("#86a174"_color);
  shadow_paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, 0.5_mm));
  shadow_paint.setBlendMode(SkBlendMode::kColorBurn);
  canvas.drawPath(path, shadow_paint);

  canvas.restore();

  rrect.outset(kBorderWidth / 2, kBorderWidth / 2);
  canvas.drawRRect(rrect, kNumberTextBorderPaint);
}

void NumberTextField::DrawBackground(SkCanvas& canvas) const {
  SkRRect rrect = ShapeRRect();
  DrawBackground(canvas, rrect);
}

void NumberTextField::DrawText(SkCanvas& canvas) const {
  gui::Font& font = gui::GetFont();
  Vec2 text_pos = GetTextPos();
  canvas.translate(text_pos.x, text_pos.y);
  font.DrawText(canvas, text, GetTextPaint());
}

Vec2 NumberTextField::GetTextPos() const {
  gui::Font& font = gui::GetFont();
  // We use the same margin on all sides because it looks nicer with fully rounded corners.
  float margin = (kHeight - gui::kLetterSize) / 2;
  float text_width = font.MeasureText(text);
  return Vec2(width - text_width - margin, margin);
}

static Str FormatNumber(double x, int max_digits = 5) {
  Str ret;
  if (x < 0) {
    return "-" + FormatNumber(-x, max_digits - 1);
  }
  double upper_limit = pow(10, max_digits);
  if (x >= upper_limit) {
    return Str(max_digits, '9');
  }
  double lower_limit = pow(10, -max_digits);
  if (x < lower_limit) {
    return "." + Str(max_digits, '0');
  }
  int exp = 0;
  while (x >= 10) {
    x /= 10;
    exp++;
  }
  while (x > 0 && x < 1) {
    x *= 10;
    exp--;
  }
  int dot_index = exp + 1;
  while (dot_index < 0 && ret.size() < max_digits) {
    ret += '0';
    dot_index++;
  }
  while (ret.size() < max_digits) {
    int digit = x;
    ret += '0' + digit;
    x -= digit;
    x *= 10;
  }
  if (x >= 5) {
    for (auto it = ret.rbegin(); it != ret.rend(); ++it) {
      if (*it == '9') {
        *it = '0';
      } else {
        (*it)++;
        break;
      }
    }
  }
  while (!ret.empty() && ret.back() == '0' && dot_index < (int)ret.size()) {
    ret.pop_back();
  }
  if (dot_index < 0) {
    ret = '.' + Str(-dot_index, '0') + ret;
  } else if (dot_index < ret.size()) {
    ret.insert(dot_index, ".");
  }
  return ret;
}

void NumberTextField::SetNumber(double x) {
  text = FormatNumber(x, 5);
  WakeAnimation();
}

}  // namespace automat::gui