#include "drag_action.hh"

#include <algorithm>

#include "pointer.hh"
#include "root.hh"

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(round(v.x * 1000) / 1000., round(v.y * 1000) / 1000.);
}

void DragActionBase::Begin(gui::Pointer& pointer) {
  current_position = pointer.PositionWithinRootMachine();
}

Vec2 DragActionBase::TargetPosition() const { return current_position - contact_point; }

Vec2 DragActionBase::TargetPositionRounded() const { return RoundToMilimeters(TargetPosition()); }

void DragActionBase::Update(gui::Pointer& pointer) {
  auto old_pos = current_position - contact_point;
  auto old_round = RoundToMilimeters(old_pos);
  current_position = pointer.PositionWithinRootMachine();
  auto new_pos = current_position - contact_point;
  auto new_round = RoundToMilimeters(new_pos);
  Vec2 d = new_pos - old_pos;
  if (old_round.x != new_round.x && fabs(d.x) < 0.0005f) {
    for (auto& rx : round_x) {
      rx.value += old_round.x - new_round.x;
      rx.value = std::clamp(rx.value, -0.001f, 0.001f);
    }
  }
  if (old_round.y != new_round.y && fabs(d.y) < 0.0005f) {
    for (auto& ry : round_y) {
      ry.value += old_round.y - new_round.y;
      ry.value = std::clamp(ry.value, -0.001f, 0.001f);
    }
  }
  DragUpdate();
}

void DragActionBase::End() { DragEnd(); }

void DragActionBase::DrawAction(gui::DrawContext& ctx) {
  auto& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto original = current_position - contact_point;
  auto rounded = RoundToMilimeters(original);

  auto& rx = round_x[display];
  auto& ry = round_y[display];
  rx.Tick(display);
  ry.Tick(display);

  auto pos = rounded + Vec2(rx, ry);
  canvas.translate(pos.x, pos.y);
  DragDraw(ctx);
  canvas.translate(-pos.x, -pos.y);
}

void DragObjectAction::DragUpdate() {
  // Nothing to do here.
}

void DragObjectAction::DragEnd() {
  Location& loc = root_machine->CreateEmpty();
  loc.position = TargetPositionRounded();
  loc.InsertHere(std::move(object));
}

void DragObjectAction::DragDraw(gui::DrawContext& ctx) { object->Draw(ctx); }

DragLocationAction::DragLocationAction(Location* location) : location(location) {
  location->drag_action = this;
}

DragLocationAction::~DragLocationAction() {
  assert(location->drag_action == this);
  location->drag_action = nullptr;
}

void DragLocationAction::DragUpdate() { location->position = TargetPositionRounded(); }

void DragLocationAction::DragEnd() { location->position = TargetPositionRounded(); }

void DragLocationAction::DragDraw(gui::DrawContext&) {
  // Location is drawn by its parent Machine so nothing to do here.
}

}  // namespace automat
