#include "gui_connection_widget.h"

#include <cassert>

#include <include/core/SkColor.h>

#include "base.h"
#include "font.h"
#include "gui_constants.h"
#include "log.h"

namespace automaton::gui {

struct ConnectionLabelWidget : Widget {
  ConnectionWidget *parent;
  std::string label;
  ConnectionLabelWidget(ConnectionWidget* parent, std::string_view label) : parent(parent), label(label) {
    this->label = "👆" + this->label + " ";
  }
  float Width() const {
    return GetFont().MeasureText(label);
  }
  float Height() const {
    return kLetterSize;
  }
  SkPath Shape() const override {
    float w = Width();
    float h = Height();
    return SkPath::Rect(SkRect::MakeWH(w, h));
  }
  void Draw(SkCanvas &canvas, animation::State &state) const override {
    SkPaint paint;
    DrawColored(canvas, state, paint);
  }

  void DrawColored(SkCanvas &canvas, animation::State &state, const SkPaint &paint) const override {
    auto &font = GetFont();
    canvas.translate(-Width()/2, -Height()/2);
    font.DrawText(canvas, label, paint);
    canvas.translate(Width()/2, Height()/2);
  }
};

ConnectionWidget::ConnectionWidget(Location *from, std::string_view label)
    : Button(from->ParentWidget(), std::make_unique<ConnectionLabelWidget>(this, label)), from(from), label(label) {}

Widget *ConnectionWidget::ParentWidget() {
  // This sholud return the Machine which holds the `from` Location.
  return from->ParentWidget();
}

vec2 ConnectionWidget::Position() const {
  SkRect from_bounds = from->Shape().getBounds();
  return Vec2(from->position.X + from_bounds.left(),
              from->position.Y + from_bounds.top() - kMargin - kMinimalTouchableSize);
}

void ConnectionWidget::Draw(SkCanvas &canvas, animation::State &state) const {
  Button::Draw(canvas, state);
  SkPaint paint;
  paint.setAntiAlias(true);
  SkPath path = Shape();
  SkRect bounds = path.getBounds();

  auto [a, b] = from->outgoing.equal_range(label);
  for (auto it = a; it != b; ++it) {
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
  SkRect b = Shape().getBounds();
  return Vec2(b.centerX(), b.centerY());
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
