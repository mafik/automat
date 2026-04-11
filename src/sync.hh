// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>

#include "argument.hh"
#include "object.hh"
#include "ptr.hh"

namespace automat {

struct Gear;
struct SyncBelt;
struct GearWidget;

struct Syncable : Argument {
  struct Table : Argument::Table {
    static bool classof(const Interface::Table* i) {
      return i->kind >= Interface::kSyncable && i->kind <= Interface::kLastSyncable;
    }

    bool (*can_sync)(Syncable, Syncable other) = nullptr;
    void (*on_sync)(Syncable) = nullptr;
    void (*on_unsync)(Syncable) = nullptr;

    static void DefaultCanConnect(Argument self, Interface end, Status& status);
    static void DefaultOnConnect(Argument self, Interface end);
    static NestedPtr<Interface::Table> DefaultFind(Argument self);

    constexpr Table(StrView name, Kind kind = Interface::kSyncable) : Argument::Table(name, kind) {
      style = Style::Invisible;
      can_connect = &DefaultCanConnect;
      on_connect = &DefaultOnConnect;
      find = &DefaultFind;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Argument::Table::FillFrom<ImplT>();
      if constexpr (requires(ImplT& i, Syncable s) { i.CanSync(s); })
        can_sync = [](Syncable a, Syncable b) { return static_cast<ImplT&>(a).CanSync(b); };
      if constexpr (requires(ImplT& i) { i.OnSync(); })
        on_sync = [](Syncable self) { static_cast<ImplT&>(self).OnSync(); };
      if constexpr (requires(ImplT& i) { i.OnUnsync(); })
        on_unsync = [](Syncable self) { static_cast<ImplT&>(self).OnUnsync(); };
    }
  };

  struct State : Argument::State {
    WeakPtr<Gear> gear_weak;
    uint32_t sync_balance = 0;  // used for scrolling the sync belt
    bool source = false;

    // Remember to call this before this state is destroyed!
    //
    // Note: DO NOT REMOVE - interfaces that don't go through Def-based table definition must call
    // this directly in object destructors.
    void Unsync(Object& self, Syncable::Table& syncable);
  };

  INTERFACE_BOUND(Syncable, Argument)

  // Distributes a command to self AND all synced peers.
  // Use this as the entry point when an external caller requests a state change.
  // The lambda is called on self first, then on every synced peer.
  template <typename T, typename F>
  void ForwardDo(this T self, F&& lambda);

  // Distributes a notification to all synced peers, EXCLUDING self.
  // Use this when this object has already changed state (e.g. detected an OS event,
  // received a hardware signal) and needs to propagate that change to its peers.
  template <typename T, typename F>
  void ForwardNotify(this T self, F&& lambda);

  void Unsync();

  // Impl may provide:
  //   bool CanSync(Syncable other);
  //   void OnSync();
  //   void OnUnsync();
  template <typename ImplT>
  struct Def : State, DefBase {
    using Impl = ImplT;
    using Bound = Syncable;

    bool OnIsConnected() { return !gear_weak.IsExpired(); }

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {
      if (source || !gear_weak.IsExpired()) {
        Bind().Unsync();
      }
    }
  };

  using Toy = SyncBelt;
  Object& GetOwner() { return *object_ptr; }
  Interface::Table* GetInterface() { return table_ptr; }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);
};

// Gear-shaped object that can make multiple interfaces act as one.
struct Gear : Object {
  std::shared_mutex mutex;

  struct Member {
    NestedWeakPtr<Syncable::Table> weak;
    bool sink;
  };

  std::vector<Member> members;

  ~Gear();

  Ptr<Object> Clone() const override { return MAKE_PTR(Gear); }

  // Make sure that this member will receive sync notifications from the Sources in this SyncGroup.
  void AddSink(Object& obj, Syncable::Table& syncable);

  // Make sure that the sync notifications from this Syncable will be propagated to Sinks of this
  // Gear.
  void AddSource(Object& obj, Syncable::Table& syncable);

  // AddSink & AddSource together.
  void FullSync(Object& obj, Syncable::Table& syncable);

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;

  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  using Toy = GearWidget;
};

template <typename T, typename F>
void Syncable::ForwardDo(this T self, F&& lambda) {
  lambda(self);
  self.ForwardNotify(std::forward<F>(lambda));
}

