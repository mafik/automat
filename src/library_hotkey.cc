#include "library_hotkey.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/effects/SkGradientShader.h>

#include "color.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "library_macros.hh"
#include "svg.hh"
#include "text_field.hh"

using namespace automat::gui;

namespace automat::library {

DEFINE_PROTO(HotKey);

static constexpr float kKeyLetterSize = 2.4_mm;
static constexpr float kKeyLetterSizeMM = kKeyLetterSize * 1000;

static Font& KeyFont() {
  static std::unique_ptr<Font> font = Font::Make(kKeyLetterSizeMM, 700);
  return *font.get();
}

static constexpr float kKeyHeight = kMinimalTouchableSize;
static constexpr float kKeySpareHeight = kKeyHeight - kKeyLetterSize;
static constexpr float kKeyTopSide = 0.5_mm;
static constexpr float kKeyBottomSide = 1.5_mm;
static constexpr float kKeyMargin = (kKeyHeight - kKeyTopSide - kKeyBottomSide) / 2;
static constexpr float kKeySide = 1_mm;

static constexpr float kKeyFaceRadius = 1_mm;
static constexpr float kKeyBaseRadius = kKeyFaceRadius;
static constexpr float kKeyFaceHeight = kKeyHeight - kKeyTopSide - kKeyBottomSide;

static constexpr float kRegularKeyWidth = kKeyHeight;
static constexpr float kCtrlKeyWidth = kRegularKeyWidth * 1.5;
static constexpr float kSuperKeyWidth = kCtrlKeyWidth;
static constexpr float kAltKeyWidth = kCtrlKeyWidth;
static constexpr float kShiftKeyWidth = kRegularKeyWidth * 2.25;

static constexpr float kKeySpacing = kMargin;

static constexpr float kFrameWidth = kBorderWidth * 2 + kMargin;
static constexpr float kFrameInnerRadius = kKeyBaseRadius + kKeySpacing;
static constexpr float kFrameOuterRadius = kFrameInnerRadius + kFrameWidth;

static constexpr float kTopRowWidth = kFrameWidth + kKeySpacing + kShiftKeyWidth + kKeySpacing +
                                      kRegularKeyWidth + kKeySpacing + kFrameWidth;
static constexpr float kBottomRowWidth = kFrameWidth + kKeySpacing + kCtrlKeyWidth + kKeySpacing +
                                         kSuperKeyWidth + kKeySpacing + kAltKeyWidth + kKeySpacing +
                                         kFrameWidth;

static constexpr float kWidth = std::max(kTopRowWidth, kBottomRowWidth);
static constexpr float kHeight = kFrameWidth * 2 + kKeyHeight * 2 + kKeySpacing * 3;
static constexpr SkRect kShapeRect = SkRect::MakeXYWH(-kWidth / 2, -kHeight / 2, kWidth, kHeight);
static const SkRRect kShapeRRect = [] {
  SkRRect ret;
  float top_right_radius = kFrameWidth + kMinimalTouchableSize / 2 - kBorderWidth;
  SkVector radii[4] = {
      {kFrameOuterRadius, kFrameOuterRadius},
      {kFrameOuterRadius, kFrameOuterRadius},
      {top_right_radius, top_right_radius},
      {kFrameOuterRadius, kFrameOuterRadius},
  };
  ret.setRectRadii(kShapeRect, radii);
  return ret;
}();

PowerButton::PowerButton(Object* object)
    : ToggleButton(MakeShapeWidget(kPowerSVG, SK_ColorWHITE), "#fa2305"_color), object(object) {}

void PowerButton::Activate(gui::Pointer&) {}

bool PowerButton::Filled() const { return true; }

HotKey::HotKey() : power_button(this) {}
string_view HotKey::Name() const { return "HotKey"; }
std::unique_ptr<Object> HotKey::Clone() const {
  auto ret = std::make_unique<HotKey>();
  ret->key = key;
  ret->ctrl = ctrl;
  ret->alt = alt;
  ret->shift = shift;
  ret->windows = windows;
  return ret;
}

static sk_sp<SkShader> MakeSweepShader(float rect_right, float rect_top, float rect_radius,
                                       float center_offset_y, SkColor side_color, SkColor top_color,
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
  float pos[] = {
      0,
      (float)(atan2(-rect_top + rect_radius + center_offset_y, -rect_right) / (2 * M_PI) + 0.5),
      (float)(atan2(-rect_top + center_offset_y, -(rect_right - rect_radius)) / (2 * M_PI) + 0.5),
      0.25,
      (float)(atan2(-rect_top + center_offset_y, rect_right - rect_radius) / (2 * M_PI) + 0.5),
      (float)(atan2(-rect_top + rect_radius + center_offset_y, rect_right) / (2 * M_PI) + 0.5),
      0.5,
      (float)(atan2(rect_top - rect_radius + center_offset_y, rect_right) / (2 * M_PI) + 0.5),
      (float)(atan2(rect_top + center_offset_y, rect_right - rect_radius) / (2 * M_PI) + 0.5),
      0.75,
      (float)(atan2(rect_top + center_offset_y, -(rect_right - rect_radius)) / (2 * M_PI) + 0.5),
      (float)(atan2(rect_top - rect_radius + center_offset_y, -rect_right) / (2 * M_PI) + 0.5),
      1};
  return SkGradientShader::MakeSweep(0, center_offset_y, colors, pos, 13);
}

static void DrawKey(SkCanvas& canvas, bool enabled, float width, std::function<void()> cb) {
  float correction = (kKeyFaceHeight - kKeyHeight) / 2 + kKeyTopSide;
  canvas.translate(0, -correction);
  SkRect key_base_box =
      SkRect::MakeXYWH(-width / 2, -kKeyHeight / 2, width, kKeyHeight).makeOffset(0, correction);
  SkRRect key_base = SkRRect::MakeRectXY(key_base_box, kKeyBaseRadius, kKeyBaseRadius);
  SkRect key_face_box = SkRect::MakeXYWH(-width / 2 + kKeySide, -kKeyFaceHeight / 2,
                                         width - 2 * kKeySide, kKeyFaceHeight);
  SkRRect key_face = SkRRect::MakeRectXY(key_face_box, kKeyFaceRadius, kKeyFaceRadius);

  SkColor base_color = enabled ? "#f3a75b"_color : "#f4efea"_color;

  SkPaint face_paint;
  SkPoint face_pts[] = {{0, key_face_box.bottom()}, {0, key_face_box.top()}};
  SkColor face_colors[] = {color::AdjustLightness(base_color, -10), base_color};
  face_paint.setAntiAlias(true);
  face_paint.setShader(
      SkGradientShader::MakeLinear(face_pts, face_colors, nullptr, 2, SkTileMode::kClamp));

  face_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  face_paint.setStrokeWidth(0.5_mm);

  canvas.drawRRect(key_face, face_paint);

  SkColor top_color = color::AdjustLightness(base_color, 20);
  SkColor side_color = color::AdjustLightness(base_color, -20);
  SkColor side_color2 = color::AdjustLightness(base_color, -25);
  SkColor bottom_color = color::AdjustLightness(base_color, -50);

  SkPaint side_paint;
  side_paint.setAntiAlias(true);
  side_paint.setShader(MakeSweepShader(key_face_box.fRight, key_face_box.fBottom, kKeyFaceRadius,
                                       1.5_mm, side_color, top_color, top_color, side_color,
                                       side_color2, bottom_color, bottom_color));
  canvas.drawDRRect(key_base, key_face, side_paint);
  canvas.translate(key_face_box.centerX(), key_face_box.centerY());
  cb();
  canvas.translate(-key_face_box.centerX(), -key_face_box.centerY());
  canvas.translate(0, correction);
}

static void DrawCenteredText(SkCanvas& canvas, const char* text) {
  SkPaint text_paint;
  text_paint.setAntiAlias(true);
  text_paint.setColor("#000000"_color);
  static auto& font = KeyFont();
  float w = font.MeasureText(text);
  canvas.translate(-w / 2, -kKeyLetterSize / 2);
  font.DrawText(canvas, text, text_paint);
  canvas.translate(w / 2, kKeyLetterSize / 2);
}

void HotKey::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;

