#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>

#include "base.hh"
#include "connector_optical.hh"
#include "location.hh"

namespace automat::gui {

static Vec2 StartPosition(const ConnectionWidget& widget) {
  SkPath from_shape = widget.from.ArgShape(widget.arg);
  return Rect::BottomCenter(from_shape.getBounds()) + widget.from.position;
}

ConnectionWidget::ConnectionWidget(Location& from, Argument& arg) : from(from), arg(arg) {
  if (arg.name == "next") {
    state.emplace(StartPosition(*this));
  }
}

SkPath ConnectionWidget::Shape() const {
  if (state) {
    Vec2 bottom_center = state->PlugBottomCenter();
    SkRect black_metal_rect = SkRect::MakeLTRB(bottom_center.x - 0.004, bottom_center.y,
                                               bottom_center.x + 0.004, bottom_center.y + 0.008);
    SkPath path = SkPath::Rect(black_metal_rect);
    return path;
  } else {
    return SkPath();
  }
}

void ConnectionWidget::Draw(DrawContext& ctx) const {
  SkCanvas& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;

  if (state) {
    Vec2 from_point = StartPosition(*this);

    Optional<Vec2> to_point;
    if (auto it = from.outgoing.find(arg.name); it != from.outgoing.end()) {
      // Because the ConnectionWidget is a child of Location, we must transform the coordinates of
      // its destination to local space.
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
    } else {
      to_point = manual_position;
    }

    float dt = ctx.animation_context.timer.d;
    SimulateCablePhysics(dt, *state, from_point, to_point);
    DrawOpticalConnector(ctx, *state);
  } else {
    SkPath from_shape = from.ArgShape(arg);
    from_shape.offset(from.position.x, from.position.y);
    SkPath to_shape;
    if (auto it = from.outgoing.find(arg.name); it != from.outgoing.end()) {
      Connection* c = it->second;
      Location* to_location = &c->to;
      if (to_location) {
        Object* to_object = to_location->object.get();
        if (to_object) {
          to_shape = to_object->Shape();
          to_shape.offset(to_location->position.x, to_location->position.y);
        }
      }
    }
    if (to_shape.isEmpty()) {
      if (manual_position) {
        to_shape.moveTo(*manual_position);
      } else {
        return;
      }
    }

    DrawArrow(canvas, from_shape, to_shape);
  }
}

std::unique_ptr<Action> ConnectionWidget::ButtonDownAction(Pointer&, PointerButton) {
  return std::make_unique<DragConnectionAction>(*this);
}

DragConnectionAction::DragConnectionAction(ConnectionWidget& widget) : widget(widget) {}

DragConnectionAction::~DragConnectionAction() {
  widget.manual_position.reset();
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

  if (widget.state) {
    grab_offset =
        pointer.PositionWithin(*widget.from.ParentAs<Machine>()) - widget.state->PlugBottomCenter();
  } else {
    grab_offset = Vec2(0, 0);
  }

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
  Vec2 new_position = pointer.PositionWithin(*widget.from.ParentAs<Machine>());
  widget.manual_position = new_position - grab_offset;
}

void DragConnectionAction::End() {
  Machine* m = widget.from.ParentAs<Machine>();
  Vec2 pos;
  if (widget.state) {
    pos = widget.state->PlugBottomCenter();
  } else if (widget.manual_position) {
    pos = *widget.manual_position;
  } else {
    return;
  }
  Location* to = m->LocationAtPoint(pos);
  if (to != nullptr && CanConnect(widget.from, *to, widget.arg)) {
    widget.from.ConnectTo(*to, widget.arg.name);
  }
}

}  // namespace automat::gui
