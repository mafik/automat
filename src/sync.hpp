#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include "argument.hpp"
#include "command.hpp"
#include "object.hpp"
#include "on_off.hpp"
#include "ptr.hpp"
#include "spin_lock.hpp"
#include "syncable.hpp"

namespace automat {

struct Gear;
struct SyncBelt;
struct GearWidget;
struct BoardWidget;

struct Sync : Argument {
  // Holds all the state needed to hook interfaces. Combines Hook & a linked list of Gear members
  // with derived classes adding the interface-specific tables.
  struct Table : Argument::Table, Hook {
    Ptr<Gear> gear = nullptr;  // may be nullptr if not connected
    Sync::Table *prev = nullptr, *next = nullptr;
    bool sink = true, source = true;
    int attached_toc_idx = 0;
    uint32_t sync_balance = 0;  // used for scrolling the sync belt
    Object* object = nullptr;

    ~Table();

    int Detach(Object& obj) override;

    virtual Locked<Command> Start() = 0;
    virtual Locked<Command> End() = 0;
    virtual Syncable SyncableOf() { return {}; }  // used when (un)syncing needs some extra logic

    static bool classof(const Interface::Table* i) { return i->kind == Interface::kSyncArgument; }

    static void DefaultCanConnect(Argument self, Interface end, Status& status);
    static void DefaultOnConnect(Argument self, Interface end);
    static Locked<Interface> DefaultFind(Argument self);
    static std::unique_ptr<Action> SyncActivate(Span<Interface>, ui::Pointer&, automat::Toy*);

    Table(Kind kind = Interface::kSyncArgument) : Argument::Table("Sync", kind) {
      style = Style::Belt;
      visible_when_disconnected = false;
      can_connect = &DefaultCanConnect;
      on_connect = &DefaultOnConnect;
      find = &DefaultFind;
      make_icon = &Interface::Table::DefaultMakeIcon;
      activate = SyncActivate;
    }
  };

  struct CommandTable : Table {
    Command::Table cmd_wrapper{""};
    Command::Table cancel_wrapper{""};
    Argument::Table completion_arg{"When Complete"};
    Command::Table completion_detector{"Notify Completed"};

    CommandTable();

    void Attach(Object& obj, int cmd_idx) override;
    Locked<Command> Start() override;
    Locked<Command> End() override;
  };

  struct OnOffTable : Table {
    OnOff::Table wrapper{""};
    Command::Table on_wrapper{""};
    Command::Table off_wrapper{""};

    void Attach(Object& obj, int toc_idx) override;
    Locked<Command> Start() override;
    Locked<Command> End() override;
    Syncable SyncableOf() override;
  };

  INTERFACE_BOUND(Sync, Argument)

  using Toy = SyncBelt;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);
};

// Gear-shaped object that can make multiple interfaces act as one.
struct Gear : Object {
  std::shared_mutex mutex;

  Sync::Table* members = nullptr;

  ~Gear();

  Ptr<Object> Clone() const override { return MAKE_PTR(Gear); }

  // Make sure that this member will receive sync notifications from the Sources in this SyncGroup.
  void AddSink(Command);

  // Make sure that the sync notifications from this Syncable will be propagated to Sinks of this
  // Gear.
  void AddSource(Command);

  // AddSink & AddSource together.
  void FullSync(Command);

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  using Toy = GearWidget;
};

// ## Old sync notification conventions
//
// Note: the following paragraphs are NOT accurate any more but are being been kept around since
// they contain a good introduction of the 'backflow' problem and how to address it.
//
// Each Command can typically be invoked three ways (using Foo as an example):
//
//   OnFoo() - "On" prefix. Applies the state change to THIS object only.
//                            Called by the sync infrastructure on each peer. Must NOT
//                            call any of the methods below — doing so would create loops.
//
//   Foo() - External entry point. Calls OnFoo on self AND
//                            every synced peer (via ForwardDo). Use when an outside caller
//                            (user click, another object) requests the transition.
//
//   NotifyFoo() - Self-originated notification. This object has already changed
//                            state on its own (e.g. an OS event, a sensor reading). Calls
//                            OnFoo on every OTHER synced peer, skipping self
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
//    This is the reason for "Foo" / "OnFoo" distinction. Foo sends
//    a notification, but OnFoo is not allowed to. This works fine for simple interfaces,
//    where the "may notify" signal can be passed through method signatures.
// 2) Track the "source" of some action & make sure that notifications don't flow back.
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
  bool initialized = false;  // both ends were placed under the owner object
  Vec2 shaped_pinion{};      // pinion position at the last Shape() refresh

  SyncBelt(ui::Widget* parent, Object& object, Argument::Table&);

  bool DrawnUnderEndpoints() const override { return true; }
  SkPath Shape() const override;
  Interface FindOption(ui::Pointer&, ui::ActionTrigger) override;
  Tock Tick(time::Timer& t) override;
  void Draw(SkCanvas& canvas) const override;
  Optional<Rect> DrawBounds() const override;
};

struct SyncAction : Action {
  Locked<Sync> sync;
  // TODO: syncing across the Boards
  MortalPtr<BoardWidget> board_widget;
  SyncAction(ui::Pointer&, Interface&, Toy*);
  ~SyncAction();
  void Update() override;
  bool Highlight(Interface end) const override;
  ui::Widget* Widget() override;
};

}  // namespace automat
