// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "object.hpp"

#include <include/core/SkColor.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradient.h>

#include <cassert>
#include <cmath>

#include "../build/generated/embedded.hpp"
#include "automat.hpp"
#include "base.hpp"
#include "casting.hpp"
#include "control_flow.hpp"
#include "drag_action.hpp"
#include "font.hpp"
#include "format.hpp"
#include "image_provider.hpp"
#include "location.hpp"
#include "object_lifetime.hpp"
#include "object_source.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "sync.hpp"
#include "time.hpp"
#include "ui_connection_widget.hpp"
#include "ui_constants.hpp"

namespace automat {

void ObjectToy::Draw(SkCanvas& canvas) const {
  SkPath path = Shape();

  SkPaint paint;
  SkPoint pts[2] = {{0, 0}, {0, 0.01}};
  SkColor4f colors[2] = {"#0f5f4d"_color4f, "#468257"_color4f};
  sk_sp<SkShader> gradient = SkShaders::LinearGradient(
      pts, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}});
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

  SkColor4f border_colors[2] = {"#1c5d3e"_color4f, "#76a87a"_color4f};
  sk_sp<SkShader> border_gradient = SkShaders::LinearGradient(
      pts, SkGradient{SkGradient::Colors{border_colors, SkTileMode::kClamp}, {}});
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

float ObjectToy::Width() const {
  auto text = Text();
  constexpr float kNameMargin = 0.001;
  float width_text = ui::GetFont().MeasureText(text) + 2 * kNameMargin;
  float width_rounded = ceil(width_text * 1000) / 1000;
  constexpr float kMinWidth = 0.008;
  return std::max(width_rounded, kMinWidth);
}

SkPath ObjectToy::Shape() const {
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

std::unique_ptr<Action> PickUp(ui::Pointer& pointer, Location& location, Object& object) {
  if (location.object.Get() != &object) {
    Object& container_object = *location.object;
    if (auto container = container_object.AsContainer()) {
      if (auto extracted = container->Extract(object)) {
        return std::make_unique<DragLocationAction>(pointer, std::move(extracted));
      } else {
        LOG << "Unable to extract " << object.Name() << " from " << container_object.Name()
            << " (no location)";
      }
    } else {
      LOG << "Unable to extract " << object.Name() << " from " << container_object.Name()
          << " (not a Container)";
    }
  }
  auto board = location.LockBoard();
  if (board && location.object) {
    auto* mw = pointer.root_widget.toys.FindOrNull(*board);
    if (mw) {
      return std::make_unique<DragLocationAction>(pointer, mw->DragStack(location), mw);
    }
  }
  return nullptr;
}

std::unique_ptr<Action> DragNew(ui::Pointer& pointer, Ptr<Object>&& object,
                                std::unique_ptr<Toy>&& toy) {
  auto loc = MAKE_PTR(Location);
  loc->InsertHere(std::move(object));
  audio::Play(embedded::assets_SFX_toolbar_pick_wav);
  return std::make_unique<DragLocationAction>(pointer, std::move(loc), std::move(toy));
}

Ptr<Object> Location::move_Impl::OnTake() {
  Ptr<Object> taken = obj->object.Release();
  obj->WakeToys();
  engine.WakeToys();
  return taken;
}

std::unique_ptr<Action> Location::move_Impl::OnActivate(ui::Pointer& pointer, automat::Toy*) {
  return PickUp(pointer, *obj, *obj->object);
}

Ptr<Object> Location::copy_Impl::OnTake() { return obj->object->Clone(); }

std::unique_ptr<Action> Location::copy_Impl::OnActivate(ui::Pointer& pointer, automat::Toy*) {
  auto board = obj->LockBoard();
  auto* board_widget = board ? pointer.root_widget.toys.FindOrNull(*board) : nullptr;
  if (board_widget == nullptr) return nullptr;
  Vec<std::unique_ptr<automat::Toy>> toys;
  auto clones = board_widget->CloneStack(*obj, toys);
  audio::Play(embedded::assets_SFX_canvas_pick_wav);
  return std::make_unique<DragLocationAction>(pointer, std::move(clones), nullptr, std::nullopt,
                                              std::move(toys));
}

Ptr<Object> Location::clone_Impl::OnTake() { return obj->object; }

std::unique_ptr<Action> Location::clone_Impl::OnActivate(ui::Pointer& pointer, automat::Toy* toy) {
  auto new_loc = MAKE_PTR(Location);
  new_loc->InsertHere(Ptr<Object>(obj->object));
  Vec2 position = obj->PeekPosition();
  if (auto board = obj->LockBoard()) position += board->position;
  new_loc->placement = Location::Direct{position, obj->PeekScale()};
  std::unique_ptr<automat::Toy> new_toy;
  if (auto* lw = toy ? ui::Closest<LocationWidget>(*toy) : nullptr; lw && lw->toy) {
    new_toy = new_loc->object->MakeToy(lw->toy.Get());
  }
  audio::Play(embedded::assets_SFX_canvas_pick_wav);
  return std::make_unique<DragLocationAction>(pointer, std::move(new_loc), std::move(new_toy));
}

constinit ObjectSource::Table kMakeObject = [] {
  ObjectSource::Table t("New");
  t.cursor = ui::Cursor::Hand;
  t.take = [](ObjectSource self) { return self.object_ptr->Clone(); };
  t.activate = [](Interface self, ui::Pointer& pointer, Toy* toy) -> std::unique_ptr<Action> {
    if (toy == nullptr) return nullptr;
    auto object = cast<ObjectSource>(self).Take();
    auto new_toy = object->MakeToy(toy);
    return DragNew(pointer, std::move(object), std::move(new_toy));
  };
  t.make_icon = [](Interface self, ui::Widget* parent) -> std::unique_ptr<ui::Widget> {
    return self.object_ptr->MakeToy(parent);
  };
  return t;
}();

Interface ObjectToy::ParentLocation() {
  auto* lw = ui::Closest<LocationWidget>(*this);
  auto loc = lw ? lw->LockLocation() : nullptr;
  return loc ? Interface(*loc) : Interface();
}

Interface ObjectToy::FindOption(ui::Pointer&, ui::ActionTrigger trigger) {
  using enum ui::Dir;
  auto object = LockOwner();
  if (!object) return {};
  ui::Dir dir = trigger;
  if (dir == S) return ParentLocation();
  if (dir == N) {
    if (HasError(*object)) return Interface(*object, kThisIsFine);
    return object->Find<Runnable>();
  }
  return {};
}

void Object::Updated(WeakPtr<Object>& updated) {
  if (auto runnable = Find<Runnable>()) {
    runnable.ScheduleRun();
  }
}

void Object::SerializeState(ObjectSerializer& writer) const {
  auto value = GetText();
  if (!value.empty()) {
    writer.Key("value");
    writer.String(value.data(), value.size());
  }
}

bool Object::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "value") {
    Status status;
    Str value;
    d.Get(value, status);
    if (!OK(status)) {
      ReportError(status.ToStr());
      return true;
    }
    SetText(value);
    return true;
  }
  return false;
}

