// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "ui_connection_widget.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkRRect.h>
#include <include/core/SkRSXform.h>
#include <include/core/SkTextBlob.h>
#include <include/core/SkTileMode.h>
#include <include/core/SkVertices.h>
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
#include "object_iconified.hh"
#include "root_widget.hh"
#include "widget.hh"

using namespace automat;

namespace automat::ui {

// Helper to get the Location from start_weak
Location* ConnectionWidget::StartLocation() const {
  if (auto locked = start_weak.Lock()) {
    if (auto* obj = locked.Owner<Object>()) {
      return obj->MyLocation();
    }
  }
  return nullptr;
}

Location* ConnectionWidget::EndLocation() const {
  if (auto locked = start_weak.Lock()) {
    if (auto* arg = locked.Get()) {
      if (auto* start_obj = locked.Owner<Object>()) {
        if (auto found = arg->Find(*start_obj)) {
          if (auto* obj = found.Owner<Object>()) {
            return obj->MyLocation();
          }
        }
      }
    }
  }
  return nullptr;
}

ConnectionWidget::ConnectionWidget(Widget* parent, Object& start, Argument& arg)
    : Toy(parent, start, arg), start_weak(start.AcquireWeakPtr(), &arg) {}

SkPath ConnectionWidget::Shape() const {
  if (state && transparency < 0.99f) {
    return state->Shape();
  } else {
    return SkPath();
  }
}

void ConnectionWidget::PreDraw(SkCanvas& canvas) const {
  auto arg = start_weak.Lock();
  if (!arg) return;
  auto& object = *arg.Owner<Object>();
  Location* from_ptr = object.MyLocation();
  if (!from_ptr) return;
  Location& from = *from_ptr;

  if (style == Argument::Style::Spotlight) {
    auto target_bounds = from.ToyForObject().CoarseBounds();
    Vec2 target = from.position;  //  + target_bounds.Center();
    float radius = target_bounds.rect.Hypotenuse() / 2;

    {  // Circle around the target
      SkPaint circle_paint;
      SkColor colors[] = {
          "#ffffff"_color,
          "#ffffbe00"_color,
      };
      float pos[] = {0.5, 1};
      circle_paint.setShader(
          SkGradientShader::MakeRadial(target, radius, colors, pos, 2, SkTileMode::kClamp));
      canvas.drawCircle(target, radius, circle_paint);
    }

    {  // Ray from the source to the target
      auto source_object = arg->ObjectOrNull(object);
      if (source_object) {
        Vec2 source = source_object->MyLocation()->position;
        Vec2 diff = target - source;
        float dist = Length(diff);
        auto angle = SinCos::FromVec2(diff, dist);
        SkPath path;
        path.moveTo(source);
        path.lineTo(target + Vec2::Polar((angle + 90_deg), radius));
        path.lineTo(target + Vec2::Polar((angle - 90_deg), radius));
        SkColor ray_colors[] = {"#ffffbe"_color, "#ffffbe00"_color};
        Vec2 ray_positions[] = {source, target};
        SkPaint ray_paint;
        ray_paint.setShader(SkGradientShader::MakeLinear(&ray_positions[0].sk, ray_colors, 0, 2,
                                                         SkTileMode::kClamp));
        ray_paint.setMaskFilter(SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle, 1_mm));
        canvas.drawPath(path, ray_paint);
      }
    }

    return;
  }
  auto anim = &animation_state;
  if (anim->radar_alpha >= 0.01f) {
    SkPaint radius_paint;
    SkColor tint = arg->Tint();
    SkColor colors[] = {SkColorSetA(tint, 0), SkColorSetA(tint, (int)(anim->radar_alpha * 96)),
                        SK_ColorTRANSPARENT};
    float pos[] = {0, 1, 1};
    constexpr float kPeriod = 2.f;
    double t = anim->time_seconds;
    auto local_matrix = SkMatrix::RotateRad(fmod(t * 2 * M_PI / kPeriod, 2 * M_PI))
                            .postTranslate(pos_dir.pos.x, pos_dir.pos.y);
    radius_paint.setShader(SkGradientShader::MakeSweep(0, 0, colors, pos, 3, SkTileMode::kClamp, 0,
                                                       60, 0, &local_matrix));
    // TODO: switch to drawArc instead
    float autoconnect_radius = arg->AutoconnectRadius();
    SkRect oval = Rect::MakeCenter(pos_dir.pos, autoconnect_radius * 2, autoconnect_radius * 2);

    float crt_width =
        animation::SinInterp(anim->radar_alpha, 0.2f, 0.1f, 0.5f, 1.f) * autoconnect_radius * 2;
    float crt_height =
        animation::SinInterp(anim->radar_alpha, 0.4f, 0.1f, 0.8f, 1.f) * autoconnect_radius * 2;
    SkRect crt_oval = Rect::MakeCenter(pos_dir.pos, crt_width, crt_height);
    canvas.drawArc(crt_oval, 0, 360, true, radius_paint);

    SkPaint stroke_paint;
    stroke_paint.setColor(SkColorSetA(tint, (int)(anim->radar_alpha * 128)));
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
    auto name = arg->Name();
    SkRSXform transforms[name.size()];
    for (size_t i = 0; i < name.size(); ++i) {
      float i_fract = (i + 1.f) / (name.size() + 1.f);
      // float i_fract = i / (float)arg.name.size();
      float letter_a = (i_fract - 0.5f) * kQuadrantSweep / 180 / 2 * radar_alpha_sin * kPi +
                       quadrant_offset / 180 * kPi;

      float x = sin(letter_a) * autoconnect_radius * radar_alpha_sin;
      float y = cos(letter_a) * autoconnect_radius * radar_alpha_sin;
      float w = font.sk_font.measureText(name.data() + i, 1, SkTextEncoding::kUTF8, nullptr);

      transforms[i] = SkRSXform::MakeFromRadians(font.font_scale, -letter_a, x, y, w / 2, 0);
    }
    auto transforms_span = SkSpan<SkRSXform>(transforms, name.size());
    auto text_blob =
        SkTextBlob::MakeFromRSXform(name.data(), name.size(), transforms_span, font.sk_font);
    SkPaint text_paint;
    float text_alpha = animation::SinInterp(anim->radar_alpha, 0.5f, 0.0f, 1.f, 1.f);
    text_paint.setColor(SkColorSetA(tint, (int)(text_alpha * 255)));

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

    auto* mw = ToyStore().FindOrNull(*root_machine);
    if (mw) {
      mw->NearbyCandidates(
          from, *arg, autoconnect_radius * 2 + 10_cm,
          [&](Object::Toy& candidate_toy, Atom&, Vec<Vec2AndDir>& to_points) {
            auto m = TransformBetween(candidate_toy, *mw);
            for (auto& to : to_points) {
              to.pos = m.mapPoint(to.pos);
            }
            auto arcline = RouteCable(pos_dir, to_points, &canvas);
            auto it = ArcLine::Iterator(arcline);
            float total_length = it.AdvanceToEnd() * anim->radar_alpha;
            Vec2 end_point = it.Position();
            float relative_dist = Length(pos_dir.pos - to_points[0].pos) / autoconnect_radius;
            auto path = arcline.ToPath(false, std::lerp(total_length, 0, relative_dist - 1));
            canvas.drawPath(path, stroke_paint);
            canvas.drawCircle(end_point, 1_mm, stroke_paint);
          });
    }
  }
  if (anim->prototype_alpha >= 0.01f && prototype_widget) {
    auto proto_shape = prototype_widget->Shape();
    Rect proto_bounds = proto_shape.getBounds();
    canvas.save();
    Vec2 prototype_position = PositionAhead(from, *arg, *prototype_widget);
    canvas.translate(prototype_position.x, prototype_position.y);
    canvas.saveLayerAlphaf(&proto_bounds.sk, anim->prototype_alpha * 0.4f);
    prototype_widget->Draw(canvas);
    canvas.restore();
    canvas.restore();
  }
}

