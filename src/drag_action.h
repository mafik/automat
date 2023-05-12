#pragma once

#include <memory>

#include "action.h"

namespace automaton {

struct Object;

struct DragActionBase : Action {
  vec2 contact_point;
  vec2 current_position;
  product_ptr<animation::Approach> round_x;
  product_ptr<animation::Approach> round_y;
  void Begin(gui::Pointer &pointer) override;
  void Update(gui::Pointer &pointer) override;
  void End() override;
  void Draw(SkCanvas &canvas, animation::State &animation_state) override;
  virtual void DragEnd(vec2 pos) = 0;
  virtual void DragDraw(SkCanvas &canvas,
                        animation::State &animation_state) = 0;
};

struct DragObjectAction : DragActionBase {
  std::unique_ptr<Object> object;
  void DragEnd(vec2 pos) override;
  void DragDraw(SkCanvas &canvas, animation::State &animation_state) override;
};

} // namespace automaton