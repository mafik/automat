#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>

#include <cassert>

#include "base.hh"
#include "font.hh"
#include "location.hh"

namespace automat::gui {

struct ConnectionLabelWidget : Widget {
  ConnectionWidget* parent;
  std::string label;
  ConnectionLabelWidget(ConnectionWidget* parent, std::string_view label)
      : parent(parent), label(label) {
    this->label = "»" + this->label + " ";
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

ConnectionWidget::ConnectionWidget(Location& from, Argument& arg)
    : Button(std::make_unique<ConnectionLabelWidget>(this, arg.name)), from(from), arg(arg) {}

void ConnectionWidget::Draw(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;
  SkPath my_shape = Shape();

  Button::Draw(ctx);
}

std::unique_ptr<Action> ConnectionWidget::ButtonDownAction(Pointer&, PointerButton) {
  return std::make_unique<DragConnectionAction>(from, arg);
}

DragConnectionAction::DragConnectionAction(Location& from, Argument& arg) : from(from), arg(arg) {}

DragConnectionAction::~DragConnectionAction() {
  if (Machine* m = from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      l->highlight_ptr[*animation_context].target = 0;
    }
  }
}

bool CanConnect(Location& from, Location& to, Argument& arg) {
  if (&from == &to) return false;
  std::string error;
  arg.CheckRequirements(from, &to, to.object.get(), error);
  return error.empty();
}

void DragConnectionAction::Begin(gui::Pointer& pointer) {
  const Path& path = pointer.Path();
  for (int i = path.size() - 1; i >= 0; --i) {
    if (auto m = dynamic_cast<Machine*>(path[i])) {
      current_position = pointer.PositionWithin(*m);
      break;
    }
  }

  animation_context = &pointer.AnimationContext();
  if (Machine* m = from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      if (CanConnect(from, *l, arg)) {
        l->highlight_ptr[*animation_context].target = 1;
      } else {
        l->highlight_ptr[*animation_context].target = 0;
      }
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
  Machine* m = from.ParentAs<Machine>();
  Location* to = m->LocationAtPoint(current_position);
  if (to != nullptr && CanConnect(from, *to, arg)) {
    from.ConnectTo(*to, arg.name);
  }
}

void DragConnectionAction::DrawAction(DrawContext& ctx) {
  SkPath from_shape = from.ArgShape(arg);
  if (Machine* parent_machine = from.ParentAs<Machine>()) {
    SkMatrix from_transform = TransformUp({parent_machine, &from}, ctx.animation_context);
    from_shape.transform(from_transform);
  }
  SkPath to_shape = SkPath();
  to_shape.moveTo(current_position.x, current_position.y);
  DrawConnection(ctx.canvas, from_shape, to_shape);
}

}  // namespace automat::gui