void ConnectionWidget::FromMoved() {
  auto arg = start_weak.Lock();
  if (!arg) return;

  if (state) {
    if (state->stabilized && !state->stabilized_end.has_value()) {
      auto& object = *arg.Owner<Object>();
      auto& toy = *ToyStore().FindOrNull(object);
      auto* mw = ToyStore().FindOrNull(*root_machine);
      auto pos_dir = toy.ArgStart(*arg, mw);
      state->stabilized_start = pos_dir.pos;
      state->sections.front().pos = pos_dir.pos;
      state->sections.back().pos = pos_dir.pos;
      return;
    }
    state->stabilized = false;
  }
  WakeAnimation();
}

// Helper for methods of ConnectionWidget that need to access the start/end of the connection.
// It performs the locking of weak pointers and locates the widgets of connected objects.
struct ConnectionWidgetLocker {
  ToyStore& toy_store;
  MachineWidget* machine_widget;

  NestedPtr<Argument> start_arg;
  Object::Toy* start_widget;

  NestedPtr<Atom> end_atom;
  Object::Toy* end_widget;
  SkMatrix end_transform;

  // Computing everything in initializer avoids zero-initialization
  ConnectionWidgetLocker(ConnectionWidget& w)
      : toy_store(w.ToyStore()),
        machine_widget(toy_store.FindOrNull(*root_machine)),
        start_arg(w.start_weak.Lock()),
        start_widget(StartObj() ? toy_store.FindOrNull(*StartObj()) : nullptr),
        end_atom(StartObj() ? start_arg->Find(*StartObj()) : NestedPtr<Atom>()),
        end_widget(EndObj() ? toy_store.FindOrNull(*EndObj()) : nullptr),
        end_transform(end_widget && machine_widget ? TransformBetween(*end_widget, *machine_widget)
                                                   : SkMatrix()) {}

