#pragma once

#include <memory>

#include "action.h"

namespace automaton {

struct Object;
struct Location;

struct DragActionBase : Action {
  vec2 contact_point;
  vec2 current_position;

  struct ApproachMaker {
    animation::Approach operator()() {
      animation::Approach ret(0);
      ret.speed = 50;
      return ret;
    }
  };

  product_ptr<animation::Approach, ApproachMaker> round_x;
  product_ptr<animation::Approach, ApproachMaker> round_y;
  void Begin(gui::Pointer &pointer) override;
  void Update(gui::Pointer &pointer) override;
  void End() override;
  void Draw(SkCanvas &canvas, animation::State &animation_state) override;
  virtual void DragUpdate(vec2 pos) = 0;
  virtual void DragEnd(vec2 pos) = 0;
  virtual void DragDraw(SkCanvas &canvas,
                        animation::State &animation_state) = 0;
};

struct DragObjectAction : DragActionBase {
  std::unique_ptr<Object> object;
  void DragUpdate(vec2 pos) override;
  void DragEnd(vec2 pos) override;
  void DragDraw(SkCanvas &canvas, animation::State &animation_state) override;
};

struct DragLocationAction : DragActionBase {
  Location *location;
  DragLocationAction(Location *location);
  ~DragLocationAction() override;
  void DragUpdate(vec2 pos) override;
  void DragEnd(vec2 pos) override;
  void DragDraw(SkCanvas &canvas, animation::State &animation_state) override;
};

} // namespace automaton