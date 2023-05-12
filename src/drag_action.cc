#include "drag_action.h"

#include "pointer.h"
#include "root.h"

namespace automaton {

static vec2 RoundToMilimeters(vec2 v) {
  return Vec2(round(v.X * 1000) / 1000., round(v.Y * 1000) / 1000.);
}

void DragActionBase::Begin(gui::Pointer &pointer) {
  current_position = pointer.PositionWithin(*root_machine);
}

void DragActionBase::Update(gui::Pointer &pointer) {
  auto old_pos = current_position - contact_point;
  auto old_round = RoundToMilimeters(old_pos);
  current_position = pointer.PositionWithin(*root_machine);
  auto new_pos = current_position - contact_point;
  auto new_round = RoundToMilimeters(new_pos);
  if (old_round.X == new_round.X) {
    for (auto &rx : round_x) {
      rx.value -= new_pos.X - old_pos.X;
    }
  }
  if (old_round.Y == new_round.Y) {
    for (auto &ry : round_y) {
      ry.value -= new_pos.Y - old_pos.Y;
    }
  }
  DragUpdate(new_pos);
}

void DragActionBase::End() {
  vec2 pos = RoundToMilimeters(current_position - contact_point);
  DragEnd(pos);
}

void DragActionBase::Draw(SkCanvas &canvas, animation::State &animation_state) {
  auto original = current_position - contact_point;
  auto rounded = RoundToMilimeters(original);

  auto &rx = round_x[animation_state];
  auto &ry = round_y[animation_state];
  rx.target = rounded.X - original.X;
  ry.target = rounded.Y - original.Y;
  rx.Tick(animation_state);
  ry.Tick(animation_state);

  canvas.save();
  auto pos = current_position - contact_point + Vec2(rx, ry);
  canvas.translate(pos.X, pos.Y);
  DragDraw(canvas, animation_state);
  canvas.restore();
}

void DragObjectAction::DragUpdate(vec2 pos) {
  // Nothing to do here.
}

void DragObjectAction::DragEnd(vec2 pos) {
  Location &loc = root_machine->CreateEmpty();
  loc.position = pos;
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

void DragLocationAction::DragUpdate(vec2 pos) { location->position = pos; }

void DragLocationAction::DragEnd(vec2 pos) { location->position = pos; }

void DragLocationAction::DragDraw(SkCanvas &canvas,
                                  animation::State &animation_state) {
  // Location is drawn by its parent Machine so nothing to do here.
}

} // namespace automaton
