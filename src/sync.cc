// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "sync.hh"

#include <include/core/SkClipOp.h>
#include <include/core/SkPaint.h>

#include <mutex>

#include "animation.hh"
#include "argument.hh"
#include "automat.hh"
#include "casting.hh"
#include "embedded.hh"
#include "global_resources.hh"
#include "log.hh"
#include "math.hh"
#include "root_widget.hh"
#include "status.hh"
#include "textures.hh"
#include "time.hh"
#include "units.hh"

namespace automat {

// --- Syncable::Table constructor ---

Syncable::Table::Table(StrView name, Kind kind) : Argument::Table(name, kind) {
  style = Style::Invisible;
  can_connect = [](Argument self, Interface end, Status& status) {
    auto& syncable = static_cast<Syncable::Table&>(*self.table);
    if (auto* other = dyn_cast_if_present<Syncable::Table>(end.table_ptr)) {
      if (syncable.can_sync && syncable.can_sync(Syncable(*self.object_ptr, syncable), Syncable(*end.object_ptr, *other))) {
        return;
      } else {
        AppendErrorMessage(status) += "Can only connect to compatible Syncable";
      }
    }
    if (auto gear = dynamic_cast<Gear*>(end.object_ptr)) {
      auto lock = std::shared_lock(gear->mutex);
      if (gear->members.empty()) {
        return;
      } else {
        auto member = gear->members.front().weak.Lock();
        if (member && syncable.can_sync && syncable.can_sync(Syncable(*self.object_ptr, syncable), Syncable(member.Owner<Object>(), member.Get()))) {
          return;
        } else {
          AppendErrorMessage(status) += "Wrong type of Gear";
        }
      }
    }
    AppendErrorMessage(status) += "Can only connect to similar parts";
  };
  on_connect = [](Argument self, Interface end) {
    auto& syncable = static_cast<Syncable::Table&>(*self.table);
    auto& st = *Syncable(*self.object_ptr, syncable).state;

    if (end) {
      st.end = NestedWeakPtr<Interface::Table>(end.object_ptr->AcquireWeakPtr(), end.table_ptr);
    } else {
      st.end = {};
    }

    if (!end) return;

    auto* target_syncable = dyn_cast_if_present<Syncable::Table>(end.table_ptr);
    if (target_syncable) {
      auto sync_block = FindGearOrNull(*end.object_ptr, *target_syncable);
      if (sync_block == nullptr) {
        sync_block = FindGearOrMake(*self.object_ptr, syncable);
        auto& loc = root_board->Insert(sync_block);
        loc.position = (end.object_ptr->here->position + self.object_ptr->here->position) / 2;
        loc.ForEachToy([](ui::RootWidget&, LocationWidget& toy) {
          toy.position_vel = Vec2(0, 1);
        });
      }
      sync_block->FullSync(*self.object_ptr, syncable);
      sync_block->FullSync(*end.object_ptr, *target_syncable);
      return;
    }
    auto* gear = dynamic_cast<Gear*>(end.object_ptr);
    if (gear) {
      gear->FullSync(*self.object_ptr, syncable);
    }
  };
  find = [](Argument self) -> NestedPtr<Interface::Table> {
    auto& syncable = static_cast<Syncable::Table&>(*self.table);
    return Syncable(*self.object_ptr, syncable).state->end.Lock();
  };
}

// --- Syncable::Unsync ---

void Syncable::Unsync() { state->Unsync(*object_ptr, *table); }

void Syncable::State::Unsync(Object& self, Syncable::Table& syncable) {
  auto gear = end.OwnerLockAs<Gear>();
  if (!gear) return;
  auto lock = std::unique_lock(gear->mutex);

  auto& members = gear->members;
  for (int i = 0; i < (int)members.size(); ++i) {
    // Compare both the interface pointer and the owner
    auto* member_iface = members[i].weak.GetUnsafe();
    auto* member_owner = members[i].weak.template OwnerUnsafe<Object>();
    if (member_iface == &syncable && member_owner == &self) {
      members.erase(members.begin() + i);
      break;
    }
  }

  source = false;
  end.Reset();
  if (syncable.on_unsync) syncable.on_unsync(Syncable(self, syncable));
}

// --- FindGearOrMake / FindGearOrNull ---

Ptr<Gear> FindGearOrMake(Object& source_obj, Syncable::Table& source) {
  auto& state = *Syncable(source_obj, source).state;
  auto sync_block = state.end.OwnerLockAs<Gear>();
  if (!sync_block) {
    sync_block = MAKE_PTR(Gear);
    sync_block->AddSource(source_obj, source);
  }
  return sync_block;
}

Ptr<Gear> FindGearOrNull(Object& source_obj, Syncable::Table& source) {
  auto& state = *Syncable(source_obj, source).state;
  auto sync_block = state.end.OwnerLockAs<Gear>();
  if (!sync_block) {
    return nullptr;
  }
  return sync_block;
}

// --- Gear methods ---

Gear::~Gear() {
  auto lock = std::unique_lock(mutex);
  while (!members.empty()) {
    auto& back = members.back();
    if (auto locked = back.weak.Lock()) {
      auto* syncable = locked.Get();
      auto* owner = locked.Owner<Object>();
      auto& state = *Syncable(*owner, *syncable).state;
      if (state.source) {
        state.source = false;
        state.end.Reset();
        if (syncable->on_unsync) syncable->on_unsync(Syncable(*owner, *syncable));
      }
    }
    members.pop_back();
  }
}

void Gear::AddSink(Object& obj, Syncable::Table& syncable) {
  auto guard = std::unique_lock(mutex);
  NestedWeakPtr<Syncable::Table> weak(obj.AcquireWeakPtr(), &syncable);
  for (int i = 0; i < (int)members.size(); ++i) {
    if (members[i].weak == weak) {
      members[i].sink = true;
      return;
    }
  }
  members.emplace_back(std::move(weak), true);
}

void Gear::AddSource(Object& obj, Syncable::Table& syncable) {
  auto& state = *Syncable(obj, syncable).state;
  auto old_sync_block = state.end.OwnerLockAs<Gear>();
  bool was_source = state.source;
  if (old_sync_block.Get() != this) {
    Syncable(obj, syncable).Connect(Interface(*this));
    if (old_sync_block) {
      while (!old_sync_block->members.empty()) {
        // stealing all of the members from the old gear
        members.emplace_back(old_sync_block->members.back());
        old_sync_block->members.pop_back();

        // redirecting the members' sync state to this gear
        if (auto locked = members.back().weak.Lock()) {
          auto* member_syncable = locked.Get();
          auto* member_owner = locked.Owner<Object>();
          auto& member_state = *Syncable(*member_owner, *member_syncable).state;
          member_state.end = NestedWeakPtr<Interface::Table>(AcquireWeakPtr<Object>(), nullptr);
        }
      }
    } else {
      NestedWeakPtr<Syncable::Table> weak(obj.AcquireWeakPtr(), &syncable);
      bool found = false;
      for (int i = 0; i < (int)members.size(); ++i) {
        if (members[i].weak == weak) {
          found = true;
          break;
        }
      }
      if (!found) {
        members.emplace_back(std::move(weak), false);
      }
    }
  }
  if (!was_source) {
    state.source = true;
    if (syncable.on_sync) syncable.on_sync(Syncable(obj, syncable));
  }
}

void Gear::FullSync(Object& obj, Syncable::Table& syncable) {
  AddSink(obj, syncable);
  AddSource(obj, syncable);
}

// --- Gear rendering ---

constexpr float kPrimaryGearRadius = 9_mm;
constexpr float kSecondaryGearRadius = 6_mm;
constexpr float kTeethAmplitude = 0.7_mm;

static sk_sp<SkRuntimeEffect>& GearShader() {
  Status status;
  static auto effect = resources::CompileShader(embedded::assets_gear_sksl, status);
  if (!OK(status)) {
    FATAL << status;
  }
  return effect;
}

static PersistentImage& RubberColor() {
  static auto color = PersistentImage::MakeFromAsset(embedded::assets_rubber_color_webp,
                                                     {
                                                         .scale = 1,
                                                         .tile_x = SkTileMode::kRepeat,
                                                         .tile_y = SkTileMode::kRepeat,
                                                     });
  return color;
}

static PersistentImage& RubberNormal() {
  static auto normal = PersistentImage::MakeFromAsset(embedded::assets_rubber_normal_webp,
                                                      {
                                                          .scale = 1,
                                                          .tile_x = SkTileMode::kRepeat,
                                                          .tile_y = SkTileMode::kRepeat,
                                                          .raw_shader = true,
                                                      });
  return normal;
}

struct GearWidget : ObjectToy {
  GearWidget(Widget* parent, Object& object) : ObjectToy(parent, object) {}

