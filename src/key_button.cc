#include "key_button.hh"

#include <include/effects/SkGradientShader.h>

#include "font.hh"
#include "gui_button.hh"
#include "log.hh"
#include "widget.hh"

using namespace maf;
using namespace std;

namespace automat::library {

KeyButton::KeyButton(std::unique_ptr<Widget>&& child, SkColor color, float width)
    : Button(std::move(child)), width(width), fg(color) {}

void KeyButton::Activate(gui::Pointer& pointer) {
  if (activate) {
    activate(pointer);
  }
}

SkMatrix KeyButton::TransformToChild(const Widget& child, animation::Display*) const {
  SkRect child_bounds = ChildBounds();
  SkRRect key_base = RRect();
  SkRect key_face =
      SkRect::MakeLTRB(key_base.rect().left() + kKeySide, key_base.rect().top() + kKeyBottomSide,
                       key_base.rect().right() - kKeySide, key_base.rect().bottom() - kKeyTopSide);
  return SkMatrix::Translate(child_bounds.center() - key_face.center());
}

SkRRect KeyButton::RRect() const {
  return SkRRect::MakeRectXY(SkRect::MakeWH(width, kKeyHeight), kKeyBaseRadius, kKeyBaseRadius);
}

static sk_sp<SkShader> MakeSweepShader(const RRect& rrect, SkColor side_color, SkColor top_color,
                                       SkColor top_corner_top, SkColor top_corner_side,
                                       SkColor bottom_corner_side, SkColor bottom_corner_bottom,
                                       SkColor bottom_color) {
  SkColor colors[] = {
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
  return SkGradientShader::MakeSweep(center.x, center.y, colors, pos, 13);
}

void KeyButton::DrawButtonFace(gui::DrawContext& ctx, SkColor bg, SkColor fg) const {
  auto& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& animation_state = animation_state_ptr[display];
  bool enabled = false;

  SkRRect key_base = RRect();
  float press_shift_y = PressRatio() * -kPressOffset;
  key_base.offset(0, press_shift_y);

  SkRRect key_face = SkRRect::MakeRectXY(
      SkRect::MakeLTRB(key_base.rect().left() + kKeySide, key_base.rect().top() + kKeyBottomSide,
                       key_base.rect().right() - kKeySide, key_base.rect().bottom() - kKeyTopSide),
      kKeyFaceRadius, kKeyFaceRadius);

  float lightness_adjust = animation_state.highlight * 10;

  SkPaint face_paint;
  SkPoint face_pts[] = {{0, key_face.rect().bottom()}, {0, key_face.rect().top()}};
  SkColor face_colors[] = {color::AdjustLightness(fg, -10 + lightness_adjust),
                           color::AdjustLightness(fg, lightness_adjust)};
  face_paint.setShader(
      SkGradientShader::MakeLinear(face_pts, face_colors, nullptr, 2, SkTileMode::kClamp));

  face_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  face_paint.setStrokeWidth(0.5_mm);

  canvas.drawRRect(key_face, face_paint);

  SkColor top_color = color::AdjustLightness(fg, 20 + lightness_adjust);
  SkColor side_color = color::AdjustLightness(fg, -20 + lightness_adjust);
  SkColor side_color2 = color::AdjustLightness(fg, -25 + lightness_adjust);
  SkColor bottom_color = color::AdjustLightness(fg, -50 + lightness_adjust);

  SkPaint side_paint;
  side_paint.setAntiAlias(true);
  side_paint.setShader(MakeSweepShader(*reinterpret_cast<union RRect*>(&key_face), side_color,
                                       top_color, top_color, side_color, side_color2, bottom_color,
                                       bottom_color));
  canvas.drawDRRect(key_base, key_face, side_paint);
}

Font& KeyFont() {
  static std::unique_ptr<Font> font = Font::Make(kKeyLetterSizeMM, 700);
  return *font.get();
}

struct KeyLabelWidget : Widget, LabelMixin {
  Str label;
  float width;

  KeyLabelWidget(StrView label) { SetLabel(label); }
  SkPath Shape(animation::Display*) const override {
    return SkPath::Rect(SkRect::MakeXYWH(-width / 2, -kKeyLetterSize / 2, width, kKeyLetterSize));
  }
  maf::Optional<Rect> TextureBounds() const override { return nullopt; }
  animation::Phase Draw(DrawContext& ctx) const override {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor("#000000"_color);
    ctx.canvas.translate(-width / 2, -kKeyLetterSize / 2);
    KeyFont().DrawText(ctx.canvas, label, paint);
    ctx.canvas.translate(width / 2, kKeyLetterSize / 2);
    return animation::Finished;
  }
  void SetLabel(StrView label) override {
    this->label = label;
    width = KeyFont().MeasureText(label);
    LOG << "KeyLabelWidget::SetLabel: " << label << " width: " << width;
    InvalidateDrawCache();
  }
};

std::unique_ptr<Widget> MakeKeyLabelWidget(StrView label) {
  return std::make_unique<KeyLabelWidget>(label);
}

void KeyButton::SetLabel(maf::StrView new_label) {
  dynamic_cast<LabelMixin*>(child.get())->SetLabel(new_label);
}

}  // namespace automat::library