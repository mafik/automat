// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_connection_widget.hh"

#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>
#include <include/core/SkRSXform.h>
#include <include/core/SkTextBlob.h>
#include <include/effects/SkGradientShader.h>

#include <optional>

#include "../build/generated/embedded.hh"
#include "argument.hh"
#include "audio.hh"
#include "automat.hh"
#include "base.hh"
#include "connector_optical.hh"
#include "font.hh"
#include "location.hh"
#include "math.hh"
#include "object.hh"
#include "root_widget.hh"
#include "widget.hh"

using namespace maf;

namespace automat::gui {

struct DummyRunnable : Object, Runnable {
  void OnRun(Location& here) override { return; }
  std::shared_ptr<Object> Clone() const override { return std::make_shared<DummyRunnable>(); }
} kDummyRunnable;

static bool IsArgumentOptical(Location& from, Argument& arg) {
  Str error;
  arg.CheckRequirements(from, nullptr, &kDummyRunnable, error);
  return error.empty();
}

ConnectionWidget::ConnectionWidget(Location& from, Argument& arg) : from(from), arg(arg) {
  if (IsArgumentOptical(from, arg)) {
    auto pos_dir = from.ArgStart(arg);
    state.emplace(from, arg, pos_dir);
  }
}

SkPath ConnectionWidget::Shape() const {
  if (state && transparency < 0.99f) {
    return state->Shape();
  } else {
    return SkPath();
  }
}

void ConnectionWidget::PreDraw(SkCanvas& canvas) const {
  auto anim = &animation_state;
  if (anim->radar_alpha >= 0.01f) {
    auto pos_dir = arg.Start(*from.WidgetForObject(), *root_machine);
    SkPaint radius_paint;
    SkColor colors[] = {SkColorSetA(arg.tint, 0),
                        SkColorSetA(arg.tint, (int)(anim->radar_alpha * 96)), SK_ColorTRANSPARENT};
    float pos[] = {0, 1, 1};
    constexpr float kPeriod = 2.f;
    double t = anim->time_seconds;
    auto local_matrix = SkMatrix::RotateRad(fmod(t * 2 * M_PI / kPeriod, 2 * M_PI))
                            .postTranslate(pos_dir.pos.x, pos_dir.pos.y);
    radius_paint.setShader(SkGradientShader::MakeSweep(0, 0, colors, pos, 3, SkTileMode::kClamp, 0,
                                                       60, 0, &local_matrix));
    // TODO: switch to drawArc instead
    SkRect oval =
        Rect::MakeCenter(pos_dir.pos, arg.autoconnect_radius * 2, arg.autoconnect_radius * 2);

    float crt_width =
        animation::SinInterp(anim->radar_alpha, 0.2f, 0.1f, 0.5f, 1.f) * arg.autoconnect_radius * 2;
    float crt_height =
        animation::SinInterp(anim->radar_alpha, 0.4f, 0.1f, 0.8f, 1.f) * arg.autoconnect_radius * 2;
    SkRect crt_oval = Rect::MakeCenter(pos_dir.pos, crt_width, crt_height);
    canvas.drawArc(crt_oval, 0, 360, true, radius_paint);

    SkPaint stroke_paint;
    stroke_paint.setColor(SkColorSetA(arg.tint, (int)(anim->radar_alpha * 128)));
    stroke_paint.setStyle(SkPaint::kStroke_Style);

    float radar_alpha_sin = sin((anim->radar_alpha - 0.5f) * M_PI) * 0.5f + 0.5f;
    radar_alpha_sin *= radar_alpha_sin;
    constexpr float kQuadrantSweep = 80;
    float quadrant_offset = -fmod(t, 360) * 15;
    canvas.drawArc(crt_oval, quadrant_offset - kQuadrantSweep / 2 * radar_alpha_sin,
                   kQuadrantSweep * radar_alpha_sin, false, stroke_paint);
    canvas.drawArc(crt_oval, quadrant_offset + 90 - kQuadrantSweep / 2 * radar_alpha_sin,
                   kQuadrantSweep * radar_alpha_sin, false, stroke_paint);
    canvas.drawArc(crt_oval, quadrant_offset + 180 - kQuadrantSweep / 2 * radar_alpha_sin,
                   kQuadrantSweep * radar_alpha_sin, false, stroke_paint);
    canvas.drawArc(crt_oval, quadrant_offset + 270 - kQuadrantSweep / 2 * radar_alpha_sin,
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

    canvas.save();
    canvas.translate(pos_dir.pos.x, pos_dir.pos.y);
    canvas.scale(1, -1);
    canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    canvas.rotate(90);
    canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    canvas.rotate(90);
    canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    canvas.rotate(90);
    canvas.drawTextBlob(text_blob, 0, 0, text_paint);
    canvas.restore();

    arg.NearbyCandidates(
        from, arg.autoconnect_radius * 2 + 10_cm,
        [&](Location& candidate, Vec<Vec2AndDir>& to_points) {
          auto m = TransformBetween(*candidate.WidgetForObject(), *root_machine);
          for (auto& to : to_points) {
            to.pos = m.mapPoint(to.pos);
          }
          auto arcline = RouteCable(pos_dir, to_points, &canvas);
          auto it = ArcLine::Iterator(arcline);
          float total_length = it.AdvanceToEnd() * anim->radar_alpha;
          Vec2 end_point = it.Position();
          float relative_dist = Length(pos_dir.pos - to_points[0].pos) / arg.autoconnect_radius;
          auto path = arcline.ToPath(false, std::lerp(total_length, 0, relative_dist - 1));
          canvas.drawPath(path, stroke_paint);
          canvas.drawCircle(end_point, 1_mm, stroke_paint);
        });
  }
  if (anim->prototype_alpha >= 0.01f) {
    auto proto = Widget::ForObject(*from.object->ArgPrototype(arg), *this);
    auto proto_shape = proto->Shape();
    Rect proto_bounds = proto_shape.getBounds();
    canvas.save();
    Vec2 offset = from.position + Rect::BottomCenter(from.WidgetForObject()->Shape().getBounds()) -
                  proto_bounds.TopCenter();
    canvas.translate(offset.x, offset.y);
    canvas.saveLayerAlphaf(&proto_shape.getBounds(), anim->prototype_alpha * 0.4f);
    proto->Draw(canvas);
    canvas.restore();
    canvas.restore();
  }
}

void ConnectionWidget::FromMoved() {
  if (state) {
    if (state->stabilized && !state->stabilized_end.has_value()) {
      auto pos_dir = arg.Start(*from.WidgetForObject(), *root_machine);
      state->stabilized_start = pos_dir.pos;
      state->sections.front().pos = pos_dir.pos;
      state->sections.back().pos = pos_dir.pos;
      return;
    }
    state->stabilized = false;
  }
  WakeAnimation();
}

animation::Phase ConnectionWidget::Tick(time::Timer& timer) {
  if (arg.style == Argument::Style::Invisible) {
    return animation::Finished;
  }
  auto& from_animation_state = from.GetAnimationState();
  SkPath from_shape = from.WidgetForObject()->Shape();
  if (arg.field) {
    from_shape = from.FieldShape(*arg.field);
  }
  SkPath to_shape;  // machine coords
  to_points.clear();

  // TODO: parent_machine is not necessarily correct.
  // For example when a location is being dragged around, or when there are nested machines.
  Widget* parent_machine = root_machine.get();

  auto pos_dir = arg.Start(*from.WidgetForObject(), *parent_machine);

  if ((to = arg.FindLocation(from))) {
    auto to_widget = to->WidgetForObject();
    to_shape = to_widget->Shape();
    to_widget->ConnectionPositions(to_points);
    Path target_path;
    SkMatrix m = TransformBetween(*to_widget, *parent_machine);
    for (auto& vec_and_dir : to_points) {
      vec_and_dir.pos = m.mapPoint(vec_and_dir.pos);
    }
    to_shape.transform(m);
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

  auto transform_from_to_machine = TransformBetween(*from.WidgetForObject(), *parent_machine);
  from_shape.transform(transform_from_to_machine);

  // If one of the to_points is over from_shape, don't draw the cable
  bool overlapping = false;
  if (to != &from && to != nullptr) {
    overlapping = to_shape.contains(pos_dir.pos.x, pos_dir.pos.y);
    if (!overlapping && !from_shape.isEmpty()) {
      for (auto& to_point : to_points) {
        if (from_shape.contains(to_point.pos.x, to_point.pos.y)) {
          overlapping = true;
          break;
        }
      }
    }
  }
  if (state) {
    state->hidden = overlapping;
  }
  auto phase = animation::LinearApproach(overlapping ? 1 : 0, timer.d, 5, transparency);

  auto alpha = (1.f - from_animation_state.transparency) * (1.f - transparency);

  if (!state.has_value() && arg.style != Argument::Style::Arrow && alpha > 0.01f) {
    auto arcline = RouteCable(pos_dir, to_points, nullptr);
    auto new_length = ArcLine::Iterator(arcline).AdvanceToEnd();
    if (new_length > length + 2_cm) {
      alpha = 0;
      transparency = 1;
      phase = animation::Animating;
    }
    length = new_length;
  }

  if (state) {
    if (to) {
      state->steel_insert_hidden.target = 1;
      phase |= state->connector_scale.SpringTowards(
          to->scale, timer.d, Location::kScaleSpringPeriod, Location::kSpringHalfTime);
    } else {
      state->steel_insert_hidden.target = 0;
      phase |= state->connector_scale.SpringTowards(
          from.scale, timer.d, Location::kScaleSpringPeriod, Location::kSpringHalfTime);
    }
    phase |= state->steel_insert_hidden.Tick(timer);

    phase |= SimulateCablePhysics(timer, *state, pos_dir, to_points);
  } else if (arg.style != Argument::Style::Arrow) {
    cable_width.target = to != nullptr ? 2_mm : 0;
    cable_width.speed = 5;
    phase |= cable_width.Tick(timer);
  }

  if (arg.autoconnect_radius > 0) {
    auto& anim = animation_state;
    phase |= animation::LinearApproach(anim.radar_alpha_target, timer.d, 2.f, anim.radar_alpha);
    if (anim.radar_alpha >= 0.01f) {
      phase = animation::Animating;
      anim.time_seconds = timer.NowSeconds();
    }

    float prototype_alpha_target = anim.prototype_alpha_target;
    if (arg.FindLocation(from)) {
      prototype_alpha_target = 0;
    }
    phase |= animation::LinearApproach(prototype_alpha_target, timer.d, 2.f, anim.prototype_alpha);
  }
  return phase;
}

void ConnectionWidget::Draw(SkCanvas& canvas) const {
  if (arg.style == Argument::Style::Invisible) {
    return;
  }
  auto& from_animation_state = from.GetAnimationState();
  auto from_widget = from.WidgetForObject();
  SkPath from_shape = from_widget->Shape();
  if (arg.field) {
    from_shape = from.FieldShape(*arg.field);
  }
  SkPath to_shape;  // machine coords

  // TODO: parent_machine is not necessarily correct.
  // For example when a location is being dragged around, or when there are nested machines.
  Widget* parent_machine = root_machine.get();

  auto pos_dir = arg.Start(*from_widget, *parent_machine);

  auto transform_from_to_machine = TransformBetween(*from_widget, *parent_machine);
  from_shape.transform(transform_from_to_machine);

  bool using_layer = false;
  auto alpha = (1.f - from_animation_state.transparency) * (1.f - transparency);

  if (alpha < 1.0f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, alpha);
  }

  if (state) {
    if (alpha > 0.01f) {
      DrawOpticalConnector(canvas, *state, arg.Icon());
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
      if (cable_width > 0.01_mm && to) {
        if (alpha > 0.01f) {
          auto arcline = RouteCable(pos_dir, to_points, &canvas);
          auto color = SkColorSetA(arg.tint, 255 * cable_width.value / 2_mm);
          auto color_filter = color::MakeTintFilter(color, 30);
          auto path = arcline.ToPath(false);
          DrawCable(canvas, path, color_filter, CableTexture::Smooth, cable_width, cable_width);
        }
      }
    }
  }
  if (using_layer) {
    canvas.restore();
  }
}

std::unique_ptr<Action> ConnectionWidget::FindAction(Pointer& pointer, ActionTrigger trigger) {
  if (trigger == PointerButton::Left) {
    return std::make_unique<DragConnectionAction>(pointer, *this);
  }
  return nullptr;
}

bool CanConnect(Location& from, Location& to, Argument& arg) {
  std::string error;
  arg.CheckRequirements(from, &to, to.object.get(), error);
  return error.empty();
}

DragConnectionAction::DragConnectionAction(Pointer& pointer, ConnectionWidget& widget)
    : Action(pointer),
      widget(widget),
      effect(audio::MakeBeginLoopEndEffect(embedded::assets_SFX_cable_start_wav,
                                           embedded::assets_SFX_cable_loop_wav,
                                           embedded::assets_SFX_cable_end_wav)) {
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

  if (Machine* m = widget.from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      if (CanConnect(widget.from, *l, widget.arg)) {
        l->GetAnimationState().highlight_target = 1;
      } else {
        l->GetAnimationState().highlight_target = 0;
      }
      l->WakeAnimation();
    }
  }
  widget.WakeAnimation();
}

DragConnectionAction::~DragConnectionAction() {
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
  widget.WakeAnimation();

  widget.manual_position.reset();
  if (Machine* m = widget.from.ParentAs<Machine>()) {
    for (auto& l : m->locations) {
      l->animation_state.highlight_target = 0;
      l->WakeAnimation();
    }
  }
}

void DragConnectionAction::Update() {
  Vec2 new_position = pointer.PositionWithin(*widget.from.ParentAs<Machine>());
  widget.manual_position = new_position - grab_offset * widget.state->connector_scale;
  widget.WakeAnimation();
}

maf::Optional<Rect> ConnectionWidget::TextureBounds() const {
  if (transparency >= 0.99f) {
    return std::nullopt;
  }
  if (state) {
    Rect bounds = Shape().getBounds();
    float w = state->cable_width / 2 +
              0.5_mm;  // add 0.5mm to account for cable stiffener width (1mm wider than cable)
    for (auto& section : state->sections) {
      bounds.ExpandToInclude(section.pos + Vec2{w, w});
      bounds.ExpandToInclude(section.pos - Vec2{w, w});
    }
    return bounds;
  } else {
    auto pos_dir = arg.Start(*from.WidgetForObject(), *root_machine);
    Vec<Vec2AndDir> to_points;  // machine coords
    if (auto to = arg.FindLocation(from)) {
      auto to_widget = to->WidgetForObject();
      to_widget->ConnectionPositions(to_points);
      SkMatrix m = TransformBetween(*to_widget, *root_machine);
      for (auto& to_point : to_points) {
        to_point.pos = m.mapPoint(to_point.pos);
      }
    }
    ArcLine arcline = RouteCable(pos_dir, to_points);
    Rect rect = arcline.Bounds();
    return rect.Outset(cable_width / 2);
  }
}

Vec<Vec2> ConnectionWidget::TextureAnchors() const {
  Vec<Vec2> anchors;
  auto pos_dir = arg.Start(*from.WidgetForObject(), *root_machine);
  anchors.push_back(pos_dir.pos);
  Optional<Vec2> end_pos;
  if (manual_position.has_value()) {
    end_pos = *manual_position;
  } else if (auto to = arg.FindLocation(from)) {
    Vec<Vec2AndDir> to_points;  // machine coords
    auto to_widget = to->WidgetForObject();
    to_widget->ConnectionPositions(to_points);
    SkMatrix m = TransformBetween(*to_widget, *root_machine);
    for (auto& to_point : to_points) {
      to_point.pos = m.mapPoint(to_point.pos);
    }
    end_pos = to_points.front().pos;
  }
  if (end_pos) {
    anchors.push_back(*end_pos);
  }
  return anchors;
}

ConnectionWidget* ConnectionWidget::Find(Location& here, Argument& arg) {
  for (auto& connection_widget : root_widget->connection_widgets) {
    if (&connection_widget->from != &here) {
      continue;
    }
    if (&connection_widget->arg != &arg) {
      continue;
    }
    return connection_widget.get();
  }
  return nullptr;
}

}  // namespace automat::gui
