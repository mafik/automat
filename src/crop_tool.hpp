#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <mutex>

#include "animation.hpp"
#include "argument.hpp"
#include "object.hpp"
#include "optional.hpp"
#include "pointer.hpp"

namespace automat {

struct CropView;
struct CropScissors;

struct CropViewParameters {
  virtual ~CropViewParameters() = default;
};

struct Croppable : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kCroppable; }

    void (*crop_view_serialize)(Croppable, const CropView&, ObjectSerializer&) = nullptr;
    void (*crop_view_deserialize)(Croppable, CropView&, ObjectDeserializer&) = nullptr;
    std::unique_ptr<CropViewParameters> (*crop_view_make_parameters)(Croppable) = nullptr;
    void (*crop_scissors_open)(Croppable, CropView&, Toy& croppable_toy, CropScissors&,
                               Vec2) = nullptr;
    void (*crop_scissors_move)(Croppable, CropView&, Toy& croppable_toy, CropScissors&,
                               Vec2) = nullptr;
    ui::Tock (*crop_scissors_tick)(Croppable, CropView&, Toy& croppable_toy, CropScissors&,
                                   time::Timer&) = nullptr;
    ui::Tock (*crop_scissors_close)(Croppable, CropScissors&, time::Timer&) = nullptr;
    std::unique_ptr<Toy> (*crop_make_toy)(Croppable, CropView&, ui::Widget* parent) = nullptr;
    ui::Tock (*crop_tick_toy)(Croppable, CropView&, Toy& cropped_toy, time::Timer&) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kCroppable, name) {}
  };

  struct State {
    std::mutex mutex;
    Vec<WeakPtr<CropView>> views;
  };

  INTERFACE_BOUND(Croppable, Interface)

  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = Croppable;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      if constexpr (requires(ImplT& i, const CropView& v, ObjectSerializer& w) {
                      i.CropViewSerialize(v, w);
                    })
        t.crop_view_serialize = [](Croppable self, const CropView& view, ObjectSerializer& writer) {
          static_cast<ImplT&>(self).CropViewSerialize(view, writer);
        };
      if constexpr (requires(ImplT& i, CropView& v, ObjectDeserializer& d) {
                      i.CropViewDeserialize(v, d);
                    })
        t.crop_view_deserialize = [](Croppable self, CropView& view, ObjectDeserializer& d) {
          static_cast<ImplT&>(self).CropViewDeserialize(view, d);
        };
      if constexpr (requires(ImplT& i) { i.CropViewMakeParameters(); })
        t.crop_view_make_parameters = [](Croppable self) -> std::unique_ptr<CropViewParameters> {
          return static_cast<ImplT&>(self).CropViewMakeParameters();
        };
      if constexpr (requires(ImplT& i, CropView& v, Toy& t, CropScissors& s, Vec2 p) {
                      i.CropScissorsOpen(v, t, s, p);
                    })
        t.crop_scissors_open = [](Croppable self, CropView& view, Toy& toy, CropScissors& scissors,
                                  Vec2 point) {
          static_cast<ImplT&>(self).CropScissorsOpen(view, toy, scissors, point);
        };
      if constexpr (requires(ImplT& i, CropView& v, Toy& t, CropScissors& s, Vec2 p) {
                      i.CropScissorsMove(v, t, s, p);
                    })
        t.crop_scissors_move = [](Croppable self, CropView& view, Toy& toy, CropScissors& scissors,
                                  Vec2 point) {
          static_cast<ImplT&>(self).CropScissorsMove(view, toy, scissors, point);
        };
      if constexpr (requires(ImplT& i, CropView& v, Toy& t, CropScissors& s, time::Timer& timer) {
                      i.CropScissorsTick(v, t, s, timer);
                    })
        t.crop_scissors_tick = [](Croppable self, CropView& view, Toy& toy, CropScissors& scissors,
                                  time::Timer& timer) -> ui::Tock {
          return static_cast<ImplT&>(self).CropScissorsTick(view, toy, scissors, timer);
        };
      if constexpr (requires(ImplT& i, CropScissors& s, time::Timer& timer) {
                      i.CropScissorsClose(s, timer);
                    })
        t.crop_scissors_close = [](Croppable self, CropScissors& scissors,
                                   time::Timer& timer) -> ui::Tock {
          return static_cast<ImplT&>(self).CropScissorsClose(scissors, timer);
        };
      if constexpr (requires(ImplT& i, CropView& v, ui::Widget* parent) {
                      i.CropMakeToy(v, parent);
                    })
        t.crop_make_toy = [](Croppable self, CropView& view,
                             ui::Widget* parent) -> std::unique_ptr<Toy> {
          return static_cast<ImplT&>(self).CropMakeToy(view, parent);
        };
      if constexpr (requires(ImplT& i, CropView& v, Toy& t, time::Timer& timer) {
                      i.CropTickToy(v, t, timer);
                    })
        t.crop_tick_toy = [](Croppable self, CropView& view, Toy& toy,
                             time::Timer& timer) -> ui::Tock {
          return static_cast<ImplT&>(self).CropTickToy(view, toy, timer);
        };
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

