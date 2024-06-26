#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>

#include "base.hh"
#include "connector_optical.hh"
#include "location.hh"
#include "object.hh"

namespace automat::gui {

struct DummyRunnable : Object, Runnable {
  LongRunning* OnRun(Location& here) override { return nullptr; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<DummyRunnable>(); }
} kDummyRunnable;

static bool IsArgumentOptical(Location& from, Argument& arg) {
  Str error;
  arg.CheckRequirements(from, nullptr, &kDummyRunnable, error);
  return error.empty();
}

ConnectionWidget::ConnectionWidget(Location& from, Argument& arg) : from(from), arg(arg) {
  if (IsArgumentOptical(from, arg)) {
    auto pos_dir = from.ArgStart(nullptr, arg);
    state.emplace(from, arg, pos_dir);
    state->tint = arg.tint;
  }
}

SkPath ConnectionWidget::Shape() const {
  if (state) {
    return state->Shape();
  } else {
    return SkPath();
  }
}

void ConnectionWidget::Draw(DrawContext& ctx) const {
  SkCanvas& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;
  auto& from_animation_state = from.animation_state[actx];
  bool using_layer = false;
  if (from_animation_state.transparency > 0.01f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, 1.f - from_animation_state.transparency);
  }
  SkPath from_shape = from.Shape();
  if (arg.field) {
    from_shape = from.FieldShape(*arg.field);
  }
  SkPath to_shape;              // machine coords
  SkPath to_shape_from_coords;  // from's coords
  Widget* parent_machine = ctx.path[ctx.path.size() - 2];
  Optional<Vec2> to_point;  // machine coords
  Location* to = nullptr;

  auto pos_dir = from.ArgStart(&actx, arg);

  if (auto it = from.outgoing.find(arg.name); it != from.outgoing.end()) {
    Connection* c = it->second;
    to = &c->to;
    if (to->object) {
      to_shape = to->object->Shape();
      SkMatrix m = TransformUp(Path{parent_machine, to}, &ctx.animation_context);
      to_point = m.mapPoint(Rect::TopCenter(to_shape.getBounds()));
      to_shape.transform(m);
      to_shape.transform(
          TransformDown(Path{parent_machine, (Widget*)&from}, &ctx.animation_context),
          &to_shape_from_coords);
    }
  } else {
    to_point = manual_position;
  }

  auto transform_from_to_machine = TransformUp(Path{parent_machine, &from}, &ctx.animation_context);
  from_shape.transform(transform_from_to_machine);

  if (state) {
    if (to) {
      state->steel_insert_hidden.target = 1;
    } else {
      state->steel_insert_hidden.target = 0;
    }
    state->steel_insert_hidden.Tick(actx);

    float dt = ctx.animation_context.timer.d;
    SimulateCablePhysics(dt, *state, pos_dir, to_point);
    DrawOpticalConnector(ctx, *state);
  } else {
    if (to_shape.isEmpty()) {
      if (to_point) {
        to_shape.moveTo(*to_point);
      } else {
        return;
      }
    }
    DrawArrow(canvas, from_shape, to_shape);
  }
  if (using_layer) {
    canvas.restore();
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
      l->animation_state[*animation_context].highlight.target = 0;
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

  grab_offset = Vec2(0, 0);
  if (widget.state) {
    auto pointer_pos = pointer.PositionWithin(*widget.from.ParentAs<Machine>());
    auto mat = widget.state->ConnectorMatrix();
    SkMatrix mat_inv;
    if (mat.invert(&mat_inv)) {
      grab_offset = mat_inv.mapXY(pointer_pos.x, pointer_pos.y);
    }
    widget.manual_position = pointer_pos - grab_offset;
  }

  animation_context = &pointer.AnimationContext();
  if (Machine* m = widget.from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      if (CanConnect(widget.from, *l, widget.arg)) {
        l->animation_state[*animation_context].highlight.target = 1;
      } else {
        l->animation_state[*animation_context].highlight.target = 0;
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