// ## Sync notification conventions
//
// Each Syncable typically exposes three tiers of methods (using OnOff as an example):
//
//   OnTurnOn / OnTurnOff   — "On" prefix. Applies the state change to THIS object only.
//                            Called by the sync infrastructure on each peer. Must NOT
//                            call any of the methods below — doing so would create loops.
//
//   TurnOn / TurnOff       — External entry point. Calls OnTurnOn/OnTurnOff on self AND
//                            every synced peer (via ForwardDo). Use when an outside caller
//                            (user click, another object) requests the transition.
//
//   NotifyTurnedOn/Off     — Self-originated notification. This object has already changed
//                            state on its own (e.g. an OS event, a sensor reading). Calls
//                            OnTurnOn/OnTurnOff on every OTHER synced peer, skipping self
//                            (via ForwardNotify).
//
// Syncables that receive sync notifications must take steps to avoid immediately sending back their
// own notifications. Otherwise:
// - infinite loops may occur (when multiple reflecting members are synced)
// - direction of the belt is messed up (scrolls in the correct direction and then back, resulting
//   in no net movement)
//
// Additionally during object initialization / destruction, any notifications may mess up the
// desired state of objects.
//
// To avoid this, there are a couple approaches:
// 1) Separate the paths of code that are allowed / or not to send notifications.
//    This is the reason for "TurnOn" / "OnTurnOn" distinction. TurnOn sends a notification,
//    but OnTurnOn is not allowed to. This works fine for simple syncables, like FlipFlop, where the
//    "may notify" signal can be passed through method signatures.
// 2) Track the "source" of some action & make sure that notifications don't flow back. This
//    approach is used in tasks.cc/hh for LongRunning syncables. They must schedule a job using a
//    scheduler and the "source" field can be used to prevent backflow of notifications.
// 3) Track the target state in each Syncable and make sure that notifications are only sent when
//    the state actually changes.
// 4) A variable that inhibits notifications when set. Care must be taken to make it work in
//    concurrent scenarios. It may be attached to individual objects or be thread-local. This does
//    not address the reflections where the feedback loop exits the process.
// 5) Inhibitory period - a startegy inspired by spiking neurons. When a notification is received,
//    the Syncable enters an inhibitory period when it no longer sends any outgoing notifications
//    for some hardcoded period. This helps with feedback loops that exit the process but may mess
//    up with some of tightly timed logic. This is used by KeyPresser, where the feedback loop goes
//    out to the OS.
// 6) Accept this behavior - fundamentally, it's how real world works. Oscillations may occur if
//    multiple objects try to synchronize their state but disagree on the result.
//
// Each object needs to deal with its unique constraints in its own way.

template <typename T, typename F>
void Syncable::ForwardNotify(this T self, F&& lambda) {
  if (self.object_ptr->inhibit_sync_notifications) {
    return;
  }
  // self.obj.inhibit_sync_notifications is guaranteed to be false at this point.
  self.object_ptr->inhibit_sync_notifications = true;
  --self.state->sync_balance;
  self.WakeToys();
  auto& st = *self.state;
  if (st.source) {
    if (auto gear = st.gear_weak.Lock()) {
      gear->WakeToys();
      auto lock = std::shared_lock(gear->mutex);
      for (auto& member : gear->members) {
        if (!member.sink) continue;
        auto locked = member.weak.Lock();
        if (!locked) continue;
        T other(*locked.template Owner<Object>(), *static_cast<typename T::Table*>(locked.Get()));
        // Skip self
        if (self == other) {
          continue;
        }
        lambda(other);
        ++other.state->sync_balance;
        other.WakeToys();
      }
    }
  }
  self.object_ptr->inhibit_sync_notifications = false;
}

// Returns a reference to the existing or a new Gear.
Ptr<Gear> FindGearOrMake(Object& source_obj, Syncable::Table& source);
Ptr<Gear> FindGearOrNull(Object& source_obj, Syncable::Table& source);

// Widget that draws one belt connection from a Gear to a synced member.
struct SyncBelt : ArgumentToy {
  Str label;
  sk_sp<SkShader> label_shader;
  float label_rotation_ratio = 0;
  float label_rotation_velocity = 0;
  Vec2 origin{};                                  // where the rubber belt starts
  Vec2 pinion{};                                  // where the small gear is located
  animation::SpringV2<Vec2> pinion_deflection{};  // deflection from the ideal position
  // Used for animating belt disappearance
  //
  // This is stored as a separate variable rather than SpringV2 because animation is only used on
  // disappearance.
  Vec2 origin_velocity{};
  float angle = 0;
  float scale = 1;
  float scroll_ratio = 0;
  float scroll_ratio_velocity = 0;
  float target_scroll_ratio = 0;
  int last_sync_balance = 0;
  bool is_dragged = false;

  SyncBelt(ui::Widget* parent, Object& object, Syncable::Table& syncable);

  SkPath Shape() const override;
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;
  animation::Phase Tick(time::Timer& t) override;
  Compositor GetCompositor() const override { return Compositor::ANCHOR_WARP; }
  Vec<Vec2> TextureAnchors() override;
  void Draw(SkCanvas& canvas) const override;
  Optional<Rect> TextureBounds() const override;
};

static_assert(ToyMaker<Syncable>);

struct SyncAction : Action {
  NestedWeakPtr<Syncable::Table> weak;
  SyncAction(ui::Pointer& pointer, Syncable syncable);
  ~SyncAction();
  void Update() override;
  bool Highlight(Interface end) const override;
  ui::Widget* Widget() override;
};

}  // namespace automat
