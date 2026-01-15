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
#include "time.hh"
#include "units.hh"

namespace automat {

void Syncable::Unsync() {
  auto sync_block = end.LockAs<Gear>();
  if (!sync_block) return;
  auto lock = std::unique_lock(sync_block->mutex);

  auto& sources = sync_block->sources;
  for (int i = 0; i < sources.size(); ++i) {
    auto* source = sources[i].GetUnsafe();
    if (source == this) {
      sources.erase(sources.begin() + i);
      break;
    }
  }

  auto& sinks = sync_block->sinks;
  for (int i = 0; i < sinks.size(); ++i) {
    auto* sink = sinks[i].GetUnsafe();
    if (sink == this) {
      sinks.erase(sinks.begin() + i);
      break;
    }
  }

  end.Reset();
  OnUnsync();
}

Ptr<Gear> Sync(NestedPtr<Syncable>& source) {
  auto sync_block = source->end.OwnerLockAs<Gear>();
  if (!sync_block) {
    sync_block = MAKE_PTR(Gear);
    sync_block->AddSource(source);
  }
  return sync_block;
}

Gear::~Gear() {
  auto lock = std::unique_lock(mutex);
  while (!sources.empty()) {
    if (auto source = sources.back().Lock()) {
      source->OnUnsync();
    }
    sources.pop_back();
  }
}

// Tells that this Syncable will receive notifications (receive-only sync)
void Gear::AddSink(NestedPtr<Syncable>& sink) {
  auto guard = std::unique_lock(mutex);
  for (int i = 0; i < sinks.size(); ++i) {
    if (sinks[i] == sink) {
      return;
    }
  }
  sinks.push_back(sink);
}

// Tells that this Syncable is the source of activity / notifications (notify-only sync)
void Gear::AddSource(NestedPtr<Syncable>& source) {
  auto old_sync_block = source->end.LockAs<Gear>();
  if (old_sync_block.Get() == this) {
    return;
  }
  source->Connect(*source.Owner<Object>(), AcquirePtr());
  sources.push_back(source);
  if (!old_sync_block) {
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
      for (auto& sink_weak : gear->sinks) {
        if (auto sink = sink_weak.Lock()) {
          auto* owner = sink.Owner<Object>();
          auto* belt = &belts.emplace_back(owner, sink.Get());
          SetBeltEnd(*belt);
          belt->sink = true;
        }
      }
      for (auto& source_weak : gear->sources) {
        if (auto source = source_weak.Lock()) {
          auto* owner = source.Owner<Object>();
          Belt* belt = nullptr;
          for (Belt& existing : belts) {
            if (existing.object_unsafe == owner && existing.syncable_unsafe == source.Get()) {
              belt = &existing;
              break;
            }
          }
          if (belt == nullptr) {
            belt = &belts.emplace_back(owner, source.Get());
            SetBeltEnd(*belt);
          }
          belt->source = true;
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
      secondary_gear_paint.setStrokeWidth((kSecondaryGearRadius + kTeethAmplitude) * 2);
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
  writer.Key("sinks");
  writer.StartArray();
  for (auto& sink_weak : sinks) {
    auto sink = sink_weak.Lock();
    if (!sink) continue;
    writer.String(writer.ResolveName(*sink.Owner<Object>(), sink.Get()));
  }
  writer.EndArray();
}

bool Gear::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "sinks") {
    Status status;
    for (int i : ArrayView(d, status)) {
      Str sink_name;
      d.Get(sink_name, status);
      NestedPtr<Part> target = d.LookupPart(sink_name);
      if (auto syncable = target.DynamicCast<Syncable>()) {
        AddSink(syncable);
      }
    }
    return true;
  }
  return false;
}

}  // namespace automat
