#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>

#include "argument.hh"
#include "base.hh"
#include "connector_optical.hh"
#include "location.hh"
#include "math.hh"
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
  auto& display = ctx.display;
  auto& from_animation_state = from.animation_state[display];
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
  Vec<Vec2AndDir> to_points;  // machine coords
  Location* to = nullptr;

  auto pos_dir = from.ArgStart(&display, arg);

  if (auto it = from.outgoing.find(arg.name); it != from.outgoing.end()) {
    Connection* c = it->second;
    to = &c->to;
    if (to->object) {
      to_shape = to->object->Shape();
      to->object->ConnectionPositions(to_points);
      SkMatrix m = TransformUp(Path{parent_machine, to}, &ctx.display);
      for (auto& vec_and_dir : to_points) {
        vec_and_dir.pos = m.mapPoint(vec_and_dir.pos);
      }
      to_shape.transform(m);
      to_shape.transform(TransformDown(Path{parent_machine, (Widget*)&from}, &ctx.display),
                         &to_shape_from_coords);
    }
  } else {
    if (manual_position) {
      to_points.emplace_back(Vec2AndDir{
          .pos = *manual_position,
          .dir = -90_deg,
      });
    }
  }

  if (&arg == &next_arg) {
    while (to_points.size() > 1) {
      // from the last two, pick the one which is closer to pointing down (-pi/2)
      float delta_1 = fabs((to_points[to_points.size() - 1].dir + 90_deg).ToRadians());
      float delta_2 = fabs((to_points[to_points.size() - 2].dir + 90_deg).ToRadians());
      if (delta_1 < delta_2) {
        std::swap(to_points[to_points.size() - 1], to_points[to_points.size() - 2]);
      }
      to_points.pop_back();
    }
  }

  auto transform_from_to_machine = TransformUp(Path{parent_machine, &from}, &ctx.display);
  from_shape.transform(transform_from_to_machine);

  if (state) {
    if (to) {
      state->steel_insert_hidden.target = 1;
    } else {
      state->steel_insert_hidden.target = 0;
    }
    state->steel_insert_hidden.Tick(display);

    float dt = ctx.display.timer.d;
    SimulateCablePhysics(ctx, dt, *state, pos_dir, to_points);
    DrawOpticalConnector(ctx, *state, arg.Icon());
  } else {
    if (to_shape.isEmpty()) {
      if (!to_points.empty()) {
        to_shape.moveTo(to_points[0].pos);
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
      l->animation_state[*display].highlight.target = 0;
    }
  }
}

bool CanConnect(Location& from, Location& to, Argument& arg) {
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

  display = &pointer.AnimationContext();
  if (Machine* m = widget.from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      if (CanConnect(widget.from, *l, widget.arg)) {
        l->animation_state[*display].highlight.target = 1;
      } else {
        l->animation_state[*display].highlight.target = 0;
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
