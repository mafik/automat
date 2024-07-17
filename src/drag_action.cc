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

void DragActionBase::Begin(gui::Pointer& pointer) {
  last_position = current_position = pointer.PositionWithinRootMachine();
}

Vec2 SnapPosition(DragActionBase& d) {
  return RoundToMilimeters(d.current_position - d.contact_point);
}

void DragActionBase::Update(gui::Pointer& pointer) {
  last_position = current_position;
  current_position = pointer.PositionWithinRootMachine();

  auto last_round = RoundToMilimeters(last_position - contact_point);
  auto curr_round = RoundToMilimeters(current_position - contact_point);
  float scale = 1;

  if (pointer.window.IsOverTrash(pointer.pointer_position)) {
    gui::Path root_machine_path;
    root_machine_path.push_back(&pointer.window);
    root_machine_path.push_back(root_machine);
    SkMatrix mat = TransformDown(root_machine_path, &pointer.window.display);
    auto drag_box = DragBox();
    Vec2 box_size = Vec2(drag_box.width(), drag_box.height());
    float diagonal = Length(box_size);

    curr_round =
        mat.mapPoint(pointer.window.size - box_size / diagonal * pointer.window.trash_radius / 2) -
        drag_box.center();

    scale = mat.mapRadius(pointer.window.trash_radius) / diagonal * 0.9f;
  }
  DragUpdate(curr_round, scale);
}

DragObjectAction::DragObjectAction(std::unique_ptr<Object>&& object_arg)
    : object(std::move(object_arg)) {
  anim = std::make_unique<LocationAnimationState>();
}

DragObjectAction::~DragObjectAction() {}

void DragObjectAction::DragUpdate(Vec2 pos, float scale) {
  this->scale = scale;
  position = pos;
  // TODO: clamp animation
}

void DragLocationAction::DragUpdate(Vec2 pos, float scale) {
  location->scale = scale;
  location->position = pos;
  for (auto& anim : location->animation_state) {
    // TODO: clamp animation
  }
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

  auto mat = anim->GetTransform(object->Shape().getBounds().center());
  SkMatrix inv;
  mat.invert(&inv);

  auto matrix_backup = ctx.canvas.getTotalMatrix();
  ctx.canvas.concat(inv);
  object->Draw(ctx);
  ctx.canvas.setMatrix(matrix_backup);
}

SkRect DragObjectAction::DragBox() { return object->Shape().getBounds(); }

DragLocationAction::DragLocationAction(Location* location) : location(location) {}

DragLocationAction::~DragLocationAction() {}

void DragLocationAction::DragEnd() { location->position = SnapPosition(*this); }

void DragLocationAction::DragDraw(gui::DrawContext&) {
  // Location is drawn by its parent Machine so nothing to do here.
}

SkRect DragLocationAction::DragBox() { return location->object->Shape().getBounds(); }
}  // namespace automat