  SkRRect frame_outer;
  SkRRect frame_inner;
  SkRRect frame_inner2;
  kShapeRRect.inset(kBorderWidth, kBorderWidth, &frame_outer);
  frame_outer.inset(kMargin, kMargin, &frame_inner);
  frame_inner.inset(kBorderWidth, kBorderWidth, &frame_inner2);

  SkPath inner_contour;
  inner_contour.moveTo(frame_inner2.rect().left() + kFrameInnerRadius,
                       frame_inner2.rect().bottom());
  inner_contour.arcTo(
      {frame_inner2.rect().left(), frame_inner2.rect().bottom()},
      {frame_inner2.rect().left(), frame_inner2.rect().bottom() - kFrameInnerRadius},
      kFrameInnerRadius);
  inner_contour.lineTo(frame_inner2.rect().left(), frame_inner2.rect().top() + kFrameInnerRadius);
  inner_contour.arcTo({frame_inner2.rect().left(), frame_inner2.rect().top()},
                      {frame_inner2.rect().left() + kFrameInnerRadius, frame_inner2.rect().top()},
                      kFrameInnerRadius);
  inner_contour.lineTo(frame_inner2.rect().right() - kFrameInnerRadius, frame_inner2.rect().top());
  inner_contour.arcTo({frame_inner2.rect().right(), frame_inner2.rect().top()},
                      {frame_inner2.rect().right(), frame_inner2.rect().top() + kFrameInnerRadius},
                      kFrameInnerRadius);
  float edge_start_y = frame_inner2.rect().top() + kKeySpacing + kKeyHeight - kKeyBaseRadius;
  float edge_y = edge_start_y + kFrameInnerRadius;
  inner_contour.lineTo(frame_inner2.rect().right(), edge_start_y);
  inner_contour.arcTo({frame_inner2.rect().right(), edge_y},
                      {frame_inner2.rect().right() - kFrameInnerRadius, edge_y}, kFrameInnerRadius);
  float edge_x = frame_inner2.rect().right() - kMinimalTouchableSize - kKeySpacing;
  float edge_start_x = edge_x + kMinimalTouchableSize / 2 + kKeySpacing;
  float edge_r = kMinimalTouchableSize / 2 + kKeySpacing;
  inner_contour.lineTo(edge_start_x, edge_y);
  inner_contour.arcTo({edge_x, edge_y}, {edge_x, edge_y + edge_r}, edge_r);
  float edge_r2 = frame_inner2.rect().bottom() - edge_y - edge_r;
  inner_contour.arcTo({edge_x, frame_inner2.rect().bottom()},
                      {edge_x - edge_r2, frame_inner2.rect().bottom()}, edge_r2);
  inner_contour.close();