  SkPath Shape() const override { return SkPath::Circle(0, 0, 1_cm); }

  bool CenteredAtZero() const override { return true; }

  animation::Phase Tick(time::Timer& t) override { return animation::Animating; }

  void Draw(SkCanvas& canvas) const override {
    auto& effect = GearShader();
    auto& color = RubberColor();
    auto& normal = RubberNormal();

    float primary_rotation = time::SteadySaw<20.0>() * M_PI * 2;
    SkRuntimeEffectBuilder builder(effect);
    builder.uniform("iRotationRad") = primary_rotation;
    SkMatrix px_to_local;
    (void)canvas.getLocalToDeviceAs3x3().invert(&px_to_local);
    builder.uniform("iPixelRadius") = (float)px_to_local.mapRadius(1);
    builder.uniform("iGearCount") = (float)12;
    builder.uniform("iTeethAmplitudeCm") = (float)(kTeethAmplitude / 1_cm);
    builder.uniform("iRadiusCm") = (float)(kPrimaryGearRadius / 1_cm);
    builder.uniform("iGrooveStartCm") = (float)0.25;
    builder.uniform("iGrooveMiddleCm") = (float)0.35;
    builder.uniform("iGrooveEndCm") = (float)0.85;
    builder.uniform("iHoleRadiusCm") = 0.1f;
    builder.uniform("iHoleRoundnessCm") = 0.05f;
    builder.uniform("iEndPos") = Vec2(0, 0);
    builder.child("iRubberColor") = *color.shader;
    builder.child("iRubberNormal") = *normal.shader;
    SkPaint gear_paint;
    gear_paint.setShader(builder.makeShader());
    canvas.drawCircle(0, 0, kPrimaryGearRadius + kTeethAmplitude, gear_paint);
  }

