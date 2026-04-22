// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "key_button.hh"

#include <include/core/SkShader.h>
#include <include/effects/SkGradient.h>

#include "font.hh"
#include "ui_button.hh"
#include "ui_constants.hh"
#include "widget.hh"

using namespace std;
using namespace automat::ui;

namespace automat::library {

struct KeyLabelWidget : Widget, LabelMixin {
  Str label;
  float width;

  KeyLabelWidget(Widget* parent, StrView label) : Widget(parent) { SetLabel(label); }
  SkPath Shape() const override {
    return SkPath::Rect(SkRect::MakeXYWH(-width / 2, -kKeyLetterSize / 2, width, kKeyLetterSize));
  }
  Optional<Rect> TextureBounds() const override {
    return SkRect::MakeLTRB(-width / 2, 1.5 * kLetterSize, width / 2, -0.5 * kLetterSize);
  }
  void Draw(SkCanvas& canvas) const override {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor("#000000"_color);
    canvas.translate(-width / 2, -kKeyLetterSize / 2);
    KeyFont().DrawText(canvas, label, paint);
    canvas.translate(width / 2, kKeyLetterSize / 2);
  }
  void SetLabel(StrView label) override {
    this->label = label;
    width = KeyFont().MeasureText(label);
    WakeAnimation();
  }
};

KeyButton::KeyButton(Widget* parent, StrView label, SkColor4f color, float width)
    : Button(parent), width(width), fg(color) {
  child = make_unique<KeyLabelWidget>(this, label);
  SetLabel(label);
}

void KeyButton::Activate(ui::Pointer& pointer) {
  if (activate) {
    activate(pointer);
  }
}

SkRRect KeyButton::RRect() const {
  return SkRRect::MakeRectXY(SkRect::MakeWH(width, kKeyHeight), kKeyBaseRadius, kKeyBaseRadius);
}

static sk_sp<SkShader> MakeSweepShader(const RRect& rrect, SkColor4f side_color,
                                       SkColor4f top_color, SkColor4f top_corner_top,
                                       SkColor4f top_corner_side, SkColor4f bottom_corner_side,
                                       SkColor4f bottom_corner_bottom, SkColor4f bottom_color) {
  SkColor4f colors[] = {
      side_color,            // right middle
      top_corner_side,       // bottom of top-right corner
      top_corner_top,        // top of the top-right corner
      top_color,             // center top
      top_corner_top,        // top of the top-left corner
      top_corner_side,       // bottom of the top-left corner
      side_color,            // left middle
      bottom_corner_side,    // top of the bottom-left corner
      bottom_corner_bottom,  // bottom of the bottom-left corner
      bottom_color,          // center bottom
      bottom_corner_bottom,  // bottom of the bottom-right corner
      bottom_corner_side,    // top of the bottom-right corner
      side_color,            // right middle
  };
  auto center = rrect.Center();
  float pos[] = {0,
                 (float)(atan(rrect.LineEndRightUpper() - center) / (2 * M_PI)),
                 (float)(atan(rrect.LineEndUpperRight() - center) / (2 * M_PI)),
                 0.25,
                 (float)(atan(rrect.LineEndUpperLeft() - center) / (2 * M_PI)),
                 (float)(atan(rrect.LineEndLeftUpper() - center) / (2 * M_PI)),
                 0.5,
                 (float)(atan(rrect.LineEndLeftLower() - center) / (2 * M_PI) + 1),
                 (float)(atan(rrect.LineEndLowerLeft() - center) / (2 * M_PI) + 1),
                 0.75,
                 (float)(atan(rrect.LineEndLowerRight() - center) / (2 * M_PI) + 1),
                 (float)(atan(rrect.LineEndRightLower() - center) / (2 * M_PI) + 1),
                 1};
  return SkShaders::SweepGradient(
      SkPoint::Make(center.x, center.y),
      SkGradient{SkGradient::Colors{colors, pos, SkTileMode::kClamp}, {}});
}

void KeyButton::DrawButtonFace(SkCanvas& canvas, SkColor4f bg, SkColor4f fg) const {
  bool enabled = false;

  SkRRect key_base = RRect();
  float press_shift_y = PressRatio() * -kPressOffset;
  key_base.offset(0, press_shift_y);

  SkRRect key_face = SkRRect::MakeRectXY(
      SkRect::MakeLTRB(key_base.rect().left() + kKeySide, key_base.rect().top() + kKeyBottomSide,
                       key_base.rect().right() - kKeySide, key_base.rect().bottom() - kKeyTopSide),
      kKeyFaceRadius, kKeyFaceRadius);

  float lightness_adjust = clickable.highlight * 10;

  SkPaint face_paint;
  SkPoint face_pts[] = {{0, key_face.rect().bottom()}, {0, key_face.rect().top()}};
  SkColor4f face_colors[] = {color::AdjustLightness(fg, -10 + lightness_adjust),
                             color::AdjustLightness(fg, lightness_adjust)};
  face_paint.setShader(SkShaders::LinearGradient(
      face_pts, SkGradient{SkGradient::Colors{face_colors, SkTileMode::kClamp}, {}}));

  face_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  face_paint.setStrokeWidth(0.5_mm);

  canvas.drawRRect(key_face, face_paint);

  SkColor4f top_color = color::AdjustLightness(fg, 20 + lightness_adjust);
  SkColor4f side_color = color::AdjustLightness(fg, -20 + lightness_adjust);
  SkColor4f side_color2 = color::AdjustLightness(fg, -25 + lightness_adjust);
  SkColor4f bottom_color = color::AdjustLightness(fg, -50 + lightness_adjust);

  SkPaint side_paint;
  side_paint.setAntiAlias(true);
  side_paint.setShader(MakeSweepShader(*reinterpret_cast<union RRect*>(&key_face), side_color,
                                       top_color, top_color, side_color, side_color2, bottom_color,
                                       bottom_color));
  canvas.drawDRRect(key_base, key_face, side_paint);
}

Font& KeyFont() {
  static std::unique_ptr<Font> font =
      Font::MakeV2(Font::MakeWeightVariation(Font::GetNotoSans(), 700), kKeyLetterSize);
  return *font.get();
}

void KeyButton::SetLabel(StrView new_label) {
  KeyLabelWidget* label_widget = static_cast<KeyLabelWidget*>(child.get());
  label_widget->SetLabel(new_label);
  SkRect child_bounds = ChildBounds();
  SkRRect key_base = RRect();
  SkRect key_face =
      SkRect::MakeLTRB(key_base.rect().left() + kKeySide, key_base.rect().top() + kKeyBottomSide,
                       key_base.rect().right() - kKeySide, key_base.rect().bottom() - kKeyTopSide);
  auto offset = key_face.center() - child_bounds.center();
  float scale_x = min(1.0f, key_face.width() / label_widget->width);
  float scale_y = min(1.0f, scale_x * 2);
  label_widget->local_to_parent =
      SkM44::Translate(offset.x(), offset.y()).preScale(scale_x, scale_y);
}

}  // namespace automat::library
