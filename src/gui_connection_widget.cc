#include "gui_connection_widget.h"

#include <cassert>

#include <include/core/SkColor.h>

#include "base.h"
#include "font.h"
#include "gui_constants.h"
#include "log.h"

namespace automaton::gui {

ConnectionWidget::ConnectionWidget(Location *from, std::string_view label)
    : from(from), label(label) {}

Widget *ConnectionWidget::ParentWidget() {
  // This sholud return the Machine which holds the `from` Location.
  return from->ParentWidget();
}

SkPath ConnectionWidget::Shape() const {
  SkPath path;
  vec2 center = Center();
  path.addCircle(center.X, center.Y, kRadius);
  return path;
}

void ConnectionWidget::Draw(SkCanvas &canvas, animation::State &state) const {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(SK_ColorRED);
  SkPath path = Shape();
  canvas.drawPath(path, paint);
  auto &font = GetFont();

  SkRect bounds = path.getBounds();
  vec2 text_pos =
      Vec2(bounds.right() + kMargin, bounds.centerY() - kLetterSize / 2);
  canvas.translate(text_pos.X, text_pos.Y);
  font.DrawText(canvas, label, paint);
  canvas.translate(-text_pos.X, -text_pos.Y);

  auto it = from->outgoing.find(label);
  if (it != from->outgoing.end()) {
    Connection &c = *it->second;
    Location &to = c.to;
    SkPoint to_center = to.Shape().getBounds().center();
    to_center.offset(to.position.X, to.position.Y);
    paint.setStyle(SkPaint::kStroke_Style);
    canvas.drawLine(bounds.centerX(), bounds.centerY(), to_center.x(),
                    to_center.y(), paint);
  }
}

std::unique_ptr<Action> ConnectionWidget::ButtonDownAction(Pointer &,
                                                           PointerButton) {
  if (drag_action != nullptr) {
    return nullptr;
  }
  return std::make_unique<DragConnectionAction>(this);
}

vec2 ConnectionWidget::Center() const {
  SkRect from_bounds = from->Shape().getBounds();
  return Vec2(from->position.X + from_bounds.left() + kRadius,
              from->position.Y + from_bounds.top() - kMargin - kRadius);
}

DragConnectionAction::DragConnectionAction(ConnectionWidget *widget)
    : widget(widget) {
  assert(widget->drag_action == nullptr);
  widget->drag_action = this;
}

DragConnectionAction::~DragConnectionAction() {
  assert(widget->drag_action == this);
  widget->drag_action = nullptr;
}

void DragConnectionAction::Begin(gui::Pointer &pointer) {
  current_position = pointer.PositionWithin(*widget);
}

void DragConnectionAction::Update(gui::Pointer &pointer) {
  current_position = pointer.PositionWithin(*widget);
}

void DragConnectionAction::End() {
  Location *from = widget->from;
  Machine *m = from->ParentAs<Machine>();
  Location *to = m->LocationAtPoint(current_position);
  if (to != nullptr) {
    from->ConnectTo(*to, widget->label);
  }
}

void DragConnectionAction::Draw(SkCanvas &canvas,
                                animation::State &animation_state) {
  vec2 from = widget->Center();
  SkPaint paint;
  canvas.drawLine(from.X, from.Y, current_position.X, current_position.Y,
                  paint);
}

} // namespace automaton::gui
