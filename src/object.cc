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
#include "control_flow.hh"
#include "drag_action.hh"
#include "font.hh"
#include "format.hh"
#include "location.hh"
#include "menu.hh"
#include "object_iconified.hh"
#include "object_lifetime.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "sync.hh"
#include "time.hh"
#include "ui_connection_widget.hh"
#include "ui_constants.hh"

namespace automat {

void Object::Toy::Draw(SkCanvas& canvas) const {
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

float Object::Toy::Width() const {
  auto text = Text();
  constexpr float kNameMargin = 0.001;
  float width_text = ui::GetFont().MeasureText(text) + 2 * kNameMargin;
  float width_rounded = ceil(width_text * 1000) / 1000;
  constexpr float kMinWidth = 0.008;
  return std::max(width_rounded, kMinWidth);
}

SkPath Object::Toy::Shape() const {
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
        audio::Play(embedded::assets_SFX_canvas_pick_wav);
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
        if (auto extracted = container->Extract(*object)) {
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
    auto parent_location = location->parent_location.Lock();
    auto* machine = parent_location->ThisAs<Machine>();
    if (machine && location->object) {
      machine->ForEachToy([](ui::RootWidget&, automat::Toy& w) { w.RedrawThisFrame(); });
      auto* mw = pointer.root_widget.toys.FindOrNull(*machine);
      if (mw) {
        return std::make_unique<DragLocationAction>(pointer, mw->ExtractStack(*location));
      }
    }
    return nullptr;
  }

  Dir PreferredDir() const override { return N; }
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

static Str SyncableName(NestedWeakPtr<Syncable>& weak) {
  if (auto ptr = weak.Lock()) {
    Str name;
    ptr.Owner<Object>()->AtomName(*ptr.Get(), name);
    return name;
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
  NestedWeakPtr<Syncable> weak;
  TrackedPtr<Toy> toy;
  SyncWidget sync_widget;
  SyncAction(ui::Pointer& pointer, NestedWeakPtr<Syncable> weak, Toy* toy)
      : Action(pointer),
        weak(weak),
        toy(toy->AcquireTrackedPtr()),
        sync_widget(pointer.GetWidget()) {
    // TODO: invite objects to show their fields that satisfy the Syncable
    Update();
  }
  ~SyncAction() {
    // TODO: tell objects to hide their fields
    // Check if the pointer is over a compatible Syncable
    if (auto syncable = weak.Lock()) {
      auto* mw = pointer.root_widget.toys.FindOrNull(*root_machine);
      if (mw) {
        mw->ConnectAtPoint(*syncable.Owner<Object>(), *syncable, sync_widget.end);
      }
    }
  }
  void Update() override {
    if (auto syncable = weak.Lock()) {
      auto* widget = pointer.root_widget.toys.FindOrNull(*syncable.Owner<Object>());
      auto* mw = pointer.root_widget.toys.FindOrNull(*root_machine);
      auto start_local = widget->AtomShape(syncable.Get()).getBounds().center();
      auto start = mw ? TransformBetween(*widget, *mw).mapPoint(start_local) : start_local;
      sync_widget.start = start;
      sync_widget.end = pointer.PositionWithinRootMachine();
      sync_widget.WakeAnimation();
    } else {
      pointer.ReplaceAction(*this, nullptr);
    }
    pointer.pointer_widget->WakeAnimation();
  }
  bool Highlight(Object& end_obj, Atom& end_atom) const override {
    auto ptr = weak.Lock();
    Object& start = *ptr.Owner<Object>();
    return ptr->Argument::CanConnect(start, end_atom);
  }
  ui::Widget* Widget() override { return &sync_widget; }
};

struct SyncOption : TextOption {
  NestedWeakPtr<Syncable> weak;
  SyncOption(NestedWeakPtr<Syncable> weak) : TextOption("Sync"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<SyncOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto syncable = weak.Lock()) {
      auto widget = pointer.root_widget.toys.FindOrNull(*syncable.Owner<Object>());
      return std::make_unique<SyncAction>(pointer, syncable, widget);
    }
    return nullptr;
  }
};

struct UnsyncOption : TextOption {
  NestedWeakPtr<Syncable> weak;
  UnsyncOption(NestedWeakPtr<Syncable> weak) : TextOption("Unsync"), weak(weak) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<UnsyncOption>(weak); }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto syncable = weak.Lock()) {
      syncable->Unsync();
    }
    return nullptr;
  }
};

struct FieldOption : TextOption, OptionsProvider {
  NestedWeakPtr<Syncable> syncable_weak;
  FieldOption(NestedWeakPtr<Syncable> weak) : TextOption(SyncableName(weak)), syncable_weak(weak) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<FieldOption>(syncable_weak);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    if (auto ptr = syncable_weak.Lock()) {
      return OpenMenu(pointer);
    }
    return nullptr;
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    if (auto syncable = syncable_weak.Lock()) {
      if (auto* on_off = dynamic_cast<OnOff*>(syncable.Get())) {
        if (on_off->IsOn()) {
          TurnOffOption turn_off(NestedWeakPtr<OnOff>(syncable_weak.GetOwnerWeak(), on_off));
          visitor(turn_off);
        } else {
          TurnOnOption turn_on(NestedWeakPtr<OnOff>(syncable_weak.GetOwnerWeak(), on_off));
          visitor(turn_on);
        }
      }
      SyncOption sync(syncable);
      visitor(sync);
      if (!syncable->end.IsExpired()) {
        UnsyncOption unsync(syncable);
        visitor(unsync);
      }
    }
  }
};

