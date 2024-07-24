#include "drag_action.hh"

#include <cmath>

#include "animation.hh"
#include "math.hh"
#include "pointer.hh"
#include "root.hh"
#include "window.hh"

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(roundf(v.x * 1000) / 1000., roundf(v.y * 1000) / 1000.);
}

void DragActionBase::Begin() {
  last_position = current_position = pointer.PositionWithinRootMachine();
}

Vec2 SnapPosition(DragActionBase& d) {
  return RoundToMilimeters(d.current_position - d.contact_point);
}

void DragActionBase::Update() {
  current_position = pointer.PositionWithinRootMachine();

  Vec2 position = current_position - contact_point;
  float scale = 1;

  for (int i = pointer.path.size() - 1; i >= 0; --i) {
    if (gui::DropTarget* drop_target = pointer.path[i]->CanDrop()) {
      drop_target->SnapPosition(position, scale, DraggedObject());
      break;
    }
  }

  if (last_snapped_position != position) {
    last_snapped_position = position;
    DragUpdate(pointer.window.display, current_position - last_position);
  }
  SnapUpdate(position, scale);

  last_position = current_position;
}

DragObjectAction::DragObjectAction(gui::Pointer& pointer, std::unique_ptr<Object>&& object_arg)
    : DragActionBase(pointer), object(std::move(object_arg)), position({}), scale(1) {
  anim = std::make_unique<LocationAnimationState>();
}

DragObjectAction::~DragObjectAction() {}

void DragObjectAction::SnapUpdate(Vec2 pos, float scale) {
  this->scale = scale;
  position = pos;
}

void DragObjectAction::DragUpdate(animation::Display&, Vec2 delta_pos) {
  anim->position.value += delta_pos;
}
void DragLocationAction::DragUpdate(animation::Display& display, Vec2 delta_pos) {
  location->animation_state[display].position.value += delta_pos;
}

void DragLocationAction::SnapUpdate(Vec2 pos, float scale) {
  location->scale = scale;
  location->position = pos;
}

void DragActionBase::End() { DragEnd(); }

void DragActionBase::DrawAction(gui::DrawContext& ctx) { DragDraw(ctx); }

void DragObjectAction::DragEnd() {
  Location& loc = root_machine->CreateEmpty();
  loc.position = position;
  loc.scale = scale;
  loc.InsertHere(std::move(object));
}

void DragObjectAction::DragDraw(gui::DrawContext& ctx) {
  anim->Tick(ctx.DeltaT(), position, scale);

  auto mat = anim->GetTransform(object->Shape(nullptr).getBounds().center());
  SkMatrix inv;
  mat.invert(&inv);

  auto matrix_backup = ctx.canvas.getTotalMatrix();
  ctx.canvas.concat(inv);
  object->Draw(ctx);
  ctx.canvas.setMatrix(matrix_backup);
}

Object* DragObjectAction::DraggedObject() { return object.get(); }

DragLocationAction::DragLocationAction(gui::Pointer& pointer, Location* location)
    : DragActionBase(pointer), location(location) {}

DragLocationAction::~DragLocationAction() {}

void DragLocationAction::DragEnd() { location->position = SnapPosition(*this); }

void DragLocationAction::DragDraw(gui::DrawContext&) {
  // Location is drawn by its parent Machine so nothing to do here.
}

Object* DragLocationAction::DraggedObject() { return location->object.get(); }
DragActionBase::DragActionBase(gui::Pointer& pointer) : Action(pointer) {
  pointer.window.drag_action_count++;
}
DragActionBase::~DragActionBase() { pointer.window.drag_action_count--; }
}  // namespace automat
