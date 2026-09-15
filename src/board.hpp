#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "animation.hpp"
#include "base.hpp"
#include "engine.hpp"
#include "image_provider.hpp"
#include "object_source.hpp"
#include "ptr.hpp"
#include "resizable.hpp"

namespace automat {

struct BoardWidget;

// 2D Canvas holding objects & a spaghetti of connections.
struct Board : Object {
  Board();

  // Center of the board in RootWidget's coordinates.
  Vec2 position = {0, 0};

  Vec2 size = {1, 1};  // 1x1 m

  bool frame_visible = false;

  // TODO: Board background styling

  // Locked<ImageProvider> background;  // uses default background if null

  // enum class BackgroundSizing {
  //   Natural,
  //   Cover,
  //   Contain,
  //   FitHeight,
  //   FitWidth,
  // } bg_sizing;

  // enum class BackgroundAnchor {
  //   TopLeft,
  //   TopRight,
  //   BottomLeft,
  //   BottomRight,
  //   Center,
  // } bg_anchor;

  // enum class BackgroundRepeat {
  //   NoRepeat,
  //   Repeat,
  //   Mirror,
  // } bg_repeat;

  float PxToMetric() const;

  deque<Ptr<Location>> locations;

  using Toy = BoardWidget;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  Ptr<Location> Extract(Location& location);

  void MoveToTop(Location& location);

  Location* LocationOrNull(Object& object);

  // Create a new location on top of all the others.
  Location& CreateEmpty();

  Location& Create(const Object& prototype) {
    auto& h = CreateEmpty();
    h.Create(prototype);
    return h;
  }

  // Adds the given object to the Board. Returns a pointer to the Location that stores the object.
  // Existing Location is returned, if the object was already part of the Board.
  Location& Insert(Ptr<Object>&& obj) {
    auto lock = std::lock_guard(engine.mutex);
    if (auto* loc = LocationOrNull(*obj)) {
      return *loc;
    }
    auto& h = CreateEmpty();
    h.InsertHere(std::move(obj));
    return h;
  }

  // Create an instance of T and return its location.
  //
  // The new instance is created from a prototype instance found in `prototypes`.
  template <typename T>
  Location& Create() {
    return Create(*prototypes->Find<T>());
  }

  void SerializeState(ObjectSerializer& writer) const override;

  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  Ptr<Object> Clone() const override {
    auto m = MAKE_PTR(Board);
    for (auto& my_it : locations) {
      auto& other_h = m->CreateEmpty();
      other_h.Create(*my_it->object);
    }
    return m;
  }

  string ToStr() const { return "Board"; }

  DEF_INTERFACE(Board, Resizable, resizable, "Resizable")
  bool ResizePx(int top, int right, int bottom, int left);
  bool ResizeM(Rect grow);
  DEF_END(resizable);

  DEF_INTERFACE(Board, ObjectSource, move, "Move")
  Ptr<Object> OnTake() { return obj->AcquirePtr(); }
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(move);

  DEF_INTERFACE(Board, Command, toggle_frame, "Toggle Frame")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) {
    obj->frame_visible = !obj->frame_visible;
    obj->WakeToys();
  }
  DEF_END(toggle_frame);

  INTERFACES(move, toggle_frame, resizable);
};

// UI widget for Board. Handles drawing, drop target, and spatial queries.
struct BoardWidget : ObjectToy, ui::DropTarget {
  struct ToyScope toys;

  animation::SpringV2<Vec2> size;
  animation::SpringV2<float> frame_width;

  BoardWidget(ui::Widget* parent, Board& board);

  Ptr<Board> LockBoard() const { return LockOwner<Board>(); }

  std::string_view Name() const override { return "BoardWidget"; }

  void OnPoll(time::Timer& timer) override { toys.Poll(timer); }

  // Widget overrides
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;

  Rect BgBounds() const { return Rect::MakeCenterZero(size.value.width, size.value.height); }
  RRect FrameBounds() const {
    return RRect::MakeSimple(BgBounds().Outset(frame_width), frame_width);
  }

  SkPath Shape() const override;
  SkPath SubtreeShape() const override;
  Compositor GetCompositor() const override { return Compositor::QUANTUM_REALM; }
  Interface FindOption(ui::Pointer&, ui::ActionTrigger) override;
  MiniMenuMode MenuMode() override { return MODE_6_DIR; }

  // DropTarget overrides
  ui::DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override { return true; }
  SkMatrix DropSnap(const Rect& bounds_local, Vec2 bounds_origin,
                    Vec2* fixed_point = nullptr) override;
  void DropLocation(Ptr<Location>&&) override;

  void RebuildOverlaps(Board&);

  // Spatial queries (these use widget shape data)
  void ConnectAtPoint(Argument, Vec2);
  void* Nearby(Vec2 center, float radius, std::function<void*(Location&)> callback);
  void NearbyCandidates(
      Location& here, Argument::Table& arg, float radius,
      std::function<void(ObjectToy&, Interface::Table*, Vec<Vec2AndDir>&)> callback);
  void ForStack(Location& base, std::function<void(Location&, int index)> callback);
  SkPath StackShape(Location& base);
  Vec<Ptr<Location>> DragStack(Location& base);
  Vec<Ptr<Location>> CloneStack(Location& base, Vec<std::unique_ptr<Toy>>& toys);
  void RaiseStack(Location& base);
};

BoardWidget* BoardOrNull(const ui::Widget& widget);

static_assert(ToyMaker<Board>);

}  // namespace automat
