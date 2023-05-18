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
  ConnectionLabelWidget(ConnectionWidget *parent, std::string_view label)
      : parent(parent), label(label) {
    this->label = "👆" + this->label + " ";
  }
  float Width() const { return GetFont().MeasureText(label); }
  float Height() const { return kLetterSize; }
  SkPath Shape() const override {
    float w = Width();
    float h = Height();
    return SkPath::Rect(SkRect::MakeWH(w, h));
  }
  void Draw(SkCanvas &canvas, animation::State &state) const override {
    SkPaint paint;
    DrawColored(canvas, state, paint);
  }

  void DrawColored(SkCanvas &canvas, animation::State &state,
                   const SkPaint &paint) const override {
    auto &font = GetFont();
    canvas.translate(-Width() / 2, -Height() / 2);
    font.DrawText(canvas, label, paint);
    canvas.translate(Width() / 2, Height() / 2);
  }
};

ConnectionWidget::ConnectionWidget(Location *from, std::string_view label)
    : Button(from->ParentWidget(),
             std::make_unique<ConnectionLabelWidget>(this, label)),
      from(from), label(label) {}

Widget *ConnectionWidget::ParentWidget() const { return from; }

void DrawConnection(SkCanvas &canvas, const SkPath &from, const SkPath &to) {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(0.001);
  paint.setColor(SK_ColorBLACK);
  SkPoint from_center = from.getBounds().center();
  SkPoint to_center = to.getBounds().center();
  canvas.drawLine(from_center.x(), from_center.y(), to_center.x(),
                  to_center.y(), paint);
}

void ConnectionWidget::Draw(SkCanvas &canvas, animation::State &state) const {
  Button::Draw(canvas, state);
  SkPaint paint;
  paint.setAntiAlias(true);
  SkPath my_shape = Shape();

  auto [a, b] = from->outgoing.equal_range(label);
  for (auto it = a; it != b; ++it) {
    Connection &c = *it->second;
    Location &to = c.to;
    SkPath to_shape = to.Shape();
    to_shape.offset(to.position.X, to.position.Y);
    // TODO: fix jumping somehow
    SkMatrix my_transform = from->ParentWidget()->TransformToChild(this);
    to_shape.transform(my_transform);
    DrawConnection(canvas, my_shape, to_shape);
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
  current_position = pointer.PositionWithin(*widget->from->ParentWidget());
}

void DragConnectionAction::Update(gui::Pointer &pointer) {
  current_position = pointer.PositionWithin(*widget->from->ParentWidget());
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
  SkPath from_path = widget->Shape();
  SkMatrix from_transform =
      widget->from->ParentWidget()->TransformFromChild(widget);
  from_path.transform(from_transform);
  SkPath to_path = SkPath();
  to_path.moveTo(current_position.X, current_position.Y);
  DrawConnection(canvas, from_path, to_path);
}

} // namespace automaton::gui
