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
#include "sincos.hh"

using namespace maf;

namespace automat {

animation::Phase Object::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
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

  canvas.save();
  canvas.translate(path_bounds.width() / 2 - gui::GetFont().MeasureText(Name()) / 2,
                   path_bounds.height() / 2 - gui::kLetterSizeMM / 2 / 1000);
  gui::GetFont().DrawText(canvas, Name(), text_paint);
  canvas.restore();
  return animation::Finished;
}

SkPath Object::Shape() const {
  static std::unordered_map<std::string_view, SkPath> basic_shapes;
  auto it = basic_shapes.find(Name());
  if (it == basic_shapes.end()) {
    constexpr float kNameMargin = 0.001;
    float width_name = gui::GetFont().MeasureText(Name()) + 2 * kNameMargin;
    float width_rounded = ceil(width_name * 1000) / 1000;
    constexpr float kMinWidth = 0.008;
    float final_width = std::max(width_rounded, kMinWidth);
    SkRect rect = SkRect::MakeXYWH(0, 0, final_width, 0.008);
    SkRRect rrect = SkRRect::MakeRectXY(rect, 0.001, 0.001);
    it = basic_shapes.emplace(std::make_pair(Name(), SkPath::RRect(rrect))).first;
  }
  return it->second;
}

std::unique_ptr<Action> Object::FindAction(gui::Pointer& p, gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto* location = Closest<Location>(p.hover);
    auto* machine = Closest<Machine>(p.hover);
    if (location && machine) {
      auto a = std::make_unique<DragLocationAction>(p, machine->Extract(*location));
      a->contact_point = p.PositionWithin(*location);
      return a;
    }
  }
  return nullptr;
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

Vec2AndDir Object::ArgStart(const Argument& arg) {
  SkPath shape;
  if (arg.field) {
    shape = FieldShape(*arg.field);
  }
  if (shape.isEmpty()) {
    shape = Shape();
  }
  Rect bounds = shape.getBounds();
  return Vec2AndDir{
      .pos = bounds.BottomCenter(),
      .dir = -90_deg,
  };
}

void Object::ConnectionPositions(maf::Vec<Vec2AndDir>& out_positions) const {
  // By default just one position on the top of the bounding box.
  auto shape = Shape();
  Rect bounds = shape.getBounds();
  out_positions.push_back(Vec2AndDir{
      .pos = bounds.TopCenter(),
      .dir = -90_deg,
  });
}

RRect Object::CoarseBounds(animation::Display* display) const {
  return RRect{.rect = Shape().getBounds(),
               .radii = {{0, 0}, {0, 0}, {0, 0}, {0, 0}},
               .type = SkRRect::kRect_Type};
}

audio::Sound& Object::NextSound() { return embedded::assets_SFX_next_wav; }

}  // namespace automat
