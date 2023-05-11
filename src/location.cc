#include "location.h"

#include <include/effects/SkGradientShader.h>

#include "color.h"
#include "format.h"
#include "gui_constants.h"


namespace automaton {

constexpr float kFrameCornerRadius = 0.001;

SkPath Location::Shape() const {
  SkRect object_bounds;
  if (object) {
    object_bounds = object->Shape().getBounds();
  } else {
    object_bounds = SkRect::MakeEmpty();
  }
  float outset = 0.001 - kBorderWidth / 2;
  SkRect bounds = object_bounds.makeOutset(outset, outset);
  if (bounds.width() < name_text_field.width + 2 * 0.001) {
    bounds.fRight = bounds.fLeft + name_text_field.width + 2 * 0.001;
  }
  bounds.fBottom += gui::kTextFieldHeight + 0.001;
  // expand the bounds to include the run button
  SkPath run_button_shape = run_button.Shape();
  bounds.fTop -= run_button_shape.getBounds().height() + 0.001;
  return SkPath::RRect(bounds, kFrameCornerRadius, kFrameCornerRadius);
}

gui::VisitResult Location::VisitImmediateChildren(gui::WidgetVisitor &visitor) {
  if (object) {
    auto result = visitor(*object, SkMatrix::I(), SkMatrix::I());
    if (result != gui::VisitResult::kContinue) {
      return result;
    }
  }
  SkPath my_shape = Shape();
  SkRect my_bounds = my_shape.getBounds();
  SkMatrix name_text_field_transform_down =
      SkMatrix::Translate(-my_bounds.left() - 0.001,
                          -my_bounds.bottom() + gui::kTextFieldHeight + 0.001);
  SkMatrix name_text_field_transform_up =
      SkMatrix::Translate(my_bounds.left() + 0.001,
                          my_bounds.bottom() - gui::kTextFieldHeight - 0.001);
  auto result = visitor(name_text_field, name_text_field_transform_down,
                        name_text_field_transform_up);
  if (result != gui::VisitResult::kContinue) {
    return result;
  }
  SkPath run_button_shape = run_button.Shape();
  SkRect run_bounds = run_button_shape.getBounds();
  SkMatrix run_button_transform_down =
      SkMatrix::Translate(-(my_bounds.centerX() - run_bounds.centerX()),
                          -(my_bounds.top() - run_bounds.fTop) - 0.001);
  SkMatrix run_button_transform_up =
      SkMatrix::Translate(my_bounds.centerX() - run_bounds.centerX(),
                          my_bounds.top() - run_bounds.fTop + 0.001);
  result =
      visitor(run_button, run_button_transform_down, run_button_transform_up);
  return result;
}

void Location::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  SkPath my_shape = Shape();
  SkRect bounds = my_shape.getBounds();
  SkPaint frame_bg;
  SkColor frame_bg_colors[2] = {0xffcccccc, 0xffaaaaaa};
  SkPoint gradient_pts[2] = {{0, bounds.bottom()}, {0, bounds.top()}};
  sk_sp<SkShader> frame_bg_shader = SkGradientShader::MakeLinear(
      gradient_pts, frame_bg_colors, nullptr, 2, SkTileMode::kClamp);
  frame_bg.setShader(frame_bg_shader);
  canvas.drawPath(my_shape, frame_bg);

  SkPaint frame_border;
  SkColor frame_border_colors[2] = {
      color::AdjustLightness(frame_bg_colors[0], 5),
      color::AdjustLightness(frame_bg_colors[1], -5)};
  sk_sp<SkShader> frame_border_shader = SkGradientShader::MakeLinear(
      gradient_pts, frame_border_colors, nullptr, 2, SkTileMode::kClamp);
  frame_border.setShader(frame_border_shader);
  frame_border.setStyle(SkPaint::kStroke_Style);
  frame_border.setStrokeWidth(0.00025);
  canvas.drawRoundRect(bounds, kFrameCornerRadius, kFrameCornerRadius,
                       frame_border);

  auto DrawInset = [&](SkPath shape) {
    const SkRect &bounds = shape.getBounds();
    SkPaint paint;
    SkColor colors[2] = {color::AdjustLightness(frame_bg_colors[0], 5),
                         color::AdjustLightness(frame_bg_colors[1], -5)};
    SkPoint points[2] = {{0, bounds.top()}, {0, bounds.bottom()}};
    sk_sp<SkShader> shader = SkGradientShader::MakeLinear(
        points, colors, nullptr, 2, SkTileMode::kClamp);
    paint.setShader(shader);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(0.0005);
    canvas.drawPath(shape, paint);
  };

  // TODO: draw inset around every child
  if (object) {
    // Draw inset around object
    DrawInset(object->Shape());
  }

  DrawChildren(canvas, animation_state);
}

void Location::SetNumber(double number) { SetText(f("%lf", number)); }
std::string Location::LoggableString() const {
  std::string_view object_name = object->Name();
  if (name.empty()) {
    if (object_name.empty()) {
      auto &o = *object;
      return typeid(o).name();
    } else {
      return std::string(object_name);
    }
  } else {
    return f("%*s \"%s\"", object_name.size(), object_name.data(),
             name.c_str());
  }
}
void Location::ReportMissing(std::string_view property) {
  auto error_message =
      f("Couldn't find \"%*s\". You can create a connection or rename "
        "one of the nearby objects to fix this.",
        property.size(), property.data());
  ReportError(error_message);
}

} // namespace automaton
