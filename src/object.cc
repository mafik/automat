// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "object.hh"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "../build/generated/embedded.hh"
#include "automat.hh"
#include "base.hh"
#include "drag_action.hh"
#include "font.hh"
#include "location.hh"
#include "menu.hh"
#include "object_iconified.hh"
#include "object_lifetime.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "time.hh"
#include "ui_connection_widget.hh"
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
  Dir PreferredDir() const override { return NW; }
};

struct MoveLocationOption : TextOption {
  WeakPtr<Location> location_weak;
  WeakPtr<Object> object_weak;

  MoveLocationOption(WeakPtr<Location> location_weak, WeakPtr<Object> object_weak)
      : TextOption("Move"), location_weak(location_weak), object_weak(object_weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MoveLocationOption>(location_weak, object_weak);
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
      Object& container_object = *location->object;
      if (auto container = container_object.AsContainer()) {
        auto& root_widget = location->FindRootWidget();
        if (auto extracted = container->Extract(*object)) {
          extracted->parent = location->object_widget->AcquireTrackedPtr();
          return std::make_unique<DragLocationAction>(pointer, std::move(extracted));
        } else {
          LOG << "Unable to extract " << object->Name() << " from " << container_object.Name()
              << " (no location)";
        }
      } else {
        LOG << "Unable to extract " << object->Name() << " from " << container_object.Name()
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

  Dir PreferredDir() const override { return N; }
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
  Dir PreferredDir() const override { return S; }
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
  Dir PreferredDir() const override { return NE; }
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
  Dir PreferredDir() const override { return NE; }
};

static const char* InterfaceName(NestedWeakPtr<Interface>& weak) {
  if (auto ptr = weak.Lock()) {
    return ptr->Name().data();
  }
  return "Field of a deleted object";
}

struct TurnOnOption : TextOption {
  NestedWeakPtr<OnOff> weak;
  TurnOnOption(NestedWeakPtr<OnOff> weak) : TextOption("Turn on"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<TurnOnOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto ptr = weak.Lock()) {
      ptr->TurnOn();
    }
    return nullptr;
  }
};

struct TurnOffOption : TextOption {
  NestedWeakPtr<OnOff> weak;
  TurnOffOption(NestedWeakPtr<OnOff> weak) : TextOption("Turn off"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<TurnOffOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto ptr = weak.Lock()) {
      ptr->TurnOff();
    }
    return nullptr;
  }
};

struct SyncWidget : ui::Widget {
  Vec2 start, end;
  SyncWidget(ui::Widget* parent) : Widget(parent) {}
  SkPath Shape() const override { return SkPath(); }
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
  animation::Phase Tick(time::Timer&) override { return animation::Finished; }
  void Draw(SkCanvas& canvas) const override {
    SkPaint paint;
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1_mm);
    paint.setColor(SK_ColorRED);
    canvas.drawLine(start.x, start.y, end.x, end.y, paint);
  }
};

struct SyncAction : Action {
  NestedWeakPtr<Interface> weak;
  TrackedPtr<ObjectWidget> object_widget;
  SyncWidget sync_widget;
  SyncAction(ui::Pointer& pointer, NestedWeakPtr<Interface> weak, ObjectWidget* widget)
      : Action(pointer),
        weak(weak),
        object_widget(widget->AcquireTrackedPtr()),
        sync_widget(pointer.GetWidget()) {
    // TODO: invite objects to show their fields that satisfy the syncable interface
    Update();
  }
  ~SyncAction() {
    // TODO: tell objects to hide their fields
    // Check if the pointer is over a compatible interface
    if (auto interface = weak.Lock()) {
      auto target_location = root_machine->LocationAtPoint(sync_widget.end);
      if (target_location == nullptr || target_location->object == nullptr) return;
      // TODO: make this work with fields
      // TODO: make this work with other (non-on/off) interfaces
      auto* target_interface = (OnOff*)(*target_location->object);  // operator cast
      if (target_interface == nullptr) return;
      auto sync_block = Sync(interface);
      NestedPtr<Interface> target{target_location->object, target_interface};
      sync_block->FullSync(interface);
      sync_block->FullSync(target);
      auto& loc = root_machine->Insert(sync_block);
      loc.position = target_location->position;
      loc.position_vel = Vec2(0, 1);
    }
  }
  void Update() {
    if (auto interface = weak.Lock()) {
      auto* widget = pointer.root_widget.widgets.FindOrNull(*interface.Owner<Object>());
      auto start_local = widget->PartShape(interface.Get()).getBounds().center();
      auto start = TransformBetween(*widget, *root_machine).mapPoint(start_local);
      sync_widget.start = start;
      sync_widget.end = pointer.PositionWithinRootMachine();
      sync_widget.WakeAnimation();
    } else {
      pointer.ReplaceAction(*this, nullptr);
    }
  }
  ui::Widget* Widget() { return &sync_widget; }
};

