#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

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

SkPath ConnectionWidget::Shape(animation::Display*) const {
  if (state) {
    return state->Shape(nullptr);
  } else {
    return SkPath();
  }
}
void ConnectionWidget::PreDraw(DrawContext& ctx) const {
  if (arg.autoconnect_radius > 0) {
    auto anim = animation_state.Find(ctx.display);
    if (anim == nullptr) {
      return;
    }
    float target = anim->radar_alpha_target;
    if (auto to = arg.FindLocation(from, Argument::IfMissing::ReturnNull)) {
      target = 0;
    }
    animation::LinearApproach(target, ctx.DeltaT(), 2.f, anim->radar_alpha);
    if (anim->radar_alpha < 0.01f) {
      return;
    }
    auto pos_dir = from.ArgStart(&ctx.display, arg);
    SkPaint radius_paint;
    SkColor colors[] = {SkColorSetA(arg.tint, 0),
                        SkColorSetA(arg.tint, (int)(anim->radar_alpha * 96)), SK_ColorTRANSPARENT};
    float pos[] = {0, 1, 1};
    constexpr float kPeriod = 2.f;
    auto local_matrix =
        SkMatrix::RotateRad(ctx.display.timer.steady_now.time_since_epoch().count() * 2 * M_PI /
                            kPeriod)
            .postTranslate(pos_dir.pos.x, pos_dir.pos.y);
    radius_paint.setShader(SkGradientShader::MakeSweep(0, 0, colors, pos, 3, SkTileMode::kClamp, 0,
                                                       60, 0, &local_matrix));
    // TODO: switch to drawArc instead
    ctx.canvas.drawCircle(pos_dir.pos, arg.autoconnect_radius, radius_paint);
  }
}

void ConnectionWidget::Draw(DrawContext& ctx) const {
  SkCanvas& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& from_animation_state = from.GetAnimationState(display);
  bool using_layer = false;
  if (from_animation_state.transparency > 0.01f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, 1.f - from_animation_state.transparency);
  }
  SkPath from_shape = from.Shape(nullptr);
  if (arg.field) {
    from_shape = from.FieldShape(*arg.field);
  }
  SkPath to_shape;              // machine coords
  SkPath to_shape_from_coords;  // from's coords
  Widget* parent_machine = ctx.path[ctx.path.size() - 2];
  Vec<Vec2AndDir> to_points;  // machine coords
  Location* to = nullptr;

  auto pos_dir = from.ArgStart(&display, arg);

  if ((to = arg.FindLocation(from, Argument::IfMissing::ReturnNull))) {
    to_shape = to->object->Shape(nullptr);
    to->object->ConnectionPositions(to_points);
    SkMatrix m = TransformUp(Path{parent_machine, to}, &ctx.display);
    for (auto& vec_and_dir : to_points) {
      vec_and_dir.pos = m.mapPoint(vec_and_dir.pos);
    }
    to_shape.transform(m);
    to_shape.transform(TransformDown(Path{parent_machine, (Widget*)&from}, &ctx.display),
                       &to_shape_from_coords);
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
      state->connector_scale.SpringTowards(to->scale, ctx.DeltaT(), Location::kSpringPeriod,
                                           Location::kSpringHalfTime);
    } else {
      state->steel_insert_hidden.target = 0;
      state->connector_scale.SpringTowards(from.scale, ctx.DeltaT(), Location::kSpringPeriod,
                                           Location::kSpringHalfTime);
    }
    state->steel_insert_hidden.Tick(display);

    SimulateCablePhysics(ctx, ctx.DeltaT(), *state, pos_dir, to_points);
    DrawOpticalConnector(ctx, *state, arg.Icon());
  } else {
    if (arg.style == Argument::Style::Arrow) {
      if (to_shape.isEmpty()) {
        if (!to_points.empty()) {
          to_shape.moveTo(to_points[0].pos);
          DrawArrow(canvas, from_shape, to_shape);
        }
      }
      if (!to_shape.isEmpty()) {
        DrawArrow(canvas, from_shape, to_shape);
      }
    } else {
      cable_width.target = to != nullptr ? 2_mm : 0;
      cable_width.speed = 5;
      cable_width.Tick(ctx.display);

      if (cable_width > 0.01_mm && to) {
        Vec2AndDir start = {.pos = Vec2(2.2_cm, 1_mm), .dir = -90_deg};
        auto arcline = RouteCable(ctx, pos_dir, to_points);
        auto color = SkColorSetA(arg.tint, 255 * cable_width.value / 2_mm);
        auto color_filter = color::MakeTintFilter(color, 30);
        auto path = arcline.ToPath(false);
        DrawCable(ctx, path, color_filter, CableTexture::Smooth, cable_width.value,
                  cable_width.value);
      }
    }
  }
  if (using_layer) {
    canvas.restore();
  }
}

std::unique_ptr<Action> ConnectionWidget::ButtonDownAction(Pointer& pointer, PointerButton) {
  return std::make_unique<DragConnectionAction>(pointer, *this);
}

DragConnectionAction::DragConnectionAction(Pointer& pointer, ConnectionWidget& widget)
    : Action(pointer), widget(widget) {}

DragConnectionAction::~DragConnectionAction() {
  widget.manual_position.reset();
  if (Machine* m = widget.from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      if (auto* anim = l->animation_state.Find(*display)) {
        anim->highlight.target = 0;
      }
    }
  }
}

bool CanConnect(Location& from, Location& to, Argument& arg) {
  std::string error;
  arg.CheckRequirements(from, &to, to.object.get(), error);
  return error.empty();
}

void DragConnectionAction::Begin() {
  if (auto it = widget.from.outgoing.find(widget.arg.name); it != widget.from.outgoing.end()) {
    delete it->second;
  }

  grab_offset = Vec2(0, 0);
  if (widget.state) {
    // Position within parent machine
    auto pointer_pos = pointer.PositionWithin(*widget.from.ParentAs<Machine>());
    auto mat = widget.state->ConnectorMatrix();
    SkMatrix mat_inv;
    if (mat.invert(&mat_inv)) {
      grab_offset = mat_inv.mapXY(pointer_pos.x, pointer_pos.y);
    }
    widget.manual_position = pointer_pos - grab_offset * widget.state->connector_scale;
  }

  display = &pointer.AnimationContext();
  if (Machine* m = widget.from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      if (CanConnect(widget.from, *l, widget.arg)) {
        l->GetAnimationState(*display).highlight.target = 1;
      } else {
        l->GetAnimationState(*display).highlight.target = 0;
      }
    }
  }
}

void DragConnectionAction::Update() {
  Vec2 new_position = pointer.PositionWithin(*widget.from.ParentAs<Machine>());
  widget.manual_position = new_position - grab_offset * widget.state->connector_scale;
}

void DragConnectionAction::End() {
  Machine* m = widget.from.ParentAs<Machine>();
  Vec2 pos;
  if (widget.state) {
    pos = widget.state->ConnectorMatrix().mapPoint({});
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
