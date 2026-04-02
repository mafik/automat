// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "sync.hh"

#include <include/core/SkClipOp.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkScalar.h>

#include <cmath>
#include <cstdint>
#include <mutex>

#include "animation.hh"
#include "argument.hh"
#include "automat.hh"
#include "casting.hh"
#include "embedded.hh"
#include "font.hh"
#include "global_resources.hh"
#include "log.hh"
#include "math.hh"
#include "root_widget.hh"
#include "status.hh"
#include "textures.hh"
#include "time.hh"
#include "units.hh"

namespace automat {

// --- Syncable::Table DefaultX functions ---

void Syncable::Table::DefaultCanConnect(Argument self, Interface end, Status& status) {
  auto self_sync = cast<Syncable>(self);
  if (auto other_sync = dyn_cast_if_present<Syncable>(end)) {
    if (self_sync.table->can_sync(self_sync, other_sync)) {
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
      if (member &&
          self_sync.table->can_sync(self_sync, Syncable(member.Owner<Object>(), member.Get()))) {
        return;
      } else {
        AppendErrorMessage(status) += "Wrong type of Gear";
      }
    }
  }
  AppendErrorMessage(status) += "Can only connect to similar parts";
}

void Syncable::Table::DefaultOnConnect(Argument self, Interface end) {
  auto syncable = cast<Syncable>(self);

  syncable.state->gear_weak = dynamic_cast<Gear*>(end.object_ptr);

  if (!end) return;

  auto* target_syncable = dyn_cast_if_present<Syncable::Table>(end.table_ptr);
  if (target_syncable) {
    auto sync_block = FindGearOrNull(*end.object_ptr, *target_syncable);
    if (sync_block == nullptr) {
      sync_block = FindGearOrMake(*self.object_ptr, *syncable.table);
      auto& loc = root_board->Insert(sync_block);
      loc.position = (end.object_ptr->here->position + self.object_ptr->here->position) / 2;
    }
    sync_block->FullSync(*self.object_ptr, *syncable.table);
    sync_block->FullSync(*end.object_ptr, *target_syncable);
    return;
  }
  auto* gear = dynamic_cast<Gear*>(end.object_ptr);
  if (gear) {
    gear->FullSync(*self.object_ptr, *syncable.table);
  }
}

NestedPtr<Interface::Table> Syncable::Table::DefaultFind(Argument self) {
  auto& syncable = static_cast<Syncable::Table&>(*self.table);
  return NestedPtr<Interface::Table>(Syncable(*self.object_ptr, syncable).state->gear_weak.Lock(),
                                     nullptr);
}

// --- Syncable::Unsync ---

void Syncable::Unsync() { state->Unsync(*object_ptr, *table); }

void Syncable::State::Unsync(Object& self, Syncable::Table& syncable) {
  auto gear = gear_weak.Lock();
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
  gear_weak.Reset();
  if (syncable.on_unsync) syncable.on_unsync(Syncable(self, syncable));
}

// --- FindGearOrMake / FindGearOrNull ---

Ptr<Gear> FindGearOrMake(Object& source_obj, Syncable::Table& source) {
  auto& state = *Syncable(source_obj, source).state;
  auto sync_block = state.gear_weak.Lock();
  if (!sync_block) {
    sync_block = MAKE_PTR(Gear);
    sync_block->AddSource(source_obj, source);
  }
  return sync_block;
}

Ptr<Gear> FindGearOrNull(Object& source_obj, Syncable::Table& source) {
  auto& state = *Syncable(source_obj, source).state;
  auto sync_block = state.gear_weak.Lock();
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
        state.gear_weak.Reset();
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
  auto old_sync_block = state.gear_weak.Lock();
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
          member_state.gear_weak = AcquireWeakPtr();
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
constexpr int kPrimaryGearCount = 10;
constexpr float kSecondaryGearRadius = 6_mm;
constexpr int kSecondaryGearCount = 6;
constexpr float kTeethAmplitude = 0.7_mm;
constexpr float kBeltWidth = 6_mm;

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

  uint32_t last_wake_counter = 0;
  float angular_velocity = 0;
  float angle = 0;
  float target_angle = 0;

  animation::Phase Tick(time::Timer& t) override {
    auto gear = LockOwner<Gear>();
    auto wake_counter = gear->wake_counter.load(std::memory_order_relaxed);
    if (wake_counter != last_wake_counter) {
      last_wake_counter = wake_counter;
      target_angle += M_PI / kPrimaryGearCount;  // half tooth
    }
    if (angle >= 2 * M_PI) {
      angle -= 2 * M_PI;
      target_angle -= 2 * M_PI;
    }

    return animation::LowLevelSineTowards(target_angle, t.d, 0.6, angle, angular_velocity);
  }

  void Draw(SkCanvas& canvas) const override {
    auto& effect = GearShader();
    auto& color = RubberColor();
    auto& normal = RubberNormal();

    SkRuntimeEffectBuilder builder(effect);
    builder.uniform("iRotationRad") = angle;
    builder.uniform("iScrollRatio") = angle / (2 * M_PI) + 0.5;
    SkMatrix px_to_local;
    (void)canvas.getLocalToDeviceAs3x3().invert(&px_to_local);
    builder.uniform("iPixelRadius") = (float)px_to_local.mapRadius(1);
    builder.uniform("iGearCount") = (float)kPrimaryGearCount;
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

SyncBelt::SyncBelt(Widget* parent, Object& object, Syncable::Table& syncable)
    : ArgumentToy(parent, object, &syncable) {}

SkPath SyncBelt::Shape() const {
  return SkPath::Circle(pinion.x, pinion.y, kSecondaryGearRadius + kTeethAmplitude);
}

std::unique_ptr<Action> SyncBelt::FindAction(ui::Pointer& pointer, ui::ActionTrigger trigger) {
  auto owner = LockOwner<Object>();
  if (!owner) return nullptr;
  auto syncable = Bind<Syncable>(*owner);
  if (trigger == ui::PointerButton::Left) {
    return std::make_unique<SyncAction>(pointer, syncable);
  }
  return nullptr;
}

animation::Phase SyncBelt::Tick(time::Timer& t) {
  auto phase = animation::Finished;
  auto& toy_store = ToyStore();

  // Check if the object of this connection still exists.
  auto syncable = LockBind<Syncable>();
  if (!syncable) return animation::Finished;

  auto sync_balance = syncable.state->sync_balance;
  if (sync_balance != last_sync_balance) {
    int32_t diff = (int)(sync_balance - last_sync_balance) > 0 ? 1 : -1;
    target_scroll_ratio += (float)(diff) / kPrimaryGearCount / 2;
    last_sync_balance = sync_balance;
  }

  phase |= animation::LowLevelSineTowards(target_scroll_ratio, t.d, 0.6, scroll_ratio,
                                          scroll_ratio_velocity);
  // Keeping the animation close to 0 makes it smoother.
  if (fabs(scroll_ratio) > 0) {
    float wholes = truncf(scroll_ratio);
    target_scroll_ratio -= wholes;
    scroll_ratio -= wholes;
  }

  // Find the owner object widget
  auto* owner_widget = toy_store.FindOrNull(*syncable.object_ptr);
  if (!owner_widget) return animation::Finished;

  auto origin_shape = owner_widget->Shape();
  origin_shape.transform(TransformBetween(*owner_widget, *this));
  origin = origin_shape.getBounds().center();
  if (auto new_label = syncable.Name(); label != new_label) {
    label = new_label;

    auto typeface = ui::Font::GetPbio();
    float letter_size = kBeltWidth * 0.6f;
    float vertical_margin = (kBeltWidth - letter_size) / 2;
    auto font = ui::Font::MakeV2(typeface, letter_size);
    float text_width = font->MeasureText(label);
    float loop_length = 6_cm;
    auto cull_rect = SkRect::MakeWH(loop_length, kBeltWidth * 2);

    SkPaint text_paint;
    text_paint.setColor(SK_ColorWHITE);

    SkPictureRecorder recorder;
    SkCanvas* text_canvas = recorder.beginRecording(loop_length, kBeltWidth);

    text_canvas->save();
    text_canvas->translate((loop_length - text_width) / 2, vertical_margin);
    font->DrawText(*text_canvas, label, text_paint);
    text_canvas->restore();

    text_canvas->rotate(180, loop_length / 2, kBeltWidth);

    text_canvas->save();
    text_canvas->translate((loop_length - text_width) / 2, vertical_margin);
    font->DrawText(*text_canvas, label, text_paint);
    text_canvas->restore();

    auto text_picture = recorder.finishRecordingAsPictureWithCull(cull_rect);
    label_shader = text_picture->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                            SkFilterMode::kLinear, nullptr, nullptr);
  }

  // Find the gear via the syncable's sync state
  auto& state = syncable.state;
  auto gear = state->gear_weak.Lock();
  if (gear) {
    // Find the gear widget
    auto* gear_widget = toy_store.FindOrNull(*gear);
    if (gear_widget) {
      angle = gear_widget->angle;
      if (gear_widget->IsAnimating()) {
        phase |= animation::Animating;
      }
      auto gear_matrix = TransformBetween(*gear_widget, *this);
      Vec2 gear_origin = gear_matrix.mapOrigin();
      float new_scale = gear_matrix.mapRadius(1);
      if (new_scale != scale) {
        scale = new_scale;
        phase |= animation::Animating;
      }
      auto dir = Normalize(origin - gear_origin);
      float gear_dist = (kPrimaryGearRadius + kSecondaryGearRadius) * scale;
      pinion_deflection.InteractiveTargetUpdate(pinion, dir * gear_dist + gear_origin);

      // Pinion bouncing off the gear
      auto effective_pinion = pinion + pinion_deflection;
      auto delta = gear_origin - effective_pinion;
      auto dist = Length(delta);
      if (dist > 0 && dist < gear_dist) {
        auto normal = -delta / dist;  // points from gear to pinion
        float penetration = gear_dist - dist;
        pinion_deflection.value += normal * penetration;
        float v_dot_n = Dot(pinion_deflection.velocity, normal);
        if (v_dot_n < 0) {
          pinion_deflection.velocity -= normal * v_dot_n;  // sticky collision
        }
        phase |= animation::Animating;
      }
    }
  } else if (is_dragged) {
    phase |= animation::ExponentialApproach(1, t.d, 0.1, scale);
  } else {
    pinion_deflection.SmoothTargetUpdate(pinion, origin);
  }

  phase |= animation::LowLevelSineTowards(pinion.x <= origin.x ? 0 : 1, t.d, 0.5,
                                          label_rotation_ratio, label_rotation_velocity);

  phase |= pinion_deflection.SpringTowards({}, t.d, 0.3, 0.05);
  return phase;
}

Vec<Vec2> SyncBelt::TextureAnchors() {
  time::Timer t;
  t.last = t.now = time::SteadyNow();
  t.d = 0;
  Tick(t);
  return {pinion + pinion_deflection, origin};
}

void SyncBelt::Draw(SkCanvas& canvas) const {
  auto& effect = GearShader();
  auto& color = RubberColor();
  auto& normal = RubberNormal();

  SkRuntimeEffectBuilder builder(effect);
  builder.uniform("iRotationRad") = angle;
  SkMatrix px_to_local;
  (void)canvas.getLocalToDeviceAs3x3().invert(&px_to_local);
  builder.uniform("iPixelRadius") = (float)px_to_local.mapRadius(1);
  builder.uniform("iGearCount") = (float)kSecondaryGearCount;
  builder.uniform("iTeethAmplitudeCm") = (float)(kTeethAmplitude / 1_cm * scale);
  builder.uniform("iRadiusCm") = (float)(kSecondaryGearRadius / 1_cm * scale);
  builder.uniform("iGrooveStartCm") = (float)10.25;  // no groove
  builder.uniform("iGrooveMiddleCm") = (float)10.35;
  builder.uniform("iGrooveEndCm") = (float)10.85;
  builder.uniform("iHoleRadiusCm") = (float)(kBeltWidth / 2 / 1_cm * scale);
  builder.uniform("iHoleRoundnessCm") = 0.1f * scale;
  builder.child("iRubberColor") = *color.shader;
  builder.child("iRubberNormal") = *normal.shader;
  SkMatrix m;
  m.setTranslateY(-label_rotation_ratio * kBeltWidth * scale);
  m.preScale(scale, scale);
  builder.child("iLabel") = label_shader->makeWithLocalMatrix(m);

  auto pinion_effective = pinion + pinion_deflection;
  auto pinion_to_origin = origin - pinion_effective;

  float ratio = (float)kPrimaryGearCount / kSecondaryGearCount;

  canvas.save();
  canvas.translate(pinion_effective.x, pinion_effective.y);

  auto dir = Normalize(pinion_to_origin);
  float rot_offset = atan(dir);
  float secondary_gear_rot = rot_offset * (ratio + 1) + angle * ratio;

  SkPaint secondary_gear_paint;
  builder.uniform("iRotationRad") = -secondary_gear_rot;
  builder.uniform("iScrollRatio") = scroll_ratio;
  builder.uniform("iEndPos") = pinion_to_origin;
  secondary_gear_paint.setShader(builder.makeShader());
  secondary_gear_paint.setStyle(SkPaint::kStroke_Style);
  secondary_gear_paint.setStrokeWidth((kSecondaryGearRadius + kTeethAmplitude) * 2.2);
  secondary_gear_paint.setStrokeCap(SkPaint::kSquare_Cap);
  canvas.drawLine(Vec2(0, 0), pinion_to_origin, secondary_gear_paint);

  canvas.restore();
}

Optional<Rect> SyncBelt::TextureBounds() const {
  constexpr float r = kSecondaryGearRadius + kTeethAmplitude;
  auto bounds = Rect::MakeCenter(pinion + pinion_deflection, r * 2, r * 2);
  bounds.ExpandToInclude(Rect::MakeCenter(origin, 6_mm, 6_mm));
  return bounds;
}

std::unique_ptr<ObjectToy> Gear::MakeToy(ui::Widget* parent) {
  return std::make_unique<GearWidget>(parent, *this);
}

std::unique_ptr<SyncBelt> Syncable::MakeToy(ui::Widget* parent) {
  return std::make_unique<SyncBelt>(parent, *object_ptr, *table);
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

SyncAction::SyncAction(ui::Pointer& pointer, Syncable syncable) : Action(pointer) {
  syncable.Unsync();
  auto& sync_widget = pointer.root_widget.toys.FindOrMake(syncable, &pointer.root_widget);
  sync_widget.is_dragged = true;
  sync_widget.pinion_deflection.SmoothTargetUpdate(sync_widget.pinion,
                                                   pointer.PositionWithinRootBoard());
  sync_widget.WakeAnimation();
  Update();
  this->weak = NestedWeakPtr<Syncable::Table>(syncable.GetOwner().AcquireWeakPtr(), syncable.table);
  pointer.root_widget.WakeAnimation();
}
SyncAction::~SyncAction() {
  // Check if the pointer is over a compatible Syncable
  if (auto syncable_ptr = weak.Lock()) {
    Syncable syncable(syncable_ptr.Owner<Object>(), syncable_ptr.Get());
    auto* sync_widget = pointer.root_widget.toys.FindOrNull(syncable);
    if (sync_widget) {
      sync_widget->is_dragged = false;
      sync_widget->WakeAnimation();
      auto* bw = pointer.root_widget.toys.FindOrNull(*root_board);
      if (bw) {
        bw->ConnectAtPoint(syncable, sync_widget->pinion);
        // TODO: animate towards the true target rather than origin
        sync_widget->pinion_deflection.SmoothTargetUpdate(sync_widget->pinion, sync_widget->origin);
      }
    }
  }
}
void SyncAction::Update() {
  if (auto syncable_ptr = weak.Lock()) {
    Syncable syncable(syncable_ptr.Owner<Object>(), syncable_ptr.Get());
    auto* origin_widget = pointer.root_widget.toys.FindOrNull(*syncable.object_ptr);
    auto* bw = pointer.root_widget.toys.FindOrNull(*root_board);
    auto start_local = origin_widget->Shape().getBounds().center();
    auto start = bw ? TransformBetween(*origin_widget, *bw).mapPoint(start_local) : start_local;
    if (auto* sync_widget = pointer.root_widget.toys.FindOrNull(syncable)) {
      sync_widget->origin = start;
      sync_widget->pinion_deflection.InteractiveTargetUpdate(sync_widget->pinion,
                                                             pointer.PositionWithinRootBoard());
      sync_widget->WakeAnimation();
    }
  } else {
    pointer.ReplaceAction(*this, nullptr);
  }
}
bool SyncAction::Highlight(Interface end) const {
  auto ptr = weak.Lock();
  Object& start = *ptr.Owner<Object>();
  return Argument(start, *ptr).CanConnect(end);
}
ui::Widget* SyncAction::Widget() { return nullptr; }

}  // namespace automat
