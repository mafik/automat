// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "sync.hh"

#include <include/core/SkClipOp.h>
#include <include/core/SkPaint.h>

#include <mutex>

#include "animation.hh"
#include "argument.hh"
#include "automat.hh"
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

void Syncable::Unsync() {
  auto gear = end.LockAs<Gear>();
  if (!gear) return;
  auto lock = std::unique_lock(gear->mutex);

  auto& members = gear->members;
  for (int i = 0; i < members.size(); ++i) {
    auto* member = members[i].weak.GetUnsafe();
    if (member == this) {
      members.erase(members.begin() + i);
      break;
    }
  }

  source = false;
  end.Reset();
  OnUnsync();
}

Ptr<Gear> FindGearOrMake(NestedPtr<Syncable>& source) {
  auto sync_block = source->end.OwnerLockAs<Gear>();
  if (!sync_block) {
    sync_block = MAKE_PTR(Gear);
    sync_block->AddSource(source);
  }
  return sync_block;
}

Ptr<Gear> FindGearOrNull(NestedPtr<Syncable>& source) {
  auto sync_block = source->end.OwnerLockAs<Gear>();
  if (!sync_block) {
    return nullptr;
  }
  return sync_block;
}

Gear::~Gear() {
  auto lock = std::unique_lock(mutex);
  while (!members.empty()) {
    if (auto member = members.back().weak.Lock()) {
      if (member->source) {
        member->source = false;
        member->end.Reset();
        member->OnUnsync();
      }
    }
    members.pop_back();
  }
}

// Tells that this Syncable will receive notifications (receive-only sync)
void Gear::AddSink(NestedPtr<Syncable>& sink) {
  auto guard = std::unique_lock(mutex);
  for (int i = 0; i < members.size(); ++i) {
    if (members[i].weak == sink) {
      members[i].sink = true;
      return;
    }
  }
  members.emplace_back(sink, true);
}

// Tells that this Syncable is the source of activity / notifications (notify-only sync)
void Gear::AddSource(NestedPtr<Syncable>& source) {
  auto old_sync_block = source->end.LockAs<Gear>();
  bool was_source = source->source;
  if (old_sync_block.Get() != this) {
    source->Connect(*source.Owner<Object>(), AcquirePtr());
    if (old_sync_block) {
      while (!old_sync_block->members.empty()) {
        // stealing all of the members from the old gear
        members.emplace_back(old_sync_block->members.back());
        old_sync_block->members.pop_back();

        // redirecting the members "sync" to this gear
        members.back().weak.Lock()->end = NestedWeakPtr<Atom>(AcquireWeakPtr<Object>(), this);
      }
    } else {
      bool found = false;
      for (int i = 0; i < members.size(); ++i) {
        if (members[i].weak == source) {
          found = true;
          break;
        }
      }
      if (!found) {
        members.emplace_back(source, false);
      }
    }
  }
  if (!was_source) {
    source->source = true;
    source->OnSync();
  }
}

void Gear::FullSync(NestedPtr<Syncable>& syncable) {
  AddSink(syncable);
  AddSource(syncable);
}

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

struct GearWidget : Object::Toy {
  GearWidget(Widget* parent, Object& object) : Object::Toy(parent, object) {}

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

SyncConnectionWidget::SyncConnectionWidget(Widget* parent, Object& object, Syncable& syncable)
    : Toy(parent, object, syncable) {}

SkPath SyncConnectionWidget::Shape() const { return SkPath(); }

animation::Phase SyncConnectionWidget::Tick(time::Timer& t) {
  bounds = Rect{};
  auto& toy_store = ToyStore();

  // Find the gear via the syncable's end pointer
  auto* syncable = dynamic_cast<Syncable*>(atom);
  auto gear = syncable->end.LockAs<Gear>();
  if (!gear) return animation::Finished;

  // Find the gear widget
  auto* gear_widget = toy_store.FindOrNull(*gear);
  if (!gear_widget) return animation::Finished;

  // Find the owner object widget
  auto owner_obj = LockOwner<Object>();
  if (!owner_obj) return animation::Finished;
  auto* owner_widget = toy_store.FindOrNull(*owner_obj);
  if (!owner_widget) return animation::Finished;

  end_shape = owner_widget->AtomShape(syncable);
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

std::unique_ptr<Object::Toy> Gear::MakeToy(ui::Widget* parent) {
  return std::make_unique<GearWidget>(parent, *this);
}

std::unique_ptr<SyncConnectionWidget> SyncMemberOf::MakeToy(ui::Widget* parent) {
  return std::make_unique<SyncConnectionWidget>(parent, object, syncable);
}

void Syncable::CanConnect(Object& start, Atom& end, Status& status) const {
  if (auto* other = dynamic_cast<Syncable*>(&end)) {
    if (CanSync(*other)) {
      return;
    } else {
      auto& msg = AppendErrorMessage(status);
      msg += "Can only connect to ";
      msg += typeid(this).name();
    }
  }
  if (auto gear = dynamic_cast<Gear*>(&end)) {
    auto lock = std::shared_lock(gear->mutex);
    if (gear->members.empty()) {
      return;
    } else {
      auto member = gear->members.front().weak.Lock();
      if (CanSync(*member)) {
        return;
      } else {
        auto& msg = AppendErrorMessage(status);
        msg += "Wrong type of Gear ";
        msg += typeid(this).name();
      }
    }
  }
  AppendErrorMessage(status) += "Can only connect to similar parts";
}

void Syncable::OnConnect(Object& start, const NestedPtr<Atom>& end) {
  InlineArgument::OnConnect(start, end);

  auto* target_syncable = dynamic_cast<Syncable*>(end.Get());
  if (target_syncable) {
    NestedPtr<Syncable> syncable{start.AcquirePtr(), this};
    NestedPtr<Syncable> target{end.GetOwnerPtr(), target_syncable};
    auto sync_block = FindGearOrNull(target);
    if (sync_block == nullptr) {
      sync_block = FindGearOrMake(syncable);
      auto& loc = root_machine->Insert(sync_block);
      loc.position = (end.Owner<Object>()->here->position + start.here->position) / 2;
      loc.ForEachToy([](ui::RootWidget&, Toy& toy) {
        static_cast<LocationWidget&>(toy).position_vel = Vec2(0, 1);
      });
    }
    sync_block->FullSync(syncable);
    sync_block->FullSync(target);
    return;
  }
  auto* gear = dynamic_cast<Gear*>(end.Get());
  if (gear) {
    NestedPtr<Syncable> syncable{start.AcquirePtr(), this};
    gear->FullSync(syncable);
  }
}

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
      NestedPtr<Atom> target = d.LookupAtom(member_name);
      if (auto syncable = target.DynamicCast<Syncable>()) {
        AddSink(syncable);
        AddSource(syncable);
      }
    }
    return true;
  }
  return false;
}

}  // namespace automat
