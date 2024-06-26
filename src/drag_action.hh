#pragma once

#include <memory>

#include "action.hh"
#include "animation.hh"
#include "object.hh"

namespace automat {

struct Location;

struct DragActionBase : Action {
  Vec2 contact_point;
  Vec2 last_position;
  Vec2 current_position;

  Vec2 TargetPosition() const;
  Vec2 TargetPositionRounded() const;

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
  animation::Spring<Vec2> position_offset;
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