  Object* StartObj() const { return start_arg ? start_arg.Owner<Object>() : nullptr; }
  Object* EndObj() const { return end_atom ? end_atom.Owner<Object>() : nullptr; }
};

// Updates ConnectionWidget.pos_dir & ConnectionWidget.to_points. This is shared among Tick &
// TextureAnchors. Tick uses it for connection animation and TextureAnchors uses it to stretch
// the texture into most up-to-date position.
static void UpdateEndpoints(ConnectionWidget& w, ConnectionWidgetLocker& a) {
  w.pos_dir = a.start_widget->ArgStart(*a.start_arg, a.machine_widget);

  w.to_points.clear();

  if (a.end_atom) {
    a.end_widget->ConnectionPositions(w.to_points);
    for (auto& vec_and_dir : w.to_points) {
      vec_and_dir.pos = a.end_transform.mapPoint(vec_and_dir.pos);
    }
  } else {
    if (w.manual_position) {
      w.to_points.emplace_back(Vec2AndDir{
          .pos = *w.manual_position,
          .dir = -90_deg,
      });
    }
  }

  if (w.start_weak.GetUnsafe() == &next_arg) {
    while (w.to_points.size() > 1) {
      // from the last two, pick the one which is closer to pointing down (-pi/2)
      float delta_1 = fabs((w.to_points[w.to_points.size() - 1].dir + 90_deg).ToRadians());
      float delta_2 = fabs((w.to_points[w.to_points.size() - 2].dir + 90_deg).ToRadians());
      if (delta_1 < delta_2) {
        std::swap(w.to_points[w.to_points.size() - 1], w.to_points[w.to_points.size() - 2]);
      }
      w.to_points.pop_back();
    }
  }
}

