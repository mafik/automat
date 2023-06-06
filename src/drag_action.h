#pragma once

#include <memory>

#include "action.h"

namespace automat {

struct Object;
struct Location;

struct DragActionBase : Action {
  vec2 contact_point;
  vec2 current_position;

  vec2 TargetPosition() const;
  vec2 TargetPositionRounded() const;

  struct ApproachMaker {
    animation::Approach operator()() {
      animation::Approach ret(0);
      ret.speed = 50;
      return ret;
    }
  };

  product_ptr<animation::Approach, ApproachMaker> round_x;
  product_ptr<animation::Approach, ApproachMaker> round_y;
  void Begin(gui::Pointer& pointer) override;
  void Update(gui::Pointer& pointer) override;
  void End() override;
  void DrawAction(gui::DrawContext&) override;
  virtual void DragUpdate() = 0;
  virtual void DragEnd() = 0;
  virtual void DragDraw(gui::DrawContext&) = 0;
};

struct DragObjectAction : DragActionBase {
  std::unique_ptr<Object> object;
  void DragUpdate() override;
  void DragEnd() override;
  void DragDraw(gui::DrawContext&) override;
};

struct DragLocationAction : DragActionBase {
  Location* location;
  DragLocationAction(Location* location);
  ~DragLocationAction() override;
  void DragUpdate() override;
  void DragEnd() override;
  void DragDraw(gui::DrawContext&) override;
};

}  // namespace automat