  // Draw background
  SkPaint inner_paint;
  inner_paint.setColor("#000000"_color);
  inner_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  inner_paint.setStrokeWidth(0.5_mm);
  canvas.drawPath(inner_contour, inner_paint);

  // Frame shadow
  SkPaint background_shadow_paint;
  background_shadow_paint.setMaskFilter(
      SkMaskFilter::MakeBlur(SkBlurStyle::kInner_SkBlurStyle, 0.0005, true));
  background_shadow_paint.setColor("#333333"_color);
  canvas.drawPath(inner_contour, background_shadow_paint);

  // Draw frame

  SkRect gradient_rect = kShapeRect.makeInset(kFrameWidth / 2, kFrameWidth / 2);
  float gradient_r = kFrameInnerRadius;

  SkPaint border_paint;
  border_paint.setAntiAlias(true);
  SkPoint border_pts[] = {{0, kShapeRect.bottom()}, {0, kShapeRect.top()}};
  SkColor border_colors[] = {"#f0f0f0"_color, "#cccccc"_color};
  border_paint.setShader(
      SkGradientShader::MakeLinear(border_pts, border_colors, nullptr, 2, SkTileMode::kClamp));

  SkPath border_path;
  border_path.addRRect(kShapeRRect);
  border_path.addPath(inner_contour);
  border_path.setFillType(SkPathFillType::kEvenOdd);
  canvas.drawPath(border_path, border_paint);
  // canvas.drawDRRect(kShapeRRect, frame_inner2, border_paint);

