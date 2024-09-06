#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>
#include <include/core/SkRSXform.h>
#include <include/core/SkTextBlob.h>
#include <include/effects/SkGradientShader.h>

#include "argument.hh"
#include "base.hh"
#include "connector_optical.hh"
#include "font.hh"
#include "location.hh"
#include "math.hh"
#include "object.hh"
#include "root.hh"
#include "widget.hh"

using namespace maf;

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
  }
}

SkPath ConnectionWidget::Shape(animation::Display* d) const {
  if (state && !state->hidden) {
    return state->Shape(d);
  } else {
    return SkPath();
  }
}

void ConnectionWidget::PreDraw(DrawContext& ctx) const {
  if (arg.autoconnect_radius <= 0) {
    return;
  }
  auto anim = animation_state.Find(ctx.display);
  if (anim == nullptr) {
    return;
  }
  animation::LinearApproach(anim->radar_alpha_target, ctx.DeltaT(), 2.f, anim->radar_alpha);
  float prototype_alpha_target = anim->prototype_alpha_target;
  if (arg.FindLocation(from)) {
    prototype_alpha_target = 0;
  }
  animation::LinearApproach(prototype_alpha_target, ctx.DeltaT(), 2.f, anim->prototype_alpha);
  if (anim->radar_alpha >= 0.01f) {
    gui::DisplayContext fromDisplayCtx = GuessDisplayContext(from, ctx.display);
    fromDisplayCtx.path.erase(fromDisplayCtx.path.begin());  // remove the window
    auto pos_dir = arg.Start(fromDisplayCtx);
    SkPaint radius_paint;
    SkColor colors[] = {SkColorSetA(arg.tint, 0),
                        SkColorSetA(arg.tint, (int)(anim->radar_alpha * 96)), SK_ColorTRANSPARENT};
    float pos[] = {0, 1, 1};
    constexpr float kPeriod = 2.f;
    double t = ctx.display.timer.steady_now.time_since_epoch().count();
    auto local_matrix = SkMatrix::RotateRad(fmod(t * 2 * M_PI / kPeriod, 2 * M_PI))
                            .postTranslate(pos_dir.pos.x, pos_dir.pos.y);
    radius_paint.setShader(SkGradientShader::MakeSweep(0, 0, colors, pos, 3, SkTileMode::kClamp, 0,
                                                       60, 0, &local_matrix));
    // TODO: switch to drawArc instead
    SkRect oval =
        Rect::MakeCenterWH(pos_dir.pos, arg.autoconnect_radius * 2, arg.autoconnect_radius * 2);

    float crt_width =
        animation::SinInterp(anim->radar_alpha, 0.2f, 0.1f, 0.5f, 1.f) * arg.autoconnect_radius * 2;
    float crt_height =
        animation::SinInterp(anim->radar_alpha, 0.4f, 0.1f, 0.8f, 1.f) * arg.autoconnect_radius * 2;
    SkRect crt_oval = Rect::MakeCenterWH(pos_dir.pos, crt_width, crt_height);
    ctx.canvas.drawArc(crt_oval, 0, 360, true, radius_paint);

    SkPaint stroke_paint;
    stroke_paint.setColor(SkColorSetA(arg.tint, (int)(anim->radar_alpha * 128)));
    stroke_paint.setStyle(SkPaint::kStroke_Style);

    float radar_alpha_sin = sin((anim->radar_alpha - 0.5f) * M_PI) * 0.5f + 0.5f;
    radar_alpha_sin *= radar_alpha_sin;
    constexpr float kQuadrantSweep = 80;
    float quadrant_offset = -fmod(t, 360) * 15;
    ctx.canvas.drawArc(crt_oval, quadrant_offset - kQuadrantSweep / 2 * radar_alpha_sin,
                       kQuadrantSweep * radar_alpha_sin, false, stroke_paint);
    ctx.canvas.drawArc(crt_oval, quadrant_offset + 90 - kQuadrantSweep / 2 * radar_alpha_sin,
                       kQuadrantSweep * radar_alpha_sin, false, stroke_paint);
    ctx.canvas.drawArc(crt_oval, quadrant_offset + 180 - kQuadrantSweep / 2 * radar_alpha_sin,
                       kQuadrantSweep * radar_alpha_sin, false, stroke_paint);
    ctx.canvas.drawArc(crt_oval, quadrant_offset + 270 - kQuadrantSweep / 2 * radar_alpha_sin,
                       kQuadrantSweep * radar_alpha_sin, false, stroke_paint);

    auto& font = GetFont();
    SkRSXform transforms[arg.name.size()];
    for (size_t i = 0; i < arg.name.size(); ++i) {
      float i_fract = (i + 1.f) / (arg.name.size() + 1.f);
      // float i_fract = i / (float)arg.name.size();
      float letter_a = (i_fract - 0.5f) * kQuadrantSweep / 180 / 2 * radar_alpha_sin * kPi +
                       quadrant_offset / 180 * kPi;

      float x = sin(letter_a) * arg.autoconnect_radius * radar_alpha_sin;
      float y = cos(letter_a) * arg.autoconnect_radius * radar_alpha_sin;
      float w = font.sk_font.measureText(arg.name.data() + i, 1, SkTextEncoding::kUTF8, nullptr);

      transforms[i] = SkRSXform::MakeFromRadians(font.font_scale, -letter_a, x, y, w / 2, 0);
    }
    auto text_blob =
        SkTextBlob::MakeFromRSXform(arg.name.data(), arg.name.size(), transforms, font.sk_font);
    SkPaint text_paint;
    float text_alpha = animation::SinInterp(anim->radar_alpha, 0.5f, 0.0f, 1.f, 1.f);
    text_paint.setColor(SkColorSetA(arg.tint, (int)(text_alpha * 255)));

    ctx.canvas.save();
    ctx.canvas.translate(pos_dir.pos.x, pos_dir.pos.y);
    ctx.canvas.scale(1, -1);
    ctx.canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    ctx.canvas.rotate(90);
    ctx.canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    ctx.canvas.rotate(90);
    ctx.canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    ctx.canvas.rotate(90);
    ctx.canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    ctx.canvas.restore();

    arg.NearbyCandidates(
        from, arg.autoconnect_radius * 2 + 10_cm,
        [&](Location& candidate, Vec<Vec2AndDir>& to_points) {
          auto arcline = RouteCable(ctx, pos_dir, to_points);
          auto it = ArcLine::Iterator(arcline);
          float total_length = it.AdvanceToEnd() * anim->radar_alpha;
          Vec2 end_point = it.Position();
          float relative_dist = Length(pos_dir.pos - to_points[0].pos) / arg.autoconnect_radius;
          auto path = arcline.ToPath(false, std::lerp(total_length, 0, relative_dist - 1));
          ctx.canvas.drawPath(path, stroke_paint);
          ctx.canvas.drawCircle(end_point, 1_mm, stroke_paint);
        });
  }
  if (anim->prototype_alpha >= 0.01f) {
    auto* proto = from.object->ArgPrototype(arg);
    auto proto_shape = proto->Shape(&ctx.display);
    Rect proto_bounds = proto_shape.getBounds();
    ctx.canvas.save();
    Vec2 offset = from.position + Rect::BottomCenter(from.object->Shape().getBounds()) -
                  proto_bounds.TopCenter();
    ctx.canvas.translate(offset.x, offset.y);
    ctx.canvas.saveLayerAlphaf(&proto_shape.getBounds(), anim->prototype_alpha * 0.4f);
    proto->Draw(ctx);
    ctx.canvas.restore();
    ctx.canvas.restore();
  }
}

