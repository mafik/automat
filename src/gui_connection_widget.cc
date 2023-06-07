#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>

#include <cassert>
#include <numbers>

#include "base.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "log.hh"
#include "svg.hh"

namespace automat::gui {

struct ConnectionLabelWidget : Widget {
  ConnectionWidget* parent;
  std::string label;
  ConnectionLabelWidget(ConnectionWidget* parent, std::string_view label)
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
  void Draw(DrawContext& ctx) const override {
    SkPaint paint;
    auto& canvas = ctx.canvas;
    auto& font = GetFont();
    canvas.translate(-Width() / 2, -Height() / 2);
    font.DrawText(canvas, label, paint);
    canvas.translate(Width() / 2, Height() / 2);
  }
};

ConnectionWidget::ConnectionWidget(Location* from, std::string_view label)
    : Button(std::make_unique<ConnectionLabelWidget>(this, label)), from(from), label(label) {}

constexpr char kArrowShapeSVG[] = "M-13-8c-3 0-3 16 0 16 3-1 10-5 13-8-3-3-10-7-13-8z";

const SkPath kArrowShape = PathFromSVG(kArrowShapeSVG);

void DrawConnection(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape) {
  SkColor color = 0xff6e4521;
  SkPaint line_paint;
  line_paint.setAntiAlias(true);
  line_paint.setStyle(SkPaint::kStroke_Style);
  line_paint.setStrokeWidth(0.0005);
  line_paint.setColor(color);
  SkPaint arrow_paint;
  arrow_paint.setAntiAlias(true);
  arrow_paint.setStyle(SkPaint::kFill_Style);
  arrow_paint.setColor(color);
  SkRRect from_rrect, to_rrect;
  bool from_is_rrect = from_shape.isRRect(&from_rrect);
  bool to_is_rrect = to_shape.isRRect(&to_rrect);

  // Find an area where the start of a connection can freely move.
  SkRect from_inner;
  if (from_is_rrect) {
    SkVector radii = from_rrect.getSimpleRadii();
    from_inner = from_rrect.rect().makeInset(radii.x(), radii.y());
  } else {
    Vec2 from_center = from_shape.getBounds().center();
    from_inner = SkRect::MakeXYWH(from_center.x, from_center.y, 0, 0);
  }
  // Find an area where the end of a connection can freely move.
  SkRect to_inner;
  if (to_is_rrect) {
    SkVector radii = to_rrect.getSimpleRadii();
    to_inner = to_rrect.rect().makeInset(radii.x(), radii.y());
  } else {
    Vec2 to_center = to_shape.getBounds().center();
    to_inner = SkRect::MakeXYWH(to_center.x, to_center.y, 0, 0);
  }

  Vec2 from, to;
  // Set the vertical positions of the connection endpoints.
  float left = std::max(from_inner.left(), to_inner.left());
  float right = std::min(from_inner.right(), to_inner.right());
  if (left <= right) {
    from.x = to.x = (left + right) / 2;
  } else if (from_inner.right() < to_inner.left()) {
    from.x = from_inner.right();
    to.x = to_inner.left();
  } else {
    from.x = from_inner.left();
    to.x = to_inner.right();
  }
  // Set the horizontal positions of the connection endpoints.
  float top = std::max(from_inner.top(), to_inner.top());
  float bottom = std::min(from_inner.bottom(), to_inner.bottom());
  if (top <= bottom) {
    from.y = to.y = (top + bottom) / 2;
  } else if (from_inner.bottom() < to_inner.top()) {
    from.y = from_inner.bottom();
    to.y = to_inner.top();
  } else {
    from.y = from_inner.top();
    to.y = to_inner.bottom();
  }
  // Find polar coordinates of the connection.
  SkVector delta = to - from;
  float degrees = 180 * std::atan2(delta.y(), delta.x()) / std::numbers::pi;
  float end = delta.length();
  float start = 0;
  if (from_is_rrect) {
    start = std::min(start + from_rrect.getSimpleRadii().fX, end);
  }
  if (to_is_rrect) {
    end = std::max(start, end - to_rrect.getSimpleRadii().fX);
  }
  float line_end = std::max(start, end + kArrowShape.getBounds().centerX());
  // Draw the connection.
  canvas.save();
  canvas.translate(from.x, from.y);
  canvas.rotate(degrees);
  if (start < line_end) {
    canvas.drawLine(start, 0, line_end, 0, line_paint);
  }
  canvas.translate(end, 0);
  canvas.drawPath(kArrowShape, arrow_paint);
  canvas.restore();
}

void ConnectionWidget::Draw(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;
  SkPath my_shape = Shape();

  if (ctx.path.size() >= 2) {
    // Assume that `from` and `to` are sharing the same parent Machine.
    Widget* parent_location = ctx.path[ctx.path.size() - 1];
    Widget* parent_machine = ctx.path[ctx.path.size() - 2];
    SkMatrix parent_to_local =
        TransformDown(Path{parent_machine, parent_location, (Widget*)this}, actx);
    auto [a, b] = from->outgoing.equal_range(label);
    for (auto it = a; it != b; ++it) {
      Connection& c = *it->second;
      Location& to = c.to;
      SkPath to_shape = to.Shape();

      SkMatrix m = TransformUp(Path{parent_machine, &to}, actx);
      m.postConcat(parent_to_local);
      to_shape.transform(m);
      DrawConnection(canvas, my_shape, to_shape);
    }
  }

  Button::Draw(ctx);
}

std::unique_ptr<Action> ConnectionWidget::ButtonDownAction(Pointer&, PointerButton) {
  if (drag_action != nullptr) {
    return nullptr;
  }
  return std::make_unique<DragConnectionAction>(this);
}

DragConnectionAction::DragConnectionAction(ConnectionWidget* widget) : widget(widget) {
  assert(widget->drag_action == nullptr);
  widget->drag_action = this;
}

DragConnectionAction::~DragConnectionAction() {
  assert(widget->drag_action == this);
  widget->drag_action = nullptr;
}

void DragConnectionAction::Begin(gui::Pointer& pointer) {
  const Path& path = pointer.Path();
  for (int i = path.size() - 1; i >= 0; --i) {
    if (auto m = dynamic_cast<Machine*>(path[i])) {
      current_position = pointer.PositionWithin(*m);
      return;
    }
  }
}

void DragConnectionAction::Update(gui::Pointer& pointer) {
  const Path& path = pointer.Path();
  for (int i = path.size() - 1; i >= 0; --i) {
    if (auto m = dynamic_cast<Machine*>(path[i])) {
      current_position = pointer.PositionWithin(*m);
      return;
    }
  }
}

void DragConnectionAction::End() {
  Location* from = widget->from;
  Machine* m = from->ParentAs<Machine>();
  Location* to = m->LocationAtPoint(current_position);
  if (to != nullptr) {
    from->ConnectTo(*to, widget->label);
  }
}

void DragConnectionAction::DrawAction(DrawContext& ctx) {
  SkPath from_shape = widget->Shape();
  if (widget != nullptr) {
    if (Location* parent_location = widget->from) {
      if (Machine* parent_machine = parent_location->ParentAs<Machine>()) {
        SkMatrix from_transform =
            TransformUp({parent_machine, parent_location, widget}, ctx.animation_context);
        from_shape.transform(from_transform);
      }
    }
  }
  SkPath to_shape = SkPath();
  to_shape.moveTo(current_position.x, current_position.y);
  DrawConnection(ctx.canvas, from_shape, to_shape);
}

}  // namespace automat::gui