  Optional<Rect> TextureBounds() const override { return Shape().getBounds(); }
};

SyncConnectionWidget::SyncConnectionWidget(Widget* parent, Object& object,
                                           Syncable::Table& syncable)
    : Toy(parent, object, &syncable) {}

SkPath SyncConnectionWidget::Shape() const { return SkPath(); }

animation::Phase SyncConnectionWidget::Tick(time::Timer& t) {
  bounds = Rect{};
  auto& toy_store = ToyStore();

  // Check if the object of this connection still exists.
  auto owner_obj = LockOwner<Object>();
  if (!owner_obj) return animation::Finished;

  // Find the gear via the syncable's sync state
  auto* syncable = static_cast<Syncable::Table*>(iface);
  auto& state = *Syncable(*owner_obj, *syncable).state;
  auto gear = state.end.OwnerLockAs<Gear>();
  if (!gear) return animation::Finished;

  // Find the gear widget
  auto* gear_widget = toy_store.FindOrNull(*gear);
  if (!gear_widget) return animation::Finished;

  // Find the owner object widget
  auto* owner_widget = toy_store.FindOrNull(*owner_obj);
  if (!owner_widget) return animation::Finished;

  end_shape = owner_widget->InterfaceShape(syncable);
  end_shape.transform(TransformBetween(*owner_widget, *gear_widget));
  auto end_bounds = end_shape.getBounds();
  end = end_bounds.center();
  bounds = end_bounds;

  return animation::Animating;
}

void SyncConnectionWidget::Draw(SkCanvas& canvas) const {
  auto& effect = GearShader();
  auto& color = RubberColor();
  auto& normal = RubberNormal();

  float primary_rotation = time::SteadySaw<20.0>() * M_PI * 2;
  SkRuntimeEffectBuilder builder(effect);
  builder.uniform("iRotationRad") = primary_rotation;
  SkMatrix px_to_local;
  (void)canvas.getLocalToDeviceAs3x3().invert(&px_to_local);
  builder.uniform("iPixelRadius") = (float)px_to_local.mapRadius(1);
  builder.uniform("iGearCount") = (float)8;
  builder.uniform("iTeethAmplitudeCm") = (float)(kTeethAmplitude / 1_cm);
  builder.uniform("iRadiusCm") = (float)(kSecondaryGearRadius / 1_cm);
  builder.uniform("iGrooveStartCm") = (float)10.25;  // no groove
  builder.uniform("iGrooveMiddleCm") = (float)10.35;
  builder.uniform("iGrooveEndCm") = (float)10.85;
  builder.uniform("iHoleRadiusCm") = (float)(3_mm / 1_cm);
  builder.uniform("iHoleRoundnessCm") = 0.1f;
  builder.child("iRubberColor") = *color.shader;
  builder.child("iRubberNormal") = *normal.shader;

  auto dir = Normalize(end);
  auto start = dir * (kPrimaryGearRadius + kSecondaryGearRadius);

  float ratio = kPrimaryGearRadius / kSecondaryGearRadius;

  canvas.save();
  canvas.clipPath(end_shape, SkClipOp::kDifference);
  canvas.save();
  canvas.translate(start.x, start.y);

  float rot_offset = atan(dir);
  float secondary_gear_rot = rot_offset * (ratio + 1) + primary_rotation * ratio;

  SkPaint secondary_gear_paint;
  builder.uniform("iRotationRad") = -secondary_gear_rot;
  builder.uniform("iEndPos") = end - start;
  secondary_gear_paint.setShader(builder.makeShader());
  secondary_gear_paint.setStyle(SkPaint::kStroke_Style);
  secondary_gear_paint.setStrokeWidth((kSecondaryGearRadius + kTeethAmplitude) * 2.2);
  secondary_gear_paint.setStrokeCap(SkPaint::kSquare_Cap);
  canvas.drawLine(Vec2(0, 0), end - start, secondary_gear_paint);

  canvas.restore();
  canvas.restore();
}

Optional<Rect> SyncConnectionWidget::TextureBounds() const { return bounds; }

std::unique_ptr<ObjectToy> Gear::MakeToy(ui::Widget* parent) {
  return std::make_unique<GearWidget>(parent, *this);
}

std::unique_ptr<SyncConnectionWidget> SyncMemberOf::MakeToy(ui::Widget* parent) {
  return std::make_unique<SyncConnectionWidget>(parent, object, syncable);
}

// --- Gear serialization ---

void Gear::SerializeState(ObjectSerializer& writer) const {
  writer.Key("members");
  writer.StartObject();
  for (auto& member : members) {
    auto ptr = member.weak.Lock();
    if (!ptr) continue;
    writer.Key(writer.ResolveName(*ptr.Owner<Object>(), ptr.Get()));
    writer.Bool(member.sink);
  }
  writer.EndObject();
}

bool Gear::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "members") {
    Status status;
    for (auto& member_name : ObjectView(d, status)) {
      bool is_sink;
      d.Get(is_sink, status);
      if (!OK(status)) {
        // the value is not a boolean - just skip it
        status.Reset();
      }
      if (!is_sink) continue;
      NestedPtr<Interface::Table> target = d.LookupInterface(member_name);
      if (auto* syncable = dyn_cast_if_present<Syncable::Table>(target.Get())) {
        auto* owner = target.Owner<Object>();
        AddSink(*owner, *syncable);
        AddSource(*owner, *syncable);
      }
    }
    return true;
  }
  return false;
}

}  // namespace automat
