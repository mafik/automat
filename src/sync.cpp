// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "sync.hpp"

#include <include/core/SkClipOp.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkScalar.h>

#include <cmath>
#include <cstdint>
#include <mutex>

#include "animation.hpp"
#include "argument.hpp"
#include "automat.hpp"
#include "board.hpp"
#include "casting.hpp"
#include "embedded.hpp"
#include "font.hpp"
#include "global_resources.hpp"
#include "log.hpp"
#include "math.hpp"
#include "menu.hpp"
#include "root_widget.hpp"
#include "status.hpp"
#include "syncable.hpp"
#include "tasks.hpp"
#include "textures.hpp"
#include "time.hpp"
#include "units.hpp"

namespace automat {

static void Link(Gear& gear, Sync::Table& sync) {
  auto lock = std::unique_lock(gear.mutex);
  if (gear.members == nullptr) {
    sync.prev = sync.next = &sync;
    gear.members = &sync;
  } else {
    sync.prev = gear.members->prev;
    sync.next = gear.members;
    sync.prev->next = &sync;
    sync.next->prev = &sync;
  }
}

static void Unlink(Sync::Table& sync) {
  Ptr<Gear> gear = std::move(sync.gear);
  if (!gear) return;
  auto lock = std::unique_lock(gear->mutex);
  if (sync.next == &sync) {
    gear->members = nullptr;
  } else {
    sync.prev->next = sync.next;
    sync.next->prev = sync.prev;
    if (gear->members == &sync) gear->members = sync.next;
  }
  sync.prev = sync.next = nullptr;
}

Syncable Sync::OnOffTable::SyncableOf() {
  if (wrapper.syncable_idx == 0) return {};
  return object->InterfaceFromIdx<Syncable>(wrapper.syncable_idx);
}

static void NotifySync(Sync::Table& sync) {
  if (auto syncable = sync.SyncableOf(); syncable && syncable.table->on_sync) {
    syncable.table->on_sync(syncable);
  }
}

static void NotifyUnsync(Sync::Table& sync) {
  if (auto syncable = sync.SyncableOf(); syncable && syncable.table->on_unsync) {
    syncable.table->on_unsync(syncable);
  }
}

static void Leave(Sync::Table& sync) {
  if (!sync.gear) return;
  sync.gear->WakeToys();
  if (sync.source) NotifyUnsync(sync);
  Unlink(sync);
  sync.object->WakeToys();
}

static void Join(Sync::Table& sync, Gear& gear) {
  if (sync.gear.Get() == &gear) return;
  Leave(sync);
  Link(gear, sync);
  sync.gear = gear.AcquirePtr();
  if (sync.source) NotifySync(sync);
  gear.WakeToys();
  sync.object->WakeToys();
}

static Locked<Command> CallBelow(Sync::Table& partner, int idx, Command::Table& wrapper) {
  if (partner.tail == nullptr || idx == 0) return {};
  auto* below = dyn_cast_if_present<Command::Table>((*partner.tail)[idx]);
  if (below == nullptr) return {};
  return below->on_run(Command(*partner.object, wrapper), *below);
}

Locked<Command> Sync::CommandTable::Start() {
  return CallBelow(*this, attached_toc_idx, cmd_wrapper);
}

Locked<Command> Sync::CommandTable::End() {
  return CallBelow(*this, cmd_wrapper.cancel_idx, cancel_wrapper);
}

Locked<Command> Sync::OnOffTable::Start() {
  return CallBelow(*this, wrapper.turn_on_idx, on_wrapper);
}

Locked<Command> Sync::OnOffTable::End() {
  return CallBelow(*this, wrapper.turn_off_idx, off_wrapper);
}

static void CallPartners(Sync::Table& sync, Gear& gear, Locked<Command> (Sync::Table::*call)()) {
  SmallVec<Locked<Sync>, 8> partners;
  {
    auto lock = std::shared_lock(gear.mutex);
    for (auto* partner = sync.next; partner && partner != &sync; partner = partner->next) {
      if (!partner->sink || !partner->object->IncrementOwningRefsNonZero()) continue;
      partners.push_back(AdoptLocked(Sync(*partner->object, *partner)));
    }
  }
  --sync.sync_balance;
  sync.object->WakeToys();
  gear.WakeToys();
  for (auto& partner : partners) {
    Sync::Table& partner_sync = *partner.table;
    if (auto next = (partner_sync.*call)()) next.ScheduleRun();
    ++partner_sync.sync_balance;
    partner.object_ptr->WakeToys();
  }
}

static Locked<Command> Forward(Sync::Table& sync, Command self, Command::Table& below,
                               Locked<Command> (Sync::Table::*call)()) {
  Ptr<Gear> gear = sync.gear;
  if (sync.source && gear) {
    if (auto suspend = Suspend(*gear)) {
      CallPartners(sync, *gear, call);
      return below.on_run(self, below);
    }
  }
  return below.on_run(self, below);
}

static Locked<Command> RunOwn(Command self, Command::Table& true_self) {
  auto& sync = OUTER_REF(Sync::CommandTable, cmd_wrapper, true_self);
  auto& below = *cast<Command::Table>((*sync.tail)[sync.attached_toc_idx]);
  return Forward(sync, self, below, &Sync::Table::Start);
}

static Locked<Command> RunCancel(Command self, Command::Table& true_self) {
  auto& sync = OUTER_REF(Sync::CommandTable, cancel_wrapper, true_self);
  auto& below = *cast<Command::Table>((*sync.tail)[sync.cmd_wrapper.cancel_idx]);
  return Forward(sync, self, below, &Sync::Table::End);
}

static Locked<Command> RunCompleted(Command self, Command::Table& true_self) {
  auto& sync = OUTER_REF(Sync::CommandTable, completion_detector, true_self);
  if (sync.tail == nullptr) return {};
  Ptr<Gear> gear = sync.gear;
  if (sync.source && gear) {
    if (auto suspend = Suspend(*gear)) CallPartners(sync, *gear, &Sync::Table::End);
  }
  auto& below = *cast<Command::Table>((*sync.tail)[sync.attached_toc_idx]);
  if (below.next_idx == 0) return {};
  auto* next = dyn_cast_if_present<Argument::Table>((*sync.tail)[below.next_idx]);
  if (next == nullptr) return {};
  return FollowArgument(Argument(*self.object_ptr, *next));
}

static Locked<Interface> FindCompleted(Argument self) {
  auto& sync = OUTER_REF(Sync::CommandTable, completion_arg, *self.table_ptr);
  return AcquireLocked(Interface(*self.object_ptr, sync.completion_detector));
}

static Locked<Command> RunOn(Command self, Command::Table& true_self) {
  auto& sync = OUTER_REF(Sync::OnOffTable, on_wrapper, true_self);
  auto& below = *cast<Command::Table>((*sync.tail)[sync.wrapper.turn_on_idx]);
  return Forward(sync, self, below, &Sync::Table::Start);
}

static Locked<Command> RunOff(Command self, Command::Table& true_self) {
  auto& sync = OUTER_REF(Sync::OnOffTable, off_wrapper, true_self);
  auto& below = *cast<Command::Table>((*sync.tail)[sync.wrapper.turn_off_idx]);
  return Forward(sync, self, below, &Sync::Table::End);
}

static Sync::Table& HookOf(Interface::Table& table) {
  if (isa<OnOff::Table>(&table)) return OUTER_REF(Sync::OnOffTable, wrapper, table);
  return OUTER_REF(Sync::CommandTable, cmd_wrapper, table);
}

static void OwnFillMenu(Interface self, Interface::Table& true_self, Menu& menu) {
  auto& sync = HookOf(true_self);
  auto* below = (*sync.tail)[sync.attached_toc_idx];
  if (below->fill_menu) below->fill_menu(self, *below, menu);
  menu.Place(ui::Dir::E, Interface(*self.object_ptr, sync));
}

Sync::CommandTable::CommandTable() {
  completion_arg.style = Style::Invisible;
  completion_arg.visible_when_disconnected = false;
  completion_arg.is_connected = [](Argument) { return false; };
  completion_arg.find = &FindCompleted;
  completion_detector.on_run = &RunCompleted;
}

void Sync::CommandTable::Attach(Object& obj, int toc_idx) {
  assert(tail == nullptr);
  assert(attached_toc_idx == 0);
  attached_toc_idx = toc_idx;
  object = &obj;
  auto* old = obj.toc.load(std::memory_order_relaxed);
  do {
    buffer.assign(old->begin(), old->end());
    cmd_wrapper = *cast<Command::Table>((*old)[toc_idx]);
    cmd_wrapper.on_run = &RunOwn;
    cmd_wrapper.fill_menu = &OwnFillMenu;
    cmd_wrapper.next_idx = buffer.size() + 2;
    buffer[toc_idx - 1] = &cmd_wrapper;
    if (cmd_wrapper.cancel_idx) {
      cancel_wrapper = *cast<Command::Table>((*old)[cmd_wrapper.cancel_idx]);
      cancel_wrapper.on_run = &RunCancel;
      buffer[cmd_wrapper.cancel_idx - 1] = &cancel_wrapper;
    }
    buffer.push_back(this);
    buffer.push_back(&completion_arg);
    buffer.push_back(&completion_detector);
    array = buffer;
    tail = old;
  } while (!obj.toc.compare_exchange_weak(old, this, std::memory_order_release,
                                          std::memory_order_relaxed));
  ++obj.hook_count;
}

void Sync::OnOffTable::Attach(Object& obj, int toc_idx) {
  assert(tail == nullptr);
  assert(attached_toc_idx == 0);
  attached_toc_idx = toc_idx;
  object = &obj;
  auto* old = obj.toc.load(std::memory_order_relaxed);
  do {
    buffer.assign(old->begin(), old->end());
    wrapper = *cast<OnOff::Table>((*old)[toc_idx]);
    wrapper.fill_menu = &OwnFillMenu;
    buffer[toc_idx - 1] = &wrapper;
    if (wrapper.turn_on_idx) {
      on_wrapper = *cast<Command::Table>((*old)[wrapper.turn_on_idx]);
      on_wrapper.on_run = &RunOn;
      buffer[wrapper.turn_on_idx - 1] = &on_wrapper;
    }
    if (wrapper.turn_off_idx) {
      off_wrapper = *cast<Command::Table>((*old)[wrapper.turn_off_idx]);
      off_wrapper.on_run = &RunOff;
      buffer[wrapper.turn_off_idx - 1] = &off_wrapper;
    }
    buffer.push_back(this);
    array = buffer;
    tail = old;
  } while (!obj.toc.compare_exchange_weak(old, this, std::memory_order_release,
                                          std::memory_order_relaxed));
  ++obj.hook_count;
}

int Sync::Table::Detach(Object& obj) {
  int toc_idx = attached_toc_idx;
  obj.toc.store(tail, std::memory_order_release);
  tail = nullptr;
  attached_toc_idx = 0;
  --obj.hook_count;
  return toc_idx;
}

Sync::Table::~Table() { Unlink(*this); }

static Sync::Table& FindOrMakeSyncTable(Command command) {
  Object& object = *command.object_ptr;
  auto lock = std::lock_guard(object.hooks_lock);
  int idx = object.InterfaceIdx(command.table_ptr);
  assert(idx > 0);
  auto* current = (*object.toc.load(std::memory_order_relaxed))[idx];
  if (current->fill_menu == &OwnFillMenu) return HookOf(*current);
  Sync::Table* sync;
  if (isa<OnOff::Table>(current)) {
    sync = new Sync::OnOffTable();
  } else {
    sync = new Sync::CommandTable();
  }
  sync->Attach(object, idx);
  return *sync;
}

static Ptr<Gear> MakeGearBetween(Object& object, Object& other) {
  auto gear = MAKE_PTR(Gear);
  auto home = object.HomeLocation();
  Ptr<Board> board = home ? home->LockBoard() : nullptr;
  if (!board) board = DefaultBoard().AcquirePtr();
  auto& location = board->Insert(gear);
  if (home) {
    auto other_home = board->LocationOrNull(other);
    location.placement = Location::PlaceBetween{home->AcquireWeakPtr(),
                                                (other_home ? other_home : home)->AcquireWeakPtr()};
  }
  board->WakeToys();
  return gear;
}

void Sync::Table::DefaultCanConnect(Argument self, Interface end, Status& status) {
  if (end.object_ptr == self.object_ptr) {
    AppendErrorMessage(status) += "Can only sync with another object";
    return;
  }
  if (dynamic_cast<Gear*>(end.object_ptr)) return;
  auto* command = dyn_cast_if_present<Command::Table>(end.table_ptr);
  if (command && command->on_run != &RunCompleted) return;
  AppendErrorMessage(status) += "Can only sync with a Command";
}

void Sync::Table::DefaultOnConnect(Argument self, Interface end) {
  auto& sync = static_cast<Sync::Table&>(*self.table);
  if (!end.has_object()) {
    Leave(sync);
    return;
  }
  if (auto* gear = dynamic_cast<Gear*>(end.object_ptr)) {
    Join(sync, *gear);
    return;
  }
  auto& end_sync = FindOrMakeSyncTable(cast<Command>(end));
  if (end_sync.gear) {
    Join(sync, *end_sync.gear);
    return;
  }
  auto gear = MakeGearBetween(*self.object_ptr, *end.object_ptr);
  Join(end_sync, *gear);
  Join(sync, *gear);
}

Locked<Interface> Sync::Table::DefaultFind(Argument self) {
  auto& sync = static_cast<Sync::Table&>(*self.table);
  if (Ptr<Gear> gear = sync.gear) return AdoptLocked(Interface(*gear.Release()));
  return {};
}

std::unique_ptr<Action> Sync::Table::SyncActivate(Span<Interface> chain, ui::Pointer& pointer,
                                                  automat::Toy* toy) {
  return std::make_unique<SyncAction>(pointer, chain.back(), toy);
}

static std::unique_ptr<Action> SyncCaller(Span<Interface> chain, ui::Pointer& pointer, Toy* toy) {
  if (chain.size() < 2) return nullptr;
  return std::make_unique<SyncAction>(pointer, chain[chain.size() - 2], toy);
}

constinit Interface::Table kSync = [] {
  Interface::Table t(Interface::kMenu, "Sync");
  t.activate = &SyncCaller;
  return t;
}();

std::unique_ptr<SyncBelt> Sync::MakeToy(ui::Widget* parent) {
  return std::make_unique<SyncBelt>(parent, *object_ptr, *table);
}

Gear::~Gear() { assert(members == nullptr); }

static void AddSinkTable(Gear& gear, Sync::Table& sync) {
  if (sync.gear.Get() == &gear) {
    auto lock = std::unique_lock(gear.mutex);
    sync.sink = true;
    return;
  }
  sync.sink = true;
  sync.source = false;
  Join(sync, gear);
}

static void AddSourceTable(Gear& gear, Sync::Table& sync) {
  if (sync.gear.Get() == &gear) {
    if (sync.source) return;
    {
      auto lock = std::unique_lock(gear.mutex);
      sync.source = true;
    }
    NotifySync(sync);
    return;
  }
  sync.sink = false;
  sync.source = true;
  Join(sync, gear);
}

void Gear::AddSink(Command command) { AddSinkTable(*this, FindOrMakeSyncTable(command)); }

void Gear::AddSource(Command command) { AddSourceTable(*this, FindOrMakeSyncTable(command)); }

void Gear::FullSync(Command command) {
  auto& sync = FindOrMakeSyncTable(command);
  AddSinkTable(*this, sync);
  AddSourceTable(*this, sync);
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

  uint32_t last_monitor = 0;
  float angular_velocity = 0;
  float angle = 0;
  float target_angle = 0;

  Tock Tick(time::Timer& t) override {
    if (auto gear = LockOwner<Gear>()) {
      auto monitor = gear->monitor.load(std::memory_order_relaxed);
      if (monitor != last_monitor) {
        last_monitor = monitor;
        target_angle += M_PI / kPrimaryGearCount;  // half tooth
      }
      if (angle >= 2 * M_PI) {
        angle -= 2 * M_PI;
        target_angle -= 2 * M_PI;
      }
    }

    Tock tock;
    tock.drawing |= animation::LowLevelSineTowards(target_angle, t.d, 0.6, angle, angular_velocity);
    return tock;
  }

  void Draw(SkCanvas& canvas) const override {
    auto& effect = GearShader();
    auto& color = RubberColor();
    auto& normal = RubberNormal();

    SkRuntimeEffectBuilder builder(effect);
    builder.uniform("iRotationRad") = angle;
    builder.uniform("iScrollRatio") = (float)(angle / (2 * M_PIf) + 0.5f);
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

  Optional<Rect> DrawBounds() const override { return Shape().getBounds(); }
};

SyncBelt::SyncBelt(Widget* parent, Object& object, Argument::Table& table)
    : ArgumentToy(parent, object, table) {}

SkPath SyncBelt::Shape() const {
  return SkPath::Circle(pinion.x, pinion.y, kSecondaryGearRadius + kTeethAmplitude);
}

Interface SyncBelt::FindOption(ui::Pointer&, ui::ActionTrigger trigger) {
  if (trigger != ui::PointerButton::Left) return {};
  auto object = LockOwner();
  if (!object) return {};
  Object::TocGuard guard(*object);
  if (!object->toc.load(std::memory_order_acquire)->SlotOf(iface)) return {};
  return Interface(*object, *iface);
}

ui::Tock SyncBelt::Tick(time::Timer& t) {
  Tock tock;
  auto& toy_store = ToyScope();

  // Check if the object of this connection still exists.
  auto object = LockOwner();
  Optional<Object::TocGuard> guard;
  if (object) guard.emplace(*object);
  if (!object || !object->toc.load(std::memory_order_acquire)->SlotOf(iface)) {
    tock.drawing |=
        animation::LowLevelSpringTowards(pinion.x, t.d, 0.3, 0.05, origin.x, origin_velocity.x);
    tock.drawing |=
        animation::LowLevelSpringTowards(pinion.y, t.d, 0.3, 0.05, origin.y, origin_velocity.y);
    if (!tock.ing) {
      MarkDead(t.now);
    }
    return tock;
  }
  auto& sync = static_cast<Sync::Table&>(*iface);

  auto sync_balance = sync.sync_balance;
  if (sync_balance != last_sync_balance) {
    int32_t diff = (int)(sync_balance - last_sync_balance) > 0 ? 1 : -1;
    target_scroll_ratio += (float)(diff) / kPrimaryGearCount / 2;
    last_sync_balance = sync_balance;
  }

  tock.drawing |= animation::LowLevelSineTowards(target_scroll_ratio, t.d, 0.6, scroll_ratio,
                                                 scroll_ratio_velocity);
  // Keeping the animation close to 0 makes it smoother.
  if (fabs(scroll_ratio) > 0) {
    float wholes = truncf(scroll_ratio);
    target_scroll_ratio -= wholes;
    scroll_ratio -= wholes;
  }

  // Find the owner object widget
  auto* owner_widget = toy_store.FindOrNull(*object);
  if (!owner_widget) return Tock::Draw;

  auto origin_shape = owner_widget->Shape().makeTransform(TransformBetween(*owner_widget, *this));
  origin = origin_shape.getBounds().center();
  if (!initialized) {
    initialized = true;
    pinion = origin;
  }
  if (auto new_label = object->InterfaceFromIdx(sync.attached_toc_idx).Name(); label != new_label) {
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

  Ptr<Gear> gear = sync.gear;
  auto* gear_widget = gear ? toy_store.FindOrNull(*gear) : nullptr;
  if (gear && !gear_widget) {
    tock.drawing |=
        animation::LowLevelSpringTowards(pinion.x, t.d, 0.3, 0.05, origin.x, origin_velocity.x);
    tock.drawing |=
        animation::LowLevelSpringTowards(pinion.y, t.d, 0.3, 0.05, origin.y, origin_velocity.y);
    if (!tock.ing) {
      MarkDead(t.now);
    }
    return tock;
  }
  if (gear) {
    if (gear_widget) {
      angle = gear_widget->angle;
      if (gear_widget->IsAnimating()) {
        tock |= Tock::Drawing;
      }
      auto gear_matrix = TransformBetween(*gear_widget, *this);
      Vec2 gear_origin = gear_matrix.mapOrigin();
      float new_scale = gear_matrix.mapRadius(1);
      if (new_scale != scale) {
        scale = new_scale;
        tock |= Tock::Drawing;
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
        tock |= Tock::Drawing;
      }
    }
  } else if (is_dragged) {
    tock.drawing |= animation::ExponentialApproach(1, t.d, 0.1, scale);
  } else {
    pinion_deflection.SmoothTargetUpdate(pinion, origin);
  }

  TickSplits();

  tock.drawing |= animation::LowLevelSineTowards(pinion.x <= origin.x ? 0 : 1, t.d, 0.5,
                                                 label_rotation_ratio, label_rotation_velocity);

  tock.drawing |= pinion_deflection.SpringTowards({}, t.d, 0.3, 0.05);

  if (!gear && !is_dragged && !tock.ing) {
    MarkDead(t.now);
  }

  if (pinion != shaped_pinion) {
    shaped_pinion = pinion;
    tock |= Tock::Shape;
  }

  texture_anchors = {{pinion + pinion_deflection, 1}, {origin, 2}};

  return tock;
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

Optional<Rect> SyncBelt::DrawBounds() const {
  constexpr float r = kSecondaryGearRadius + kTeethAmplitude;
  auto bounds = Rect::MakeCenter(pinion + pinion_deflection, r * 2, r * 2);
  bounds.ExpandToInclude(Rect::MakeCenter(origin, r * 2, r * 2));
  return bounds;
}

std::unique_ptr<ObjectToy> Gear::MakeToy(ui::Widget* parent) {
  return std::make_unique<GearWidget>(parent, *this);
}

SyncAction::SyncAction(ui::Pointer& pointer, Interface& iface, Toy* toy) : Action(pointer) {
  Object& obj = *iface.object_ptr;
  if (auto* existing = dyn_cast_if_present<Sync::Table>(iface.table_ptr)) {
    sync = AcquireLocked(Sync(obj, *existing));
  } else {
    sync = AcquireLocked(Sync(obj, FindOrMakeSyncTable(cast<Command>(iface))));
  }
  Leave(*sync.table);
  if (toy) board_widget = BoardOrNull(*toy);
  if (auto* bw = board_widget.Get()) {
    if (bw->toys.FindOrNull(obj)) {
      auto& sync_widget = bw->toys.FindOrMake(sync, bw);
      sync_widget.is_dragged = true;
      sync_widget.WakeAnimation();
      bw->WakeAnimation();
    }
  }
  Update();
  pointer.root_widget.WakeAnimation();
}

SyncAction::~SyncAction() {
  // Check if the pointer is over a compatible Syncable
  if (auto* bw = board_widget.Get()) {
    if (auto* sync_widget = bw->toys.FindOrNull(sync)) {
      sync_widget->is_dragged = false;
      sync_widget->WakeAnimation();
      bw->ConnectAtPoint(sync, sync_widget->pinion);
      // TODO: animate towards the true target rather than origin
      sync_widget->pinion_deflection.SmoothTargetUpdate(sync_widget->pinion, sync_widget->origin);
    }
  }
  if (sync.table->gear == nullptr) sync.object_ptr->RemoveHook(*sync.table);
}

void SyncAction::Update() {
  auto* bw = board_widget.Get();
  if (!bw) return;
  auto* origin_widget = bw->toys.FindOrNull(*sync.object_ptr);
  if (!origin_widget) return;
  auto* sync_widget = bw->toys.FindOrNull(sync);
  if (!sync_widget) return;
  auto start_local = origin_widget->Shape().getBounds().center();
  auto start = TransformBetween(*origin_widget, *bw).mapPoint(start_local);
  auto pointer_pos = pointer.PositionWithin(*bw);
  sync_widget->origin = start;
  if (!sync_widget->initialized) {
    sync_widget->initialized = true;
    sync_widget->pinion = start;
    sync_widget->pinion_deflection.SmoothTargetUpdate(sync_widget->pinion, pointer_pos);
  } else {
    sync_widget->pinion_deflection.InteractiveTargetUpdate(sync_widget->pinion, pointer_pos);
  }
  sync_widget->WakeAnimation();
}

bool SyncAction::Highlight(Interface end) const { return sync.CanConnect(end); }

ui::Widget* SyncAction::Widget() { return nullptr; }

}  // namespace automat
