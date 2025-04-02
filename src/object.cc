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
#include "menu.hh"
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

struct DeleteOption : Option {
  std::weak_ptr<Location> weak;
  DeleteOption(std::weak_ptr<Location> weak) : Option("Delete"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<DeleteOption>(weak); }
  std::unique_ptr<Action> Activate(gui::Pointer& pointer) const override {
    if (shared_ptr<Location> loc = weak.lock()) {
      if (auto parent_machine = loc->ParentAs<Machine>()) {
        parent_machine->Extract(*loc);
      }
      loc->ForgetParents();
    }
    return nullptr;
  }
};

struct MoveOption : Option {
  std::weak_ptr<Location> location_weak;
  std::weak_ptr<Object> object_weak;

  MoveOption(std::weak_ptr<Location> location_weak, std::weak_ptr<Object> object_weak)
      : Option("Move"), location_weak(location_weak), object_weak(object_weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MoveOption>(location_weak, object_weak);
  }
  std::unique_ptr<Action> Activate(gui::Pointer& pointer) const override {
    auto location = location_weak.lock();
    if (location == nullptr) {
      return nullptr;
    }
    auto object = object_weak.lock();
    if (object == nullptr) {
      LOG << "Object is nullptr";
      return nullptr;
    }
    if (location->object != object) {
      if (auto container = location->object->AsContainer()) {
        Vec2 contact_point{0, 0};
        auto& root_widget = location->FindRootWidget();
        auto object_widget = root_widget.widgets.Find(*object);
        float scale = 1;
        if (object_widget) {
          contact_point = pointer.PositionWithin(*object_widget);
          scale = object_widget->local_to_parent.rc(0, 0);
        }
        if (auto extracted = container->Extract(*object)) {
          extracted->position = pointer.PositionWithinRootMachine() - contact_point;
          extracted->animation_state.position =
              pointer.PositionWithinRootMachine() - contact_point * scale;
          extracted->animation_state.scale = scale;
          return std::make_unique<DragLocationAction>(pointer, std::move(extracted), contact_point);
        } else {
          LOG << "Unable to extract " << object->Name() << " from " << location->object->Name()
              << " (no location)";
        }
      } else {
        LOG << "Unable to extract " << object->Name() << " from " << location->object->Name()
            << " (not a Container)";
      }
    }
    auto* machine = Closest<Machine>(*location);
    if (machine && location->object) {
      auto contact_point = pointer.PositionWithin(*location->WidgetForObject());
      return std::make_unique<DragLocationAction>(pointer, machine->Extract(*location),
                                                  contact_point);
    }
    return nullptr;
  }
};

struct RunOption : Option {
  std::weak_ptr<Location> weak;
  RunOption(std::weak_ptr<Location> weak) : Option("Run"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<RunOption>(weak); }
  std::unique_ptr<Action> Activate(gui::Pointer& pointer) const override {
    if (auto loc = weak.lock()) {
      loc->ScheduleRun();
    }
    return nullptr;
  }
};

void Object::FallbackWidget::VisitOptions(const OptionsVisitor& visitor) const {
  if (auto loc = gui::Closest<Location>(const_cast<FallbackWidget&>(*this))) {
    auto loc_weak = loc->WeakPtr();
    DeleteOption del{loc_weak};
    visitor(del);
    MoveOption move{loc_weak, object};
    visitor(move);
    if (auto runnable = loc->As<Runnable>()) {
      RunOption run{loc_weak};
      visitor(run);
    }
  }
}

std::unique_ptr<Action> Object::FallbackWidget::FindAction(gui::Pointer& p,
                                                           gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    MoveOption move{Closest<Location>(*p.hover)->WeakPtr(), object};
    return move.Activate(p);
  } else if (btn == gui::PointerButton::Right) {
    return OpenMenu(p);
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
  for (auto* root_widget : gui::root_widgets) {
    if (auto widget = root_widget->widgets.Find(*this)) {
      widget->WakeAnimation();
    }
  }
}

}  // namespace automat
