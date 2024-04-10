#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>

#include "base.hh"
#include "connector_optical.hh"
#include "location.hh"

namespace automat::gui {

ConnectionWidget::ConnectionWidget(Location& from, Argument& arg) : from(from), arg(arg) {}

SkPath ConnectionWidget::Shape() const {
  SkRect black_metal_rect =
      SkRect::MakeLTRB(position.x - 0.004, position.y, position.x + 0.004, position.y + 0.008);
  SkPath path = SkPath::Rect(black_metal_rect);
  return path;
}

void ConnectionWidget::Draw(DrawContext& ctx) const {
  SkCanvas& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;

  SkPath from_shape = from.ArgShape(arg);
  Vec2 from_point = Rect::BottomCenter(from_shape.getBounds()) + from.position;

  Vec2 to_point;
  if (auto it = from.outgoing.find(arg.name); it != from.outgoing.end()) {
    // Because the ConnectionWidget is a child of Location, we must transform the coordinates of its
    // destination to local space.
    Widget* parent_machine = ctx.path[ctx.path.size() - 2];
    SkMatrix parent_to_local =
        TransformDown(Path{parent_machine, (Widget*)this}, ctx.animation_context);

    Connection* c = it->second;
    Location& to = c->to;
    SkPath to_shape;
    if (to.object) {
      to_shape = to.object->Shape();
    }
    SkMatrix m = TransformUp(Path{parent_machine, &to}, ctx.animation_context);
    m.postConcat(parent_to_local);
    to_shape.transform(m);
    to_point = Rect::TopCenter(to_shape.getBounds());
    position = to_point;
  } else {
    to_point = position;
  }

  DrawOpticalConnector(ctx, state, from_point, to_point);
}

std::unique_ptr<Action> ConnectionWidget::ButtonDownAction(Pointer&, PointerButton) {
  return std::make_unique<DragConnectionAction>(*this);
}

DragConnectionAction::DragConnectionAction(ConnectionWidget& widget) : widget(widget) {}

DragConnectionAction::~DragConnectionAction() {
  if (Machine* m = widget.from.ParentAs<Machine>()) {
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
  if (auto it = widget.from.outgoing.find(widget.arg.name); it != widget.from.outgoing.end()) {
    delete it->second;
  }

  grab_offset = pointer.PositionWithin(widget.from) - widget.position;

  animation_context = &pointer.AnimationContext();
  if (Machine* m = widget.from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      if (CanConnect(widget.from, *l, widget.arg)) {
        l->highlight_ptr[*animation_context].target = 1;
      } else {
        l->highlight_ptr[*animation_context].target = 0;
      }
    }
  }
}

void DragConnectionAction::Update(gui::Pointer& pointer) {
  Vec2 new_position = pointer.PositionWithin(widget.from);
  widget.position = new_position - grab_offset;
}

void DragConnectionAction::End() {
  Machine* m = widget.from.ParentAs<Machine>();
  Location* to = m->LocationAtPoint(widget.position);
  if (to != nullptr && CanConnect(widget.from, *to, widget.arg)) {
    widget.from.ConnectTo(*to, widget.arg.name);
  }
}

}  // namespace automat::gui
