// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "sync.hh"

#include <include/core/SkClipOp.h>
#include <include/core/SkPaint.h>

#include <mutex>

#include "animation.hh"
#include "embedded.hh"
#include "global_resources.hh"
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
        members.back().weak.Lock()->end = NestedWeakPtr<Part>(AcquireWeakPtr<Object>(), this);
      }
    } else {
      members.emplace_back(source, false);
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

struct GearWidget : Object::WidgetBase {
  Rect bounds;

  struct Belt {
    Object* object_unsafe = nullptr;
    Syncable* syncable_unsafe = nullptr;
    SkPath end_shape;
    Vec2 end{};
    bool sink = false;
    bool source = false;

    Belt(Object* object_unsafe, Syncable* syncable_unsafe)
        : object_unsafe(object_unsafe), syncable_unsafe(syncable_unsafe) {}
  };

  std::vector<Belt> belts;

  GearWidget(Gear& sync_block, Widget* parent) : WidgetBase(parent) {
    object = sync_block.AcquireWeakPtr();
  }

  SkPath Shape() const override { return SkPath::Circle(0, 0, 1_cm); }

  bool CenteredAtZero() const override { return true; }

  animation::Phase Tick(time::Timer& t) override {
    bounds = Shape().getBounds();
    auto& widget_store = WidgetStore();
    auto gear = LockObject<Gear>();
    belts.clear();

    auto SetBeltEnd = [&](Belt& belt) {
      if (auto owner_widget = widget_store.FindOrNull(*belt.object_unsafe)) {
        belt.end_shape = owner_widget->PartShape(belt.syncable_unsafe);
        belt.end_shape.transform(TransformBetween(*owner_widget, *this));
        auto end_bounds = belt.end_shape.getBounds();
        belt.end = end_bounds.center();
        bounds.ExpandToInclude(end_bounds);
      }
    };

    if (gear) {
      for (auto& member : gear->members) {
        if (auto ptr = member.weak.Lock()) {
          auto* owner = ptr.Owner<Object>();
          auto* belt = &belts.emplace_back(owner, ptr.Get());
          SetBeltEnd(*belt);
          belt->sink = member.sink;
          belt->source = ptr->source;
        }
      }
    }
    return animation::Animating;
  }

  constexpr static float kPrimaryGearRadius = 9_mm;
  constexpr static float kSecondaryGearRadius = 6_mm;
  constexpr static float kTeethAmplitude = 0.7_mm;

  void Draw(SkCanvas& canvas) const override {
    Status status;
    static auto effect = resources::CompileShader(embedded::assets_gear_sksl, status);
    if (!OK(status)) {
      FATAL << status;
    }

    static auto color = PersistentImage::MakeFromAsset(embedded::assets_rubber_color_webp,
                                                       {
                                                           .scale = 1,
                                                           .tile_x = SkTileMode::kRepeat,
                                                           .tile_y = SkTileMode::kRepeat,
                                                       });
    static auto normal = PersistentImage::MakeFromAsset(embedded::assets_rubber_normal_webp,
                                                        {
                                                            .scale = 1,
                                                            .tile_x = SkTileMode::kRepeat,
                                                            .tile_y = SkTileMode::kRepeat,
                                                            .raw_shader = true,
                                                        });

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

    builder.uniform("iGearCount") = (float)8;
    builder.uniform("iRadiusCm") = (float)(kSecondaryGearRadius / 1_cm);
    builder.uniform("iGrooveStartCm") = (float)10.25;  // no groove
    builder.uniform("iGrooveMiddleCm") = (float)10.35;
    builder.uniform("iGrooveEndCm") = (float)10.85;
    builder.uniform("iHoleRadiusCm") = (float)(3_mm / 1_cm);
    builder.uniform("iHoleRoundnessCm") = 0.1f;

    SkPaint belt_paint;
    belt_paint.setColor("#404040"_color);
    belt_paint.setStyle(SkPaint::kStroke_Style);
    belt_paint.setStrokeWidth(6_mm);
    for (auto& belt : belts) {
      auto dir = Normalize(belt.end);
      auto start = dir * (kPrimaryGearRadius + kSecondaryGearRadius);

      float ratio = kPrimaryGearRadius / kSecondaryGearRadius;

      canvas.save();
      canvas.clipPath(belt.end_shape, SkClipOp::kDifference);
      canvas.save();
      canvas.translate(start.x, start.y);

      float rot_offset = atan(dir);

      float secondary_gear_rot = rot_offset * (ratio + 1) + primary_rotation * ratio;

      SkPaint secondary_gear_paint;
      builder.uniform("iRotationRad") = -secondary_gear_rot;
      builder.uniform("iEndPos") = belt.end - start;
      secondary_gear_paint.setShader(builder.makeShader());
      secondary_gear_paint.setStyle(SkPaint::kStroke_Style);
      secondary_gear_paint.setStrokeWidth((kSecondaryGearRadius + kTeethAmplitude) * 2.2);
      secondary_gear_paint.setStrokeCap(SkPaint::kSquare_Cap);
      canvas.drawLine(Vec2(0, 0), belt.end - start, secondary_gear_paint);

      canvas.restore();

      canvas.restore();
    }
  }

  Optional<Rect> TextureBounds() const override { return bounds; }
};

std::unique_ptr<ObjectWidget> Gear::MakeWidget(ui::Widget* parent) {
  return std::make_unique<GearWidget>(*this, parent);
}

void Syncable::CanConnect(Object& start, Part& end, Status& status) const {
  if (dynamic_cast<Gear*>(&end) != nullptr) {
    AppendErrorMessage(status) += "Can only connect to Gear";
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
      NestedPtr<Part> target = d.LookupPart(member_name);
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
