// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "object.hh"

#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "../build/generated/embedded.hh"
#include "base.hh"
#include "drag_action.hh"
#include "font.hh"
#include "location.hh"
#include "menu.hh"
#include "root_widget.hh"
#include "ui_constants.hh"

namespace automat {

std::string_view Object::WidgetBase::Name() const {
  StrView name;
  if (auto obj = object.lock()) {
    name = obj->Name();
  } else {
    name = Widget::Name();
  }
  return name;
}

void Object::WidgetBase::Draw(SkCanvas& canvas) const {
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
  canvas.translate(path_bounds.width() / 2 - ui::GetFont().MeasureText(text) / 2,
                   path_bounds.height() / 2 - ui::kLetterSizeMM / 2 / 1000);
  ui::GetFont().DrawText(canvas, text, text_paint);
  canvas.restore();
}

float Object::WidgetBase::Width() const {
  auto text = Text();
  constexpr float kNameMargin = 0.001;
  float width_text = ui::GetFont().MeasureText(text) + 2 * kNameMargin;
  float width_rounded = ceil(width_text * 1000) / 1000;
  constexpr float kMinWidth = 0.008;
  return std::max(width_rounded, kMinWidth);
}

SkPath Object::WidgetBase::Shape() const {
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

struct DeleteOption : TextOption {
  WeakPtr<Location> weak;
  DeleteOption(WeakPtr<Location> weak) : TextOption("Delete"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<DeleteOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (Ptr<Location> loc = weak.lock()) {
      if (auto parent_machine = loc->ParentAs<Machine>()) {
        parent_machine->Extract(*loc);
      }
    }
    return nullptr;
  }
};

struct MoveOption : TextOption {
  WeakPtr<Location> location_weak;
  WeakPtr<Object> object_weak;

  MoveOption(WeakPtr<Location> location_weak, WeakPtr<Object> object_weak)
      : TextOption("Move"), location_weak(location_weak), object_weak(object_weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MoveOption>(location_weak, object_weak);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    auto location = location_weak.lock();
    if (location == nullptr) {
      return nullptr;
    }
    auto object = object_weak.lock();
    if (object == nullptr) {
      return nullptr;
    }
    // Sometimes we may want to pick an object that's stored within another object.
    // This branch handles such cases.
    if (location->object != object) {
      if (auto container = location->object->AsContainer()) {
        auto& root_widget = location->FindRootWidget();
        auto* object_widget = root_widget.widgets.Find(*object);
        if (auto extracted = container->Extract(*object)) {
          return std::make_unique<DragLocationAction>(pointer, std::move(extracted));
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
      machine->ForEachWidget([](ui::RootWidget&, ui::Widget& w) { w.RedrawThisFrame(); });
      return std::make_unique<DragLocationAction>(pointer, machine->ExtractStack(*location));
    }
    return nullptr;
  }
};

struct RunOption : TextOption {
  WeakPtr<Location> weak;
  RunOption(WeakPtr<Location> weak) : TextOption("Run"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<RunOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto loc = weak.lock()) {
      loc->ScheduleRun();
    }
    return nullptr;
  }
};

struct IconifyOption : TextOption {
  WeakPtr<Location> weak;
  IconifyOption(WeakPtr<Location> weak) : TextOption("Iconify"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<IconifyOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto loc = weak.lock()) {
      loc->Iconify();
    }
    return nullptr;
  }
};

struct DeiconifyOption : TextOption {
  WeakPtr<Location> weak;
  DeiconifyOption(WeakPtr<Location> weak) : TextOption("Deiconify"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<DeiconifyOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto loc = weak.lock()) {
      loc->Deiconify();
    }
    return nullptr;
  }
};

void Object::WidgetBase::VisitOptions(const OptionsVisitor& visitor) const {
  if (auto loc = ui::Closest<Location>(const_cast<WidgetBase&>(*this))) {
    auto loc_weak = loc->AcquireWeakPtr();
    DeleteOption del{loc_weak};
    visitor(del);
    MoveOption move{loc_weak, object};
    visitor(move);
    if (auto runnable = loc->As<Runnable>()) {
      RunOption run{loc_weak};
      visitor(run);
    }
    if (IsIconified()) {
      DeiconifyOption deiconify{loc_weak};
      visitor(deiconify);
    } else {
      IconifyOption iconify{loc_weak};
      visitor(iconify);
    }
  }
}

std::unique_ptr<Action> Object::WidgetBase::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  if (btn == ui::PointerButton::Left) {
    MoveOption move{Closest<Location>(*p.hover)->AcquireWeakPtr(), object};
    return move.Activate(p);
  } else if (btn == ui::PointerButton::Right) {
    return OpenMenu(p);
  }
  return Widget::FindAction(p, btn);
}

void Object::Updated(Location& here, Location& updated) {
  if (Runnable* runnable = dynamic_cast<Runnable*>(this)) {
    runnable->Run(here, here.GetRunTask());
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
  Status status;
  Str value;
  d.Get(value, status);
  if (!OK(status)) {
    l.ReportError(status.ToStr());
    return;
  }
  SetText(l, value);
}

audio::Sound& Object::NextSound() { return embedded::assets_SFX_next_wav; }

Object::WidgetInterface& Object::FindWidget(const ui::Widget* parent) {
  return parent->FindRootWidget().widgets.For(*this, parent);
}

void Object::ForEachWidget(std::function<void(ui::RootWidget&, ui::Widget&)> cb) {
  for (auto* root_widget : ui::root_widgets) {
    if (auto widget = root_widget->widgets.Find(*this)) {
      cb(*root_widget, *widget);
    }
  }
}

void Object::WakeWidgetsAnimation() {
  ForEachWidget([](ui::RootWidget&, ui::Widget& widget) { widget.WakeAnimation(); });
}

float Object::WidgetInterface::GetBaseScale() const {
  if (IsIconified()) {
    auto bounds = CoarseBounds().rect;
    return std::min<float>(1_cm / bounds.Width(), 1_cm / bounds.Height());
  }
  return 1;
}
}  // namespace automat
