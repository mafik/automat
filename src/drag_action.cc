#include "drag_action.hh"

#include "animation.hh"
#include "pointer.hh"
#include "root.hh"
#include "units.hh"

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(roundf(v.x * 1000) / 1000., roundf(v.y * 1000) / 1000.);
}

void DragActionBase::Begin(gui::Pointer& pointer) {
  last_position = current_position = pointer.PositionWithinRootMachine();
}

Vec2 DragActionBase::TargetPosition() const { return current_position - contact_point; }

Vec2 DragActionBase::TargetPositionRounded() const { return RoundToMilimeters(TargetPosition()); }

void DragActionBase::Update(gui::Pointer& pointer) {
  last_position = current_position;
  current_position = pointer.PositionWithinRootMachine();
  DragUpdate();
}

void DragActionBase::End() { DragEnd(); }

void DragActionBase::DrawAction(gui::DrawContext& ctx) { DragDraw(ctx); }

void DragObjectAction::DragUpdate() {
  auto last_round = RoundToMilimeters(last_position - contact_point);
  auto curr_round = RoundToMilimeters(current_position - contact_point);
  if (last_round != curr_round && Length(current_position - last_position) < 0.5_mm) {
    auto& offset = position_offset;
    offset.acceleration = 1000;
    offset.friction = 50;
    offset.value += last_round - curr_round;
    offset.value.x = std::clamp(offset.value.x, -1_mm, 1_mm);
    offset.value.y = std::clamp(offset.value.y, -1_mm, 1_mm);
  }
}

void DragObjectAction::DragEnd() {
  Location& loc = root_machine->CreateEmpty();
  loc.position = TargetPositionRounded();
  loc.InsertHere(std::move(object));
}

void DragObjectAction::DragDraw(gui::DrawContext& ctx) {
  position_offset.Tick(ctx.display);
  auto pos = TargetPositionRounded() + position_offset.value;
  ctx.canvas.translate(pos.x, pos.y);
  object->Draw(ctx);
  ctx.canvas.translate(-pos.x, -pos.y);
}

DragLocationAction::DragLocationAction(Location* location) : location(location) {}

DragLocationAction::~DragLocationAction() {}

void DragLocationAction::DragUpdate() {
  auto last_round = RoundToMilimeters(last_position - contact_point);
  auto curr_round = RoundToMilimeters(current_position - contact_point);
  if (last_round != curr_round && Length(current_position - last_position) < 0.5_mm) {
    for (auto display : animation::displays) {
      auto& animation_state = location->animation_state[*display];
      auto& offset = animation_state.position_offset;
      offset.acceleration = 1000;
      offset.friction = 50;
      offset.value += last_round - curr_round;
      offset.value.x = std::clamp(offset.value.x, -1_mm, 1_mm);
      offset.value.y = std::clamp(offset.value.y, -1_mm, 1_mm);
    }
  }
  location->position = curr_round;
}

void DragLocationAction::DragEnd() { location->position = TargetPositionRounded(); }

void DragLocationAction::DragDraw(gui::DrawContext&) {
  // Location is drawn by its parent Machine so nothing to do here.
}
}  // namespace automat