struct SyncOption : TextOption {
  NestedWeakPtr<Interface> weak;
  SyncOption(NestedWeakPtr<Interface> weak) : TextOption("Sync"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<SyncOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto interface = weak.Lock()) {
      auto widget = pointer.root_widget.widgets.FindOrNull(*interface.Owner<Object>());
      return std::make_unique<SyncAction>(pointer, interface, widget);
    }
    return nullptr;
  }
};

struct UnsyncOption : TextOption {
  NestedWeakPtr<Interface> weak;
  UnsyncOption(NestedWeakPtr<Interface> weak) : TextOption("Unsync"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<UnsyncOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto interface = weak.Lock()) {
      interface->Unsync();
    }
    return nullptr;
  }
};

struct FieldOption : TextOption, OptionsProvider {
  NestedWeakPtr<Interface> interface_weak;
  FieldOption(NestedWeakPtr<Interface> weak)
      : TextOption(InterfaceName(weak)), interface_weak(weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<FieldOption>(interface_weak);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto ptr = interface_weak.Lock()) {
      return OpenMenu(pointer);
    }
    return nullptr;
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    if (auto interface = interface_weak.Lock()) {
      if (auto* on_off = dynamic_cast<OnOff*>(interface.Get())) {
        if (on_off->IsOn()) {
          TurnOffOption turn_off(NestedWeakPtr<OnOff>(interface_weak.GetOwnerWeak(), on_off));
          visitor(turn_off);
        } else {
          TurnOnOption turn_on(NestedWeakPtr<OnOff>(interface_weak.GetOwnerWeak(), on_off));
          visitor(turn_on);
        }
      }
      SyncOption sync(interface);
      visitor(sync);
      if (!interface->end.IsExpired()) {
        UnsyncOption unsync(interface);
        visitor(unsync);
      }
    }
  }
};

void Object::WidgetBase::VisitOptions(const OptionsVisitor& visitor) const {
  if (auto loc = ui::Closest<Location>(const_cast<WidgetBase&>(*this))) {
    auto loc_weak = loc->AcquireWeakPtr();
    DeleteOption del{loc_weak};
    visitor(del);
    MoveLocationOption move{loc_weak, object};
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
    if (auto obj = object.Lock()) {
      obj->Parts([&](Part& part) {
        if (auto* interface = dynamic_cast<Interface*>(&part)) {
          FieldOption field_option{NestedWeakPtr<Interface>(WeakPtr<Object>(object), interface)};
          Str part_name;
          obj->PartName(*interface, part_name);
          // Sometimes it's convenient for an object to expose some options more directly (without
          // nested menus). We do this when the object uses some part without giving it a name.
          if (part_name.empty()) {
            field_option.VisitOptions(visitor);
          } else {
            visitor(field_option);
          }
        }
      });
    }
  }
}

std::unique_ptr<Action> Object::WidgetBase::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  if (btn == ui::PointerButton::Left) {
    if (auto* location = Closest<Location>(*p.hover)) {
      MoveLocationOption move{location->AcquireWeakPtr(), object};
      return move.Activate(p);
    } else {
      LOG << "No parent location";
    }
  } else if (btn == ui::PointerButton::Right) {
    return OpenMenu(p);
  }
  return Widget::FindAction(p, btn);
}

void Object::Updated(Location& here, Location& updated) {
  if (Runnable* runnable = dynamic_cast<Runnable*>(this)) {
    here.ScheduleRun();
  }
}