animation::Phase ConnectionWidget::Tick(time::Timer& timer) {
  ConnectionWidgetLocker a(*this);

  if (!a.start_arg) {
    return animation::Finished;
  }
  style = a.start_arg->GetStyle();
  if (style == Argument::Style::Invisible || style == Argument::Style::Spotlight) {
    return animation::Finished;
  }

  from_shape = a.start_widget->AtomShape(a.start_arg.Get());
  if (a.machine_widget) {
    auto transform_from_to_machine = TransformBetween(*a.start_widget, *a.machine_widget);
    from_shape.transform(transform_from_to_machine);
  }

  UpdateEndpoints(*this, a);

  // Lazy initialization of cable physics state
  if (!state.has_value() && style == Argument::Style::Cable) {
    if (auto* loc = a.StartObj()->MyLocation()) {
      state.emplace(*loc, *a.start_arg, pos_dir);
    }
  }

  if (a.end_atom) {
    to_shape = a.end_widget->AtomShape(a.end_atom.Get());
    to_shape.transform(a.end_transform);
  } else {
    to_shape.reset();
  }

  // Don't draw the cable if one of the to_points is over from_shape
  bool overlapping = false;
  if (a.EndObj() != a.StartObj()) {
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

  bool should_be_hidden = overlapping;

  bool start_iconified = IsIconified(a.StartObj());
  if (start_iconified) {
    // Hide the connector if the object is iconified
    if (a.end_atom) {
      // cable is connected to something - keep it visible
    } else if (manual_position.has_value()) {
      // cable is held by the pointer - keep it visible
    } else {
      // cable is not connected and not held by the mouse
      // it can be hidden
      should_be_hidden = true;
    }
  }

  auto phase = animation::LinearApproach(should_be_hidden ? 1 : 0, timer.d, 5, transparency);

  float loc_transparency = (a.StartObj()->here && a.StartObj()->here->widget)
                               ? a.StartObj()->here->widget->transparency
                               : 0.f;
  alpha = (1.f - loc_transparency) * (1.f - transparency);

  if (!state.has_value() && style != Argument::Style::Arrow && alpha > 0.01f) {
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
    if (a.end_atom) {
      state->steel_insert_hidden.target = 1;
      // phase |= state->connector_scale.SpringTowards(
      //     to->scale, timer.d, Location::kScaleSpringPeriod, Location::kSpringHalfTime);
    } else {
      state->steel_insert_hidden.target = 0;
      // phase |= state->connector_scale.SpringTowards(
      //     from.scale, timer.d, Location::kScaleSpringPeriod, Location::kSpringHalfTime);
    }
    phase |= state->steel_insert_hidden.Tick(timer);

    phase |= SimulateCablePhysics(timer, *state, pos_dir, to_points);
  } else if (style != Argument::Style::Arrow) {
    cable_width.target = a.end_atom ? 2_mm : 0;
    cable_width.speed = 5;
    phase |= cable_width.Tick(timer);
  }

  if (a.start_arg->AutoconnectRadius() > 0) {
    auto& anim = animation_state;
    phase |= animation::LinearApproach(anim.radar_alpha_target, timer.d, 2.f, anim.radar_alpha);
    if (anim.radar_alpha >= 0.01f) {
      phase = animation::Animating;
      anim.time_seconds = timer.NowSeconds();
    }

    float prototype_alpha_target = anim.prototype_alpha_target;
    if (a.end_atom) {
      prototype_alpha_target = 0;
    }
    phase |= animation::LinearApproach(prototype_alpha_target, timer.d, 2.f, anim.prototype_alpha);
    if (anim.prototype_alpha > 0) {
      if (!prototype_widget) {
        auto proto = a.start_arg->Prototype();
        prototype_widget = proto->MakeToy(this);
      }
      phase |= prototype_widget->Tick(timer);
    }
  }
  return phase;
}

void ConnectionWidget::Draw(SkCanvas& canvas) const {
  if (style == Argument::Style::Invisible || style == Argument::Style::Spotlight) {
    return;
  }

  auto arg = start_weak.Lock();

  bool using_layer = false;

  if (alpha < 1.0f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, alpha);
  }

  if (state) {
    if (alpha > 0.01f) {
      DrawOpticalConnector(canvas, *state, arg->Icon());
    }
  } else {
    if (style == Argument::Style::Arrow) {
      if (to_shape.isEmpty()) {
        if (!to_points.empty()) {
          SkPath dummy_to_shape;
          dummy_to_shape.moveTo(to_points[0].pos);
          DrawArrow(canvas, from_shape, dummy_to_shape);
        }
      }
      if (!to_shape.isEmpty()) {
        DrawArrow(canvas, from_shape, to_shape);
      }
    } else {
      if (cable_width > 0.01_mm) {
        if (alpha > 0.01f) {
          auto arcline = RouteCable(pos_dir, to_points, &canvas);
          auto color = SkColorSetA(arg->Tint(), 255 * cable_width.value / 2_mm);
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

DragConnectionAction::DragConnectionAction(Pointer& pointer, ConnectionWidget& widget)
    : Action(pointer),
      widget(widget),
      effect(audio::MakeBeginLoopEndEffect(embedded::assets_SFX_cable_start_wav,
                                           embedded::assets_SFX_cable_loop_wav,
                                           embedded::assets_SFX_cable_end_wav)) {
  auto arg = widget.start_weak.Lock();
  auto* start = arg.Owner<Object>();

  // Disconnect existing connection
  arg->Disconnect(*start);

  grab_offset = Vec2(0, 0);
  if (widget.state) {
    // Position within parent machine
    auto pointer_pos = pointer.PositionWithinRootMachine();
    auto mat = widget.state->ConnectorMatrix();
    SkMatrix mat_inv;
    if (mat.invert(&mat_inv)) {
      grab_offset = mat_inv.mapPoint(pointer_pos);
    }
    widget.manual_position = pointer_pos - grab_offset * widget.state->connector_scale;
  }

  widget.WakeAnimation();
}

DragConnectionAction::~DragConnectionAction() {
  auto arg = widget.start_weak.Lock();
  if (!arg) return;
  auto start = arg.GetOwnerWeak().Lock().Cast<Object>();

  Vec2 pos;
  if (widget.state) {
    pos = widget.state->ConnectorMatrix().mapPoint({});
  } else if (widget.manual_position) {
    pos = *widget.manual_position;
  } else {
    return;
  }
  auto* mw = pointer.root_widget.toys.FindOrNull(*root_machine);
  if (mw) {
    mw->ConnectAtPoint(*start, *arg, pos);
  }
  widget.manual_position.reset();
  widget.WakeAnimation();
}

void DragConnectionAction::Update() {
  auto start = widget.start_weak.Lock();
  if (!start) return;
  Location* from = start.Owner<Object>()->MyLocation();
  if (!from) return;

  auto* parent_mw = pointer.root_widget.toys.FindOrNull(*from->ParentAs<Machine>());
  if (!parent_mw) return;
  Vec2 new_position = pointer.PositionWithin(*parent_mw);
  widget.manual_position = new_position - grab_offset * widget.state->connector_scale;
  widget.WakeAnimation();
  pointer.pointer_widget->WakeAnimation();
}

bool DragConnectionAction::Highlight(Object& obj, Atom& atom) const {
  return widget.start_weak.Lock()->CanConnect(obj, atom);
}

Optional<Rect> ConnectionWidget::TextureBounds() const {
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
    ArcLine arcline = RouteCable(pos_dir, to_points);
    Rect rect = arcline.Bounds();
    return rect.Outset(cable_width / 2);
  }
}

Vec<Vec2> ConnectionWidget::TextureAnchors() {
  ConnectionWidgetLocker a(*this);
  UpdateEndpoints(*this, a);
  Vec<Vec2> anchors;
  anchors.push_back(pos_dir.pos);
  Optional<Vec2> end_pos;
  if (manual_position.has_value()) {
    end_pos = *manual_position;
  } else if (!to_points.empty()) {
    ArcLine arcline = RouteCable(pos_dir, to_points);
    auto it = ArcLine::Iterator(arcline);
    it.AdvanceToEnd();
    end_pos = it.Position();  // to_points.front().pos;
  }
  if (end_pos) {
    anchors.push_back(*end_pos);
  }
  return anchors;
}

ConnectionWidget* ConnectionWidget::FindOrNull(Object& obj, Argument& arg) {
  auto arg_of = arg.Of(obj);
  return root_widget->toys.FindOrNull(arg_of);
}

}  // namespace automat::ui
