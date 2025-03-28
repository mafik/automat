// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "object.hh"

#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "../build/generated/embedded.hh"
#include "base.hh"
#include "drag_action.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "location.hh"
#include "root_widget.hh"

using namespace maf;

namespace automat {

std::string_view Object::FallbackWidget::Name() const {
  StrView name;
  if (auto obj = object.lock()) {
    name = obj->Name();
  } else {
    name = Widget::Name();
  }
  return name;
}

void Object::FallbackWidget::Draw(SkCanvas& canvas) const {
  SkPath path = Shape();

  SkPaint paint;
  SkPoint pts[2] = {{0, 0}, {0, 0.01}};
  SkColor colors[2] = {0xff0f5f4d, 0xff468257};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  canvas.drawPath(path, paint);

  SkPaint border_paint;
  border_paint.setStroke(true);
  border_paint.setStrokeWidth(0.00025);

  SkRRect rrect;
  if (path.isRRect(&rrect)) {
    float inset = border_paint.getStrokeWidth() / 2;
    rrect.inset(inset, inset);
    path = SkPath::RRect(rrect);
  }

  SkColor border_colors[2] = {0xff1c5d3e, 0xff76a87a};
  sk_sp<SkShader> border_gradient =
      SkGradientShader::MakeLinear(pts, border_colors, nullptr, 2, SkTileMode::kClamp);
  border_paint.setShader(border_gradient);

  canvas.drawPath(path, border_paint);

  SkPaint text_paint;
  text_paint.setColor(SK_ColorWHITE);

  SkRect path_bounds = path.getBounds();

  auto text = Text();
  canvas.save();
  canvas.translate(path_bounds.width() / 2 - gui::GetFont().MeasureText(text) / 2,
                   path_bounds.height() / 2 - gui::kLetterSizeMM / 2 / 1000);
  gui::GetFont().DrawText(canvas, text, text_paint);
  canvas.restore();
}

float Object::FallbackWidget::Width() const {
  auto text = Text();
  constexpr float kNameMargin = 0.001;
  float width_text = gui::GetFont().MeasureText(text) + 2 * kNameMargin;
  float width_rounded = ceil(width_text * 1000) / 1000;
  constexpr float kMinWidth = 0.008;
  return std::max(width_rounded, kMinWidth);
}

SkPath Object::FallbackWidget::Shape() const {
  static std::unordered_map<float, SkPath> basic_shapes;
  float width = Width();
  auto it = basic_shapes.find(width);
  if (it == basic_shapes.end()) {
    SkRect rect = SkRect::MakeXYWH(0, 0, width, 0.008);
    SkRRect rrect = SkRRect::MakeRectXY(rect, 0.001, 0.001);
    it = basic_shapes.emplace(std::make_pair(width, SkPath::RRect(rrect))).first;
  }
  return it->second;
}

std::unique_ptr<Action> Object::FallbackWidget::FindAction(gui::Pointer& p,
                                                           gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto* location = Closest<Location>(*p.hover);
    auto* machine = Closest<Machine>(*p.hover);
    if (location && machine) {
      auto contact_point = p.PositionWithin(*this);
      auto a = std::make_unique<DragLocationAction>(p, machine->Extract(*location), contact_point);
      return a;
    }
  }
  return Widget::FindAction(p, btn);
}

void Object::Updated(Location& here, Location& updated) {
  if (Runnable* runnable = dynamic_cast<Runnable*>(this)) {
    runnable->Run(here);
  }
}

void Object::SerializeState(Serializer& writer, const char* key) const {
  auto value = GetText();
  if (!value.empty()) {
    writer.Key(key);
    writer.String(value.data(), value.size());
  }
}

void Object::DeserializeState(Location& l, Deserializer& d) {
  maf::Status status;
  Str value;
  d.Get(value, status);
  if (!OK(status)) {
    l.ReportError(status.ToStr());
    return;
  }
  SetText(l, value);
}

audio::Sound& Object::NextSound() { return embedded::assets_SFX_next_wav; }

void Object::WakeWidgetsAnimation() {
  auto weak = WeakPtr();
  for (auto* root_widget : gui::root_widgets) {
    auto it = root_widget->widgets.container.find(weak);
    if (it != root_widget->widgets.container.end()) {
      if (auto widget = it->second.lock()) {
        widget->WakeAnimation();
      }
    }
  }
}

}  // namespace automat
