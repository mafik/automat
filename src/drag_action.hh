#pragma once

#include <memory>

#include "action.hh"
#include "animation.hh"
#include "object.hh"

namespace automat {

struct Location;
struct LocationAnimationState;

struct DragActionBase : Action {
  Vec2 contact_point;
  Vec2 last_position;
  Vec2 current_position;

  void Begin(gui::Pointer& pointer) override;
  void Update(gui::Pointer& pointer) override;
  void End() override;
  void DrawAction(gui::DrawContext&) override;

  // DragActionBase takes care of snapping objects to grid and animating a virtual spring that
  // animates them smoothly. Derived classes are given the snapped position and should call the
  // given callback so that spring animation can be applied.
  virtual void DragUpdate(Vec2 pos, maf::Fn<void(LocationAnimationState&)> callback) = 0;
  virtual void DragEnd() = 0;
  virtual void DragDraw(gui::DrawContext&) = 0;
  virtual SkRect DragBox() = 0;
};

struct DragObjectAction : DragActionBase {
  Vec2 position;
  std::unique_ptr<Object> object;
  std::unique_ptr<LocationAnimationState> anim;
  DragObjectAction(std::unique_ptr<Object>&&);
  ~DragObjectAction() override;
  void DragUpdate(Vec2 pos, maf::Fn<void(LocationAnimationState&)> callback) override;
  void DragEnd() override;
  void DragDraw(gui::DrawContext&) override;
  SkRect DragBox() override;
};

struct DragLocationAction : DragActionBase {
  Location* location;
  DragLocationAction(Location* location);
  ~DragLocationAction() override;
  void DragUpdate(Vec2 pos, maf::Fn<void(LocationAnimationState&)> callback) override;
  void DragEnd() override;
  void DragDraw(gui::DrawContext&) override;
  SkRect DragBox() override;
};

}  // namespace automat