void Object::SerializeState(Serializer& writer, const char* key) const {
  auto value = GetText();
  if (!value.empty()) {
    writer.Key(key);
    writer.String(value.data(), value.size());
  }
}

void Object::DeserializeState(Deserializer& d) {
  Status status;
  Str value;
  d.Get(value, status);
  if (!OK(status)) {
    ReportError(status.ToStr());
    return;
  }
  SetText(value);
}

audio::Sound& Object::NextSound() { return embedded::assets_SFX_next_wav; }

void Object::ForEachWidget(std::function<void(ui::RootWidget&, ui::Widget&)> cb) {
  for (auto* root_widget : ui::root_widgets) {
    if (auto widget = root_widget->widgets.FindOrNull(*this)) {
      cb(*root_widget, *widget);
    }
  }
}

void Object::WakeWidgetsAnimation() {
  ForEachWidget([](ui::RootWidget&, ui::Widget& widget) { widget.WakeAnimation(); });
}

void Object::ReportError(std::string_view message, std::source_location location) {
  automat::ReportError(*this, *this, message, location);
}

void Object::ClearOwnError() { automat::ClearError(*this, *this); }

float Object::WidgetBase::GetBaseScale() const {
  if (automat::IsIconified(object.GetUnsafe())) {
    auto bounds = CoarseBounds().rect;
    return std::min<float>(1_cm / bounds.Width(), 1_cm / bounds.Height());
  }
  return 1;
}

void Object::WidgetBase::ConnectionPositions(Vec<Vec2AndDir>& out_positions) const {
  // By default just one position on the top of the bounding box.
  auto shape = Shape();
  Rect bounds = shape.getBounds();
  out_positions.push_back(Vec2AndDir{
      .pos = bounds.TopCenter(),
      .dir = -90_deg,
  });
  out_positions.push_back(Vec2AndDir{
      .pos = bounds.LeftCenter(),
      .dir = 0_deg,
  });
  out_positions.push_back(Vec2AndDir{
      .pos = bounds.RightCenter(),
      .dir = -180_deg,
  });
}

Vec2AndDir ObjectWidget::ArgStart(const Argument& arg, ui::Widget* coordinate_space) {
  SkPath shape = PartShape(&const_cast<Argument&>(arg));
  Rect bounds = shape.getBounds();
  Vec2AndDir pos_dir{
      .pos = bounds.BottomCenter(),
      .dir = -90_deg,
  };
  if (coordinate_space) {
    auto m = TransformBetween(*this, *coordinate_space);
    pos_dir.pos = m.mapPoint(pos_dir.pos);
  }
  return pos_dir;
}

void Object::Relocate(Location* new_here) { here = new_here; }

Object::~Object() { LifetimeObserver::CheckDestroyNotified(*this); }

bool Object::WidgetBase::AllowChildPointerEvents(ui::Widget&) const { return !IsIconified(); }

bool Object::WidgetBase::IsIconified() const { return automat::IsIconified(object.GetUnsafe()); }

void Object::Parts(const std::function<void(Part&)>& cb) { cb(*this); }

void Object::PartName(Part& part, Str& out_name) { out_name = part.Name(); }

void Object::Args(const std::function<void(Argument&)>& cb) {
  Parts([&](Part& part) {
    if (Argument* arg = dynamic_cast<Argument*>(&part)) {
      cb(*arg);
    }
  });
}

Location* Object::MyLocation() {
  for (auto& loc : root_machine->locations) {
    if (loc->object == this) {
      return loc.Get();
    }
  }
  return here;
}

void Object::InvalidateConnectionWidgets(const Argument* arg) const {
  for (auto& w : ui::ConnectionWidgetRange(this, arg)) {
    w.WakeAnimation();
    if (w.state) {
      w.state->stabilized = false;
    }
  }
}

Part* Object::PartFromName(StrView needle) {
  Part* result = nullptr;
  Parts([&](Part& part) {
    Str part_name;
    PartName(part, part_name);
    if (part_name == needle) {
      result = &part;
    }
  });
  return result;
}

}  // namespace automat
