#include "drag_action.hh"

#include <cmath>

#include "animation.hh"
#include "math.hh"
#include "pointer.hh"
#include "root.hh"
#include "time.hh"
#include "units.hh"
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
  maf::Fn<void(LocationAnimationState&)> callback;

  if (pointer.window.IsOverTrash(pointer.pointer_position)) {
    gui::Path root_machine_path;
    root_machine_path.push_back(&pointer.window);
    root_machine_path.push_back(root_machine);
    SkMatrix mat = TransformDown(root_machine_path, &pointer.window.display);
    auto drag_box = DragBox();
    Vec2 box_size = Vec2(drag_box.width(), drag_box.height());
    float diagonal = Length(box_size);

    Vec2 actual_target_position =
        mat.mapPoint(pointer.window.size - box_size / diagonal * pointer.window.trash_radius / 2) -
        drag_box.center();

    float target_scale = mat.mapRadius(pointer.window.trash_radius) / diagonal * 0.9f;
    callback = [=](LocationAnimationState& anim) {
      anim.scale.target = target_scale;
      anim.position_offset.target = actual_target_position - curr_round;
      anim.position_offset.value += last_round - curr_round;
    };
  } else if (last_round != curr_round && Length(current_position - last_position) < 0.5_mm) {
    callback = [delta = last_round - curr_round](LocationAnimationState& anim) {
      anim.scale.target = 1;
      anim.position_offset.target = Vec2(0, 0);
      anim.position_offset.value += delta;
      anim.position_offset.value.x = std::clamp(anim.position_offset.value.x, -1_mm, 1_mm);
      anim.position_offset.value.y = std::clamp(anim.position_offset.value.y, -1_mm, 1_mm);
    };
  } else {
    callback = [](LocationAnimationState& anim) {
      anim.scale.target = 1;
      anim.position_offset.target = Vec2(0, 0);
      anim.position_offset.value.x = std::clamp(anim.position_offset.value.x, -1_mm, 1_mm);
      anim.position_offset.value.y = std::clamp(anim.position_offset.value.y, -1_mm, 1_mm);
    };
  }
  DragUpdate(curr_round, callback);
}

DragObjectAction::DragObjectAction(std::unique_ptr<Object>&& object_arg)
    : object(std::move(object_arg)) {
  anim = std::make_unique<LocationAnimationState>();
}

DragObjectAction::~DragObjectAction() {}

void DragObjectAction::DragUpdate(Vec2 pos, maf::Fn<void(LocationAnimationState& state)> callback) {
  callback(*anim);
  position = pos;
}

void DragLocationAction::DragUpdate(Vec2 pos,
                                    maf::Fn<void(LocationAnimationState& anim)> callback) {
  for (auto display : animation::displays) {
    auto& animation_state = location->animation_state[*display];
    callback(animation_state);
  }
  location->position = pos;
}

void DragActionBase::End() { DragEnd(); }

void DragActionBase::DrawAction(gui::DrawContext& ctx) { DragDraw(ctx); }

void DragObjectAction::DragEnd() {
  Location& loc = root_machine->CreateEmpty();
  loc.position = position;
  loc.InsertHere(std::move(object));
}

void DragObjectAction::DragDraw(gui::DrawContext& ctx) {
  anim->position_offset.Tick(ctx.display);
  anim->scale.Tick(ctx.display);

  auto mat = SkMatrix::Translate(-position);
  anim->Apply(mat, *object);

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
