#pragma once
#include <memory>

#include "action.hh"
#include "animation.hh"
#include "object.hh"
#include "time.hh"

namespace automat {

namespace gui {
struct DropTarget {
  virtual void SnapPosition(Vec2& position, float& scale, Object* object,
                            Vec2* fixed_point = nullptr) = 0;
};
}  // namespace gui

struct Location;
struct LocationAnimationState;

struct DragActionBase : Action {
  Vec2 contact_point;          // in the coordinate space of the dragged Object
  Vec2 last_position;          // root machine coordinates
  Vec2 current_position;       // root machine coordinates
  Vec2 last_snapped_position;  // root machine coordinates
  time::SteadyPoint last_update;

  DragActionBase(gui::Pointer& pointer);
  ~DragActionBase();
  void Begin() override;
  void Update() override;
  void End() override;
  void DrawAction(gui::DrawContext&) override;

  // DragActionBase takes care of snapping objects to grid and animating a virtual spring that
  // animates them smoothly. Derived classes are given the snapped position and should call the
  // given callback so that spring animation can be applied.
  virtual void SnapUpdate(Vec2 pos, float scale) = 0;

  // Called when the object should be immediately shifted on screen by the given delta_pos. This is
  // used when the user drags the object directly so the movement should be immediate.
  virtual void DragUpdate(animation::Display&, Vec2 delta_pos) = 0;
  virtual void DragEnd() = 0;
  virtual void DragDraw(gui::DrawContext&) = 0;
  virtual Object* DraggedObject() = 0;
};

struct DragObjectAction : DragActionBase {
  Vec2 position = {};
  float scale = 1;
  std::unique_ptr<Object> object;
  std::unique_ptr<LocationAnimationState> anim;

  DragObjectAction(gui::Pointer&, std::unique_ptr<Object>&&);
  ~DragObjectAction() override;
  void DragUpdate(animation::Display&, Vec2 delta_pos) override;
  void SnapUpdate(Vec2 pos, float scale) override;
  void DragEnd() override;
  void DragDraw(gui::DrawContext&) override;
  Object* DraggedObject() override;
};

struct DragLocationAction : DragActionBase {
  Location* location;
  DragLocationAction(gui::Pointer&, Location*);
  ~DragLocationAction() override;

  void DragUpdate(animation::Display&, Vec2 delta_pos) override;
  void SnapUpdate(Vec2 pos, float scale) override;
  void DragEnd() override;
  void DragDraw(gui::DrawContext&) override;
  Object* DraggedObject() override;
};

}  // namespace automat