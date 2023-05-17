#include "drag_action.h"

#include "pointer.h"
#include "root.h"
#include <algorithm>

namespace automaton {

static vec2 RoundToMilimeters(vec2 v) {
  return Vec2(round(v.X * 1000) / 1000., round(v.Y * 1000) / 1000.);
}

void DragActionBase::Begin(gui::Pointer &pointer) {
  current_position = pointer.PositionWithin(*root_machine);
}

vec2 DragActionBase::TargetPosition() const {
  return current_position - contact_point;
}

vec2 DragActionBase::TargetPositionRounded() const {
  return RoundToMilimeters(TargetPosition());
}

void DragActionBase::Update(gui::Pointer &pointer) {
  auto old_pos = current_position - contact_point;
  auto old_round = RoundToMilimeters(old_pos);
  current_position = pointer.PositionWithin(*root_machine);
  auto new_pos = current_position - contact_point;
  auto new_round = RoundToMilimeters(new_pos);
  vec2 d = new_pos - old_pos;
  if (old_round.X != new_round.X && abs(d.X) < 0.0005f) {
    for (auto &rx : round_x) {
      rx.value += old_round.X - new_round.X;
      rx.value = std::clamp(rx.value, -0.001f, 0.001f);
    }
  }
  if (old_round.Y != new_round.Y && abs(d.Y) < 0.0005f) {
    for (auto &ry : round_y) {
      ry.value += old_round.Y - new_round.Y;
      ry.value = std::clamp(ry.value, -0.001f, 0.001f);
    }
  }
  DragUpdate();
}

void DragActionBase::End() { DragEnd(); }

void DragActionBase::Draw(SkCanvas &canvas, animation::State &animation_state) {
  auto original = current_position - contact_point;
  auto rounded = RoundToMilimeters(original);

  auto &rx = round_x[animation_state];
  auto &ry = round_y[animation_state];
  rx.Tick(animation_state);
  ry.Tick(animation_state);

  auto pos = rounded + Vec2(rx, ry);
  canvas.translate(pos.X, pos.Y);
  DragDraw(canvas, animation_state);
  canvas.translate(-pos.X, -pos.Y);
}

void DragObjectAction::DragUpdate() {
  // Nothing to do here.
}

void DragObjectAction::DragEnd() {
  Location &loc = root_machine->CreateEmpty();
  loc.position = TargetPositionRounded();
  loc.InsertHere(std::move(object));
}

void DragObjectAction::DragDraw(SkCanvas &canvas,
                                animation::State &animation_state) {
  object->Draw(canvas, animation_state);
}

DragLocationAction::DragLocationAction(Location *location)
    : location(location) {
  location->drag_action = this;
}

DragLocationAction::~DragLocationAction() {
  assert(location->drag_action == this);
  location->drag_action = nullptr;
}

void DragLocationAction::DragUpdate() {
  location->position = TargetPositionRounded();
}

void DragLocationAction::DragEnd() {
  location->position = TargetPositionRounded();
}

void DragLocationAction::DragDraw(SkCanvas &canvas,
                                  animation::State &animation_state) {
  // Location is drawn by its parent Machine so nothing to do here.
}

} // namespace automaton