void Object::Toy::VisitOptions(const OptionsVisitor& visitor) const {
  if (auto* lw = ui::Closest<LocationWidget>(const_cast<Toy&>(*this))) {
    if (auto loc = lw->LockLocation()) {
      auto loc_weak = loc->AcquireWeakPtr();
      DeleteOption del{loc_weak};
      visitor(del);
      MoveLocationOption move{loc_weak, owner.Copy<Object>()};
      visitor(move);
      if (auto runnable = loc->object->AsRunnable()) {
        RunOption run{owner.Copy<Object>(), *runnable};
        visitor(run);
      }
      if (IsIconified()) {
        DeiconifyOption deiconify{loc_weak};
        visitor(deiconify);
      } else {
        IconifyOption iconify{loc_weak};
        visitor(iconify);
      }
      if (auto obj = LockOwner<Object>()) {
        obj->Atoms([&](Atom& atom) {
          if (auto* syncable = dynamic_cast<Syncable*>(&atom)) {
            FieldOption field_option{NestedWeakPtr<Syncable>(owner.Copy<Object>(), syncable)};
            visitor(field_option);
          }
          return LoopControl::Continue;
        });
      }
    }
  }
}

std::unique_ptr<Action> Object::Toy::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  if (btn == ui::PointerButton::Left) {
    if (auto* lw = Closest<LocationWidget>(*p.hover)) {
      if (auto loc = lw->LockLocation()) {
        MoveLocationOption move{loc->AcquireWeakPtr(), owner.Copy<Object>()};
        return move.Activate(p);
      }
    } else {
      LOG << "No parent location";
    }
  } else if (btn == ui::PointerButton::Right) {
    return OpenMenu(p);
  }
  return Widget::FindAction(p, btn);
}

void Object::Updated(WeakPtr<Object>& updated) {
  if (Runnable* runnable = AsRunnable()) {
    runnable->ScheduleRun(*this);
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

float Object::Toy::GetBaseScale() const {
  if (automat::IsIconified(static_cast<Object*>(owner.GetUnsafe()))) {
    auto bounds = CoarseBounds().rect;
    return std::min<float>(1_cm / bounds.Width(), 1_cm / bounds.Height());
  }
  return 1;
}

void Object::Toy::ConnectionPositions(Vec<Vec2AndDir>& out_positions) const {
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

Vec2AndDir Object::Toy::ArgStart(const Argument& arg, ui::Widget* coordinate_space) {
  SkPath shape = AtomShape(&const_cast<Argument&>(arg));
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

bool Object::Toy::AllowChildPointerEvents(ui::Widget&) const { return !IsIconified(); }

bool Object::Toy::IsIconified() const {
  return automat::IsIconified(static_cast<Object*>(owner.GetUnsafe()));
}

void Object::Atoms(const std::function<LoopControl(Atom&)>& cb) {}

void Object::AtomName(Atom& atom, Str& out_name) { out_name = atom.Name(); }

template <typename T>
static T* FindAtom(Object& obj) {
  T* result = nullptr;
  obj.Atoms([&](Atom& atom) {
    result = dynamic_cast<T*>(&atom);
    return result ? LoopControl::Break : LoopControl::Continue;
  });
  return result;
}

LongRunning* Object::AsLongRunning() { return FindAtom<LongRunning>(*this); }
Runnable* Object::AsRunnable() { return FindAtom<Runnable>(*this); }
SignalNext* Object::AsSignalNext() { return FindAtom<SignalNext>(*this); }
OnOff* Object::AsOnOff() { return FindAtom<OnOff>(*this); }

void Object::Args(const std::function<void(Argument&)>& cb) {
  Atoms([&](Atom& atom) {
    if (Argument* arg = dynamic_cast<Argument*>(&atom)) {
      cb(*arg);
    }
    return LoopControl::Continue;
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

Atom* Object::AtomFromName(StrView needle) {
  Atom* result = nullptr;
  Atoms([&](Atom& atom) {
    Str atom_name;
    AtomName(atom, atom_name);
    if (atom_name == needle) {
      result = &atom;
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

Str ObjectSerializer::ResolveName(Object& object, Atom* atom, StrView hint) {
  Str ret = ResolveName(object, hint);
  if (atom && atom != &object) {
    ret += ".";
    Str atom_name;
    object.AtomName(*atom, atom_name);
    ret += atom_name;
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
    o->SerializeState(*this);

    {  // Serialize object parts
      // ATM we only serialize Args
      bool args_opened = false;  // used to lazily call StartObject
      o->Args([&](Argument& arg) {
        auto end = arg.Find(*o);
        if (!end) return;
        if (!args_opened) {
          args_opened = true;
          Key("links");
          StartObject();
        }
        Str arg_name;
        o->AtomName(arg, arg_name);
        Key(arg_name);
        auto to_name = ResolveName(*end.Owner<Object>(), end.Get());
        String(to_name);
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

NestedPtr<Atom> ObjectDeserializer::LookupAtom(StrView name) {
  auto dot_pos = name.find('.');
  Str to_name, to_atom;
  if (dot_pos != Str::npos) {
    to_name = name.substr(0, dot_pos);
    to_atom = name.substr(dot_pos + 1);
  } else {
    to_name = name;
    to_atom = "";
  }
  auto* to = LookupObject(to_name);
  if (to == nullptr) {
    return {};
  }
  if (to_atom.empty()) {
    return NestedPtr<Atom>(to->AcquirePtr(), to);
  } else {
    auto* atom = to->AtomFromName(to_atom);
    return NestedPtr<Atom>(to->AcquirePtr(), atom);
  }
}

}  // namespace automat