struct CropView : Object {
  mutable std::mutex mutex;
  Linked<Croppable> chain_end;
  std::unique_ptr<CropViewParameters> parameters;
  float hue = 95.2;

  ~CropView() override;

  DEF_INTERFACE(CropView, InterfaceArgument<Croppable>, scissors, "Scissors")
  static constexpr ui::Cursor kCursor = ui::Cursor::Grab;
  void OnCanConnect(Interface end, Status&);
  void OnConnect(Interface end);
  std::unique_ptr<ArgumentToy> OnMakeToy(ui::Widget* parent);
  void OnDestroyToy(std::unique_ptr<automat::Toy>);
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(scissors);

  DEF_INTERFACE(CropView, Croppable, croppable, "Crop")
  DEF_END(croppable);

  INTERFACES(scissors_tbl, croppable_tbl);

  StrView Name() const override { return "Crop View"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

struct CropScissors : ArgumentToy {
  struct Group {
    Linked<Croppable> croppable;
    Vec<std::unique_ptr<ui::Widget>> gizmos;
    Vec2 center;
    Vec2 size;
    animation::Kinetic<Vec2> center_deflection;
    animation::Kinetic<Vec2> size_deflection;
    animation::Kinetic<float> unfolding;
    Vec2 extent;

    Group() = default;
    Group(Group&&) noexcept = default;
    Group& operator=(Group&&) noexcept = default;

    Vec2 DrawnCenter() const { return center + center_deflection.value; }
    Vec2 DrawnSize() const { return (size + size_deflection.value) * Vec2(1, unfolding.value); }
    Tock Unfold(Vec2 target_center, Vec2 target_size, time::Timer&);
    Tock Fold(time::Timer&);
  };

  MortalPtr<ui::Pointer> pointer;
  struct Box {
    Vec2 center;
    SinCos turn;
    float scale;
    Vec2 size;

    SkMatrix ToScissors() const {
      return SkMatrix::Translate(center.x, center.y)
          .preConcat(turn.ToMatrix())
          .preScale(scale, scale);
    }
  };
  Optional<Box> box;
  Vec<Group> groups;

  MortalPtr<ObjectToy> view_toy;
  float hue = 0;
  const ui::Widget* base_source = nullptr;
  Vec2 pivot;
  SinCos turn;
  float scale = 1;
  float elevation = 0;
  float halves_distance = 0;
  animation::Kinetic<Vec2> pivot_deflection;
  animation::Kinetic<SinCos> turn_deflection;
  animation::Kinetic<float> scale_deflection;
  animation::Kinetic<float> opening;
  animation::Kinetic<float> halves_distance_deflection;
  animation::Kinetic<float> elevation_deflection;
  uint32_t observed_target_monitors = 0;

  CropScissors(ObjectToy& view_toy, Object& view);

  Group* FindGroup(const Linked<Croppable>& croppable);
  Group& FindOrMakeGroup(const Linked<Croppable>& croppable);
  SkMatrix BoxToScissors() const;

  SkPath Shape() const override;
  Optional<Rect> DrawBounds() const override;
  Interface FindOption(ui::Pointer&, ui::ActionTrigger) override;
  void FillMenu(ui::Pointer&, Menu&) override;
  void OnReparent(ui::Widget& new_parent, SkM44& fix) override;
  void OnPoll(time::Timer&) override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
};

struct CropBox : ui::Widget {
  Vec2 size;

  CropBox(ui::Widget* parent);

  SkPath Shape() const override { return SkPath(); }
  Optional<Rect> DrawBounds() const override;
  Tock Tick(time::Timer&) override { return Tock::Draw; }
  void Draw(SkCanvas&) const override;
};

struct CropHandle : ui::Widget {
  Vec2 origin;
  float scale = 0;
  SkPath outline;
  Interface::Table* option = nullptr;

  CropHandle(ui::Widget* parent);

  static SkPath Corner(Vec2 outward);
  static SkPath End(Vec2 outward);

  SkPath Shape() const override;
  Optional<Rect> DrawBounds() const override;
  Interface FindOption(ui::Pointer&, ui::ActionTrigger) override;
  Tock Tick(time::Timer&) override { return Tock::Shape; }
  void Draw(SkCanvas&) const override;
};

}  // namespace automat