audio::Sound& Object::NextSound() { return embedded::assets_SFX_next_wav; }

void Object::ReportError(std::string_view message, std::source_location location) {
  automat::ReportError(*this, *this, message, location);
}

void Object::ClearOwnError() { automat::ClearError(*this, *this); }

float ObjectToy::GetBaseScale() const {
  if (iconified) {
    auto bounds = CoarseBounds().rect;
    float fully_iconified_scale = std::min<float>(1_cm / bounds.Width(), 1_cm / bounds.Height());
    // TODO: interpolate between 1 and fully_iconified_scale using 'float_iconified'
    return std::lerp(1, fully_iconified_scale, iconified);
  }
  return 1;
}

void ObjectToy::ConnectionPositions(Vec<Vec2AndDir>& out_positions) const {
  // By default just one position on the top of the bounding box.
  Rect bounds = CoarseBounds().rect;
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

Vec2AndDir ObjectToy::ArgStart(const Interface::Table& arg) {
  Rect bounds = CoarseBounds().rect;
  Vec2AndDir pos_dir{
      .pos = bounds.BottomCenter(),
      .dir = -90_deg,
  };
  return pos_dir;
}

Vec2AndDir ObjectToy::ArgStart(const Interface::Table& arg, ui::Widget* coordinate_space) {
  Vec2AndDir pos_dir = ArgStart(arg);
  if (coordinate_space) {
    auto m = TransformBetween(*this, *coordinate_space);
    pos_dir.pos = m.mapPoint(pos_dir.pos);
  }
  return pos_dir;
}

Object::~Object() {
  assert(owners == nullptr);
  LifetimeObserver::CheckDestroyNotified(*this);
}

bool ObjectToy::AllowChildPointerEvents(ui::Widget&) const { return iconified <= 0.5f; }

void ObjectToy::UpdateErrorFlames() {
  auto obj = LockOwner();
  Str text;
  bool burning = obj && HasError(*obj, [&](Error& error) { text = error.text; });
  if (!burning) {
    error_flames.reset();
    return;
  }
  if (!error_flames) {
    error_flames = std::make_unique<ErrorFlames>(*this);
  }
  error_flames->SetText(text);
}

void Object::Interfaces(const std::function<LoopControl(Interface)>& cb) {}

void OwnerLink::Link(Object& target) {
  auto lock = std::lock_guard(target.owners_lock);
  if (target.owners == nullptr) {
    prev = next = this;
    target.owners = this;
  } else {
    prev = target.owners->prev;
    next = target.owners;
    prev->next = this;
    next->prev = this;
  }
}

void OwnerLink::Unlink(Object& target) {
  auto lock = std::lock_guard(target.owners_lock);
  if (next == this) {
    target.owners = nullptr;
  } else {
    prev->next = next;
    next->prev = prev;
    if (target.owners == this) target.owners = next;
  }
  prev = next = nullptr;
}

void OwnerLink::TakeOver(OwnerLink& moved, Object& target) {
  auto lock = std::lock_guard(target.owners_lock);
  if (moved.next == &moved) {
    prev = next = this;
  } else {
    prev = moved.prev;
    next = moved.next;
    prev->next = this;
    next->prev = this;
  }
  if (target.owners == &moved) target.owners = this;
  moved.prev = moved.next = nullptr;
}

SmallVec<Ptr<Object>, 4> Object::Owners() {
  SmallVec<Ptr<Object>, 4> result;
  auto lock = std::lock_guard(owners_lock);
  if (owners == nullptr) return result;
  OwnerLink* link = owners;
  do {
    if (link->owner->IncrementOwningRefsNonZero()) {
      result.emplace_back(Ptr<Object>(link->owner));
    }
    link = link->next;
  } while (link != owners);
  return result;
}

void Object::MakeHome(Object& owner) {
  auto lock = std::lock_guard(owners_lock);
  if (owners == nullptr) return;
  OwnerLink* link = owners;
  do {
    if (link->owner == &owner) {
      owners = link;
      return;
    }
    link = link->next;
  } while (link != owners);
}

Ptr<Object> Object::HomeOwner() {
  auto lock = std::lock_guard(owners_lock);
  if (owners == nullptr) return nullptr;
  OwnerLink* link = owners;
  do {
    if (link->owner->IncrementOwningRefsNonZero()) return Ptr<Object>(link->owner);
    link = link->next;
  } while (link != owners);
  return nullptr;
}

Ptr<Location> Object::HomeLocation() {
  auto owner = HomeOwner();
  if (owner == nullptr) return nullptr;
  if (auto location = dyn_cast<Location>(owner)) return location;
  return owner->HomeLocation();
}

Interface Object::InterfaceFromName(StrView needle) {
  Interface result;
  Interfaces([&](Interface iface) {
    if (iface.Name() == needle) {
      result = iface;
      return LoopControl::Break;
    }
    return LoopControl::Continue;
  });
  return result;
}

Str& ObjectSerializer::ResolveName(Object& object, StrView hint) {
  auto it = object_to_name.find(&object);
  if (it == object_to_name.end()) {
    auto base_name = Str(object.Name());
    if (!hint.empty()) {
      base_name = f("{} {}", hint, base_name);
    }
    auto name = base_name;
    int i = 2;
    while (assigned_names.count(name)) {
      name = f("{} #{}", base_name, i++);
    }
    it = object_to_name.emplace(&object, name).first;
    assigned_names.insert(name);
    serialization_queue.push_back(&object);
  }
  return it->second;
}

Str ObjectSerializer::ResolveName(Object& object, Interface::Table* iface, StrView hint) {
  Str ret = ResolveName(object, hint);
  if (iface) {
    ret += ".";
    ret += iface->name;
  }
  return ret;
}

void ObjectSerializer::Serialize(Object& start) {
  ResolveName(start);
  while (!serialization_queue.empty()) {
    auto* o = serialization_queue.back();
    serialization_queue.pop_back();
    auto name = ResolveName(*o);
    auto type_name = o->Name();
    Key(name);
    StartObject();
    Key("type");
    String(type_name.data(), type_name.length());
    if (auto home = o->HomeOwner()) {
      if (auto location = dyn_cast<Location>(home)) home = location->LockBoard();
      if (home) {
        Key("home");
        String(ResolveName(*home));
      }
    }
    o->SerializeState(*this);

    {  // Serialize object parts
      // ATM we only serialize Args
      bool args_opened = false;  // used to lazily call StartObject
      o->Each<Argument>([&](Argument arg) {
        if (!arg.IsConnected()) return LoopControl::Continue;
        auto end = arg.Find();
        auto* end_owner = end.object_ptr;
        if (end_owner == nullptr) return LoopControl::Continue;
        if (!args_opened) {
          args_opened = true;
          Key("links");
          StartObject();
        }
        Str arg_name(arg.Name());
        Key(arg_name);
        auto to_name = ResolveName(*end_owner, end.table_ptr);
        String(to_name);
        return LoopControl::Continue;
      });
      if (args_opened) {
        args_opened = false;
        EndObject();
      }
    }
    EndObject();
  }
}

void ObjectDeserializer::RegisterObject(StrView name, Object& object) {
  objects.emplace(name, object.AcquirePtr());
}

Object* ObjectDeserializer::LookupObject(StrView name) {
  auto to_it = objects.find(name);
  if (to_it == objects.end()) {
    return nullptr;
  }
  return to_it->second.Get();
}

Locked<Interface> ObjectDeserializer::LookupInterface(StrView name) {
  auto dot_pos = name.find('.');
  Str to_name, to_iface;
  if (dot_pos != Str::npos) {
    to_name = name.substr(0, dot_pos);
    to_iface = name.substr(dot_pos + 1);
  } else {
    to_name = name;
    to_iface = "";
  }
  auto* to = LookupObject(to_name);
  if (to == nullptr) {
    return {};
  }
  if (to_iface.empty()) {
    return AdoptLocked(Interface(*to->AcquirePtr().Release()));
  }
  Interface iface = to->InterfaceFromName(to_iface);
  if (iface.object_ptr == nullptr) {
    return {};
  }
  iface.object_ptr->IncrementOwningRefs();
  return AdoptLocked(iface);
}

}  // namespace automat