  SkBlendMode shade_blend_mode = SkBlendMode::kHardLight;
  float shade_alpha = 0.5;
  SkPoint light_pts[] = {{0, kShapeRect.bottom()}, {0, kShapeRect.top()}};
  SkColor light_colors[] = {"#fdf8e0"_color, "#111c22"_color};
  SkPaint light_paint;
  light_paint.setAntiAlias(true);
  light_paint.setBlendMode(shade_blend_mode);
  light_paint.setAlphaf(shade_alpha);
  light_paint.setShader(
      SkGradientShader::MakeLinear(light_pts, light_colors, nullptr, 2, SkTileMode::kClamp));

  canvas.drawDRRect(kShapeRRect, frame_outer, light_paint);

  SkPoint shadow_pts[] = {{0, kShapeRect.top() + kFrameOuterRadius}, {0, kShapeRect.top()}};
  SkColor shadow_colors[] = {"#111c22"_color, "#fdf8e0"_color};
  SkPaint shadow_paint;
  shadow_paint.setAntiAlias(true);
  shadow_paint.setBlendMode(shade_blend_mode);
  shadow_paint.setAlphaf(shade_alpha);
  shadow_paint.setStyle(SkPaint::kStroke_Style);
  shadow_paint.setStrokeWidth(kBorderWidth * 2);
  shadow_paint.setShader(
      SkGradientShader::MakeLinear(shadow_pts, shadow_colors, nullptr, 2, SkTileMode::kClamp));
  canvas.save();
  canvas.clipPath(border_path, true);
  canvas.drawPath(inner_contour, shadow_paint);
  canvas.restore();
  // canvas.drawDRRect(frame_inner, frame_inner2, shadow_paint);

  // Draw keys
  canvas.save();
  // Ctrl
  canvas.translate(-kWidth / 2 + kFrameWidth + kKeySpacing + kCtrlKeyWidth / 2,
                   -kHeight / 2 + kFrameWidth + kKeySpacing + kKeyHeight / 2);
  DrawKey(canvas, ctrl, kCtrlKeyWidth, [&]() { DrawCenteredText(canvas, "Ctrl"); });

  // Super
  canvas.translate(kCtrlKeyWidth / 2 + kKeySpacing + kSuperKeyWidth / 2, 0);
  DrawKey(canvas, windows, kSuperKeyWidth, [&]() { DrawCenteredText(canvas, "Super"); });
  // Alt
  canvas.translate(kSuperKeyWidth / 2 + kKeySpacing + kAltKeyWidth / 2, 0);
  DrawKey(canvas, alt, kAltKeyWidth, [&]() { DrawCenteredText(canvas, "Alt"); });
  canvas.restore();
  canvas.save();
  // Shift
  canvas.translate(-kWidth / 2 + kFrameWidth + kKeySpacing + kShiftKeyWidth / 2,
                   kHeight / 2 - kFrameWidth - kKeySpacing - kKeyHeight / 2);
  DrawKey(canvas, shift, kShiftKeyWidth, [&]() { DrawCenteredText(canvas, "Shift"); });
  // Shortcut
  canvas.translate(kShiftKeyWidth / 2 + kKeySpacing + kRegularKeyWidth / 2, 0);
  DrawKey(canvas, true, kRegularKeyWidth, [&]() { DrawCenteredText(canvas, "F5"); });
  canvas.restore();

  DrawChildren(ctx);
}

SkPath HotKey::Shape() const { return SkPath::RRect(kShapeRRect); }
std::unique_ptr<Action> HotKey::ButtonDownAction(gui::Pointer&, gui::PointerButton) {
  return nullptr;
}
void HotKey::Args(std::function<void(Argument&)> cb) {}
void HotKey::Run(Location&) {}

ControlFlow HotKey::VisitChildren(gui::Visitor& visitor) {
  if (visitor(power_button) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}

SkMatrix HotKey::TransformToChild(const Widget& child, animation::Context&) const {
  if (&child == &power_button) {
    return SkMatrix::Translate(-kWidth / 2 + kFrameWidth + kMinimalTouchableSize - kBorderWidth,
                               -kHeight / 2 + kFrameWidth + kMinimalTouchableSize - kBorderWidth);
  }
  return SkMatrix::I();
}

}  // namespace automat::library