animation::Phase ConnectionWidget::Draw(DrawContext& ctx) const {
  SkCanvas& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& from_animation_state = from.GetAnimationState(display);
  SkPath from_shape = from.object->Shape(&display);
  if (arg.field) {
    from_shape = from.FieldShape(*arg.field);
  }
  SkPath to_shape;              // machine coords
  SkPath to_shape_from_coords;  // from's coords
  Vec<Vec2AndDir> to_points;    // machine coords
  Location* to = nullptr;

  // TODO: parent_machine is not necessarily correct.
  // For example when a location is being dragged around, or when there are nested machines.
  Widget* parent_machine = root_machine;

  auto fromDisplayCtx = GuessDisplayContext(from, display);
  fromDisplayCtx.path.erase(fromDisplayCtx.path.begin());  // remove the window
  auto pos_dir = arg.Start(fromDisplayCtx);

  if ((to = arg.FindLocation(from))) {
    to_shape = to->object->Shape(nullptr);
    to->object->ConnectionPositions(to_points);
    Path target_path;
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

  // If one of the to_points is over from_shape, don't draw the cable
  bool overlapping = to_shape.contains(pos_dir.pos.x, pos_dir.pos.y);
  if (!overlapping && !from_shape.isEmpty()) {
    for (auto& to_point : to_points) {
      if (from_shape.contains(to_point.pos.x, to_point.pos.y)) {
        overlapping = true;
        break;
      }
    }
  }
  if (state) {
    state->hidden = overlapping;
  }
  animation::LinearApproach(overlapping ? 1 : 0, ctx.DeltaT(), 5, transparency);

  bool using_layer = false;
  auto alpha = (1.f - from_animation_state.transparency) * (1.f - transparency);

  if (!state.has_value() && arg.style != Argument::Style::Arrow && alpha > 0.01f) {
    auto arcline = RouteCable(ctx, pos_dir, to_points);
    auto new_length = ArcLine::Iterator(arcline).AdvanceToEnd();
    if (new_length > length + 2_cm) {
      alpha = 0;
      transparency = 1;
    }
    length = new_length;
  }

  if (alpha < 0.99f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, alpha);
  }

  auto phase = animation::Finished;
  if (state) {
    if (to) {
      state->steel_insert_hidden.target = 1;
      phase |= state->connector_scale.SpringTowards(
          to->scale, ctx.DeltaT(), Location::kSpringPeriod, Location::kSpringHalfTime);
    } else {
      state->steel_insert_hidden.target = 0;
      phase |= state->connector_scale.SpringTowards(
          from.scale, ctx.DeltaT(), Location::kSpringPeriod, Location::kSpringHalfTime);
    }
    phase |= state->steel_insert_hidden.Tick(display);

    phase |= SimulateCablePhysics(ctx, ctx.DeltaT(), *state, pos_dir, to_points);
    if (alpha > 0.01f) {
      DrawOpticalConnector(ctx, *state, arg.Icon());
    }
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
        if (alpha > 0.01f) {
          auto arcline = RouteCable(ctx, pos_dir, to_points);
          auto color = SkColorSetA(arg.tint, 255 * cable_width.value / 2_mm);
          auto color_filter = color::MakeTintFilter(color, 30);
          auto path = arcline.ToPath(false);
          DrawCable(ctx, path, color_filter, CableTexture::Smooth, cable_width.value,
                    cable_width.value);
        }
      }
    }
  }
  if (using_layer) {
    canvas.restore();
  }
  return phase;
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
  if (auto it = widget.from.outgoing.find(&widget.arg); it != widget.from.outgoing.end()) {
    delete *it;
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
    widget.from.ConnectTo(*to, widget.arg);
  }
}

}  // namespace automat::gui
