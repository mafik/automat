// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "ui_connection_widget.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkRRect.h>
#include <include/core/SkRSXform.h>
#include <include/core/SkShader.h>
#include <include/core/SkTextBlob.h>
#include <include/core/SkTileMode.h>
#include <include/core/SkVertices.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkRuntimeEffect.h>
#include <src/core/SkPathPriv.h>

#include <atomic>
#include <optional>

#include "../build/generated/embedded.hpp"
#include "animation.hpp"
#include "argument.hpp"
#include "audio.hpp"
#include "automat.hpp"
#include "base.hpp"
#include "casting.hpp"
#include "connector_optical.hpp"
#include "font.hpp"
#include "global_resources.hpp"
#include "location.hpp"
#include "math.hpp"
#include "object.hpp"
#include "object_iconified.hpp"
#include "root_widget.hpp"
#include "widget.hpp"

using namespace automat;

namespace automat::ui {

static Location* FindOnSameBoard(const automat::ArgumentToy& w, Object& obj) {
  if (auto* bw = BoardOrNull(w)) {
    if (auto board = bw->LockBoard()) {
      return board->LocationOrNull(obj);
    }
  }
  return nullptr;
}

ConnectionWidget::ConnectionWidget(Widget* parent, Object& start, Argument::Table& arg)
    : ArgumentToy(parent, start, &arg), tint(arg.tint.toSkColor()) {}

SpotlightWidget::SpotlightWidget(Widget* parent, Object& start, Argument::Table& arg)
    : ArgumentToy(parent, start, &arg) {}

InvisibleWidget::InvisibleWidget(Widget* parent, Object& start, Argument::Table& arg)
    : ArgumentToy(parent, start, &arg) {}

constexpr float kStreamBore = 4_mm;
constexpr float kStreamWall = 0.5_mm;

SkPath ConnectionWidget::Shape() const { return SkPath(); }

SkPath CableWidget::Shape() const {
  if (state && transparency < 0.99f) {
    return state->Shape();
  }
  return SkPath();
}

SkPath StreamPipeWidget::Shape() const {
  if (transparency < 0.99f) {
    SkPathBuilder builder;
    builder.addRect(Rect::MakeCenter(pos_dir.pos, kStreamBore * 2, kStreamBore * 2).sk);
    if (arcline) {
      SkPaint outline;
      outline.setStyle(SkPaint::kStroke_Style);
      outline.setStrokeWidth(cable_width + 2 * kStreamWall);
      SkPath bore_path = arcline->ToPath(false);
      builder.addPath(skpathutils::FillPathWithPaint(bore_path, outline));
    }
    return builder.detach();
  }
  return SkPath();
}

Optional<Rect> SpotlightWidget::DrawBounds() const {
  auto arg = LockBind<Argument>();
  if (!arg) return std::nullopt;
  auto* from = FindOnSameBoard(*this, *arg.object_ptr);
  if (!from || !from->widget || !from->widget->toy) return std::nullopt;
  float radius = from->widget->toy->CoarseBounds().rect.Hypotenuse() / 2;
  Rect bounds = Rect::MakeCenter(from->Position(*from->widget), radius * 2, radius * 2);
  if (auto* source = arg.ObjectOrNull()) {
    if (auto* source_loc = FindOnSameBoard(*this, *source)) {
      bounds.ExpandToInclude(source_loc->PeekPosition());
    }
  }
  return bounds;
}

void SpotlightWidget::Draw(SkCanvas& canvas) const {
  auto arg = LockBind<Argument>();
  if (!arg) return;
  auto* from_ptr = FindOnSameBoard(*this, *arg.object_ptr);
  if (!from_ptr || !from_ptr->widget || !from_ptr->widget->toy) return;
  Location& from = *from_ptr;

  auto target_bounds = from.widget->toy->CoarseBounds();
  Vec2 target = from.Position(*from.widget);
  float radius = target_bounds.rect.Hypotenuse() / 2;

  {  // Disc around the target
    SkPaint circle_paint;
    SkColor4f colors[] = {
        "#ffffff"_color4f,
        "#ffffbe00"_color4f,
    };
    float pos[] = {0.5, 1};
    circle_paint.setShader(SkShaders::RadialGradient(
        target, radius, SkGradient{SkGradient::Colors{colors, pos, SkTileMode::kClamp}, {}}));
    canvas.drawCircle(target, radius, circle_paint);
  }

  if (auto* source_loc =
          arg.ObjectOrNull() ? FindOnSameBoard(*this, *arg.ObjectOrNull()) : nullptr) {
    // Ray from the source to the target
    Vec2 source = source_loc->PeekPosition();
    Vec2 diff = target - source;
    float dist = Length(diff);
    auto angle = SinCos::FromVec2(diff, dist);
    SkPath path = SkPathBuilder()
                      .moveTo(source)
                      .lineTo(target + Vec2::Polar((angle + 90_deg), radius))
                      .lineTo(target + Vec2::Polar((angle - 90_deg), radius))
                      .detach();
    SkColor4f ray_colors[] = {"#ffffbe"_color4f, "#ffffbe00"_color4f};
    Vec2 ray_positions[] = {source, target};
    SkPaint ray_paint;
    ray_paint.setShader(SkShaders::LinearGradient(
        &ray_positions[0].sk, SkGradient{SkGradient::Colors{ray_colors, SkTileMode::kClamp}, {}}));
    ray_paint.setMaskFilter(SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle, 1_mm));
    canvas.drawPath(path, ray_paint);
  }
}

struct AutoconnectRadar : Widget {
  ArgumentToy& connection;
  float alpha = 0;
  double time_seconds = 0;
  AutoconnectRadar(ArgumentToy* parent) : Widget(parent), connection(*parent) {}
  StrView Name() const override { return "AutoconnectRadar"; }
  SkPath Shape() const override { return SkPath(); }

  Optional<Rect> DrawBounds() const override {
    auto arg = connection.LockBind<Argument>();
    if (!arg) return std::nullopt;
    float reach = arg.table->autoconnect_radius * 2 + 10_cm;
    return Rect::MakeCenter(connection.pos_dir.pos, reach * 2, reach * 2);
  }

  Tock Tick(time::Timer& timer) override {
    auto arg = connection.LockBind<Argument>();
    auto progress =
        animation::LinearApproach(arg ? connection.radar_alpha_target : 0, timer.d, 2.f, alpha);
    if (progress.settled && alpha < 0.01f) {
      MarkDead(timer.now);
      return {};
    }
    time_seconds = timer.NowSeconds();
    return Tock::Drawing;
  }

  void Draw(SkCanvas& canvas) const override {
    auto arg = connection.LockBind<Argument>();
    if (!arg) return;
    auto* from_ptr = FindOnSameBoard(connection, *arg.object_ptr);
    if (!from_ptr) return;
    Location& from = *from_ptr;
    if (alpha < 0.01f) return;
    auto& pos_dir = connection.pos_dir;

    SkPaint radius_paint;
    SkColor4f tint = arg.table->tint;
    SkColor4f colors[] = {{tint.fR, tint.fG, tint.fB, 0},
                          {tint.fR, tint.fG, tint.fB, alpha * 96 / 255.f},
                          SkColors::kTransparent};
    float pos[] = {0, 1, 1};
    constexpr float kPeriod = 2.f;
    double t = time_seconds;
    auto local_matrix = SkMatrix::RotateRad(fmod(t * 2 * M_PI / kPeriod, 2 * M_PI))
                            .postTranslate(pos_dir.pos.x, pos_dir.pos.y);
    radius_paint.setShader(SkShaders::SweepGradient(
        SkPoint::Make(0, 0), 0, 60,
        SkGradient{SkGradient::Colors{colors, pos, SkTileMode::kClamp}, {}}, &local_matrix));
    // TODO: switch to drawArc instead
    float autoconnect_radius = arg.table->autoconnect_radius;

    float crt_width = animation::SinInterp(alpha, 0.2f, 0.1f, 0.5f, 1.f) * autoconnect_radius * 2;
    float crt_height = animation::SinInterp(alpha, 0.4f, 0.1f, 0.8f, 1.f) * autoconnect_radius * 2;
    SkRect crt_oval = Rect::MakeCenter(pos_dir.pos, crt_width, crt_height);
    canvas.drawArc(crt_oval, 0, 360, true, radius_paint);

    SkPaint stroke_paint;
    stroke_paint.setColor(SkColor4f{tint.fR, tint.fG, tint.fB, alpha * 128 / 255.f});
    stroke_paint.setStyle(SkPaint::kStroke_Style);

    float radar_alpha_sin = sin((alpha - 0.5f) * M_PI) * 0.5f + 0.5f;
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
    auto name = arg.table->name;
    SkRSXform transforms[name.size()];
    for (size_t i = 0; i < name.size(); ++i) {
      float i_fract = (i + 1.f) / (name.size() + 1.f);
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
    float text_alpha = animation::SinInterp(alpha, 0.5f, 0.0f, 1.f, 1.f);
    text_paint.setColor(SkColor4f{tint.fR, tint.fG, tint.fB, text_alpha});

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

    if (auto* mw = BoardOrNull(connection)) {
      mw->NearbyCandidates(
          from, *arg.table, autoconnect_radius * 2 + 10_cm,
          [&](ObjectToy& candidate_toy, Interface::Table*, Vec<Vec2AndDir>& to_points) {
            auto m = TransformBetween(candidate_toy, *mw);
            for (auto& to : to_points) {
              to.pos = m.mapPoint(to.pos);
            }
            auto arcline = RouteCable(pos_dir, to_points, &canvas);
            auto it = ArcLine::Iterator(arcline);
            float total_length = it.AdvanceToEnd() * alpha;
            Vec2 end_point = it.Position();
            float relative_dist = Length(pos_dir.pos - to_points[0].pos) / autoconnect_radius;
            auto path = arcline.ToPath(false, std::lerp(total_length, 0, relative_dist - 1));
            canvas.drawPath(path, stroke_paint);
            canvas.drawCircle(end_point, 1_mm, stroke_paint);
          });
    }
  }
};

struct PrototypeGhost : Widget {
  ArgumentToy& connection;
  float alpha = 0;
  std::unique_ptr<ObjectToy> prototype_widget;

  PrototypeGhost(ArgumentToy* parent, Argument::Table& table)
      : Widget(parent), connection(*parent), prototype_widget(table.prototype()->MakeToy(this)) {
    layers.OrderInside(prototype_widget.get());
  }
  StrView Name() const override { return "PrototypeGhost"; }
  SkPath Shape() const override { return SkPath(); }
  Optional<Rect> DrawBounds() const override {
    const SkRect& bounds = prototype_widget->Shape().getBounds();
    if (bounds.isEmpty()) return std::nullopt;
    return bounds;
  }

  Tock Tick(time::Timer& timer) override {
    auto arg = connection.LockBind<Argument>();
    if (!arg) {
      MarkDead(timer.now);
      return {};
    }
    float target = arg.IsConnected() ? 0 : connection.prototype_alpha_target;
    auto progress = animation::LinearApproach(target, timer.d, 2.f, alpha);
    if (progress.settled && alpha < 0.01f) {
      MarkDead(timer.now);
      return {};
    }
    if (auto* from = FindOnSameBoard(connection, *arg.object_ptr)) {
      Vec2 pos = PositionAhead(*from, *arg.table, *prototype_widget);
      local_to_parent = SkM44(SkMatrix::Translate(pos.x, pos.y));
    }
    Tock tock;
    tock.drawing |= progress;
    return tock;
  }

  void Draw(SkCanvas& canvas) const override {
    Rect bounds = prototype_widget->Shape().getBounds();
    canvas.saveLayerAlphaf(&bounds.sk, alpha * 0.4f);
    BakeChildren(canvas);
    canvas.restore();
  }
};

// Locks the connection's weak pointers and finds the connected objects' widgets.
struct ConnectionWidgetLocker {
  ToyStore& toy_store;
  BoardWidget* board_widget;

  Ptr<Object> start_obj;
  Argument start_arg;
  ObjectToy* start_widget;

  NestedPtr<Interface::Table> end_iface;
  ObjectToy* end_widget;
  SkMatrix end_transform;

  // Computing everything in initializer avoids zero-initialization
  ConnectionWidgetLocker(ArgumentToy& w)
      : toy_store(w.ToyStore()),
        board_widget(BoardOrNull(w)),
        start_obj(w.LockOwner<Object>()),
        start_arg(start_obj ? w.Bind<Argument>(*start_obj) : nullptr),
        start_widget(start_obj ? toy_store.FindOrNull(*start_obj) : nullptr),
        end_iface(start_arg ? start_arg.Find() : NestedPtr<Interface::Table>()),
        end_widget(EndObj() ? toy_store.FindOrNull(*EndObj()) : nullptr),
        end_transform(end_widget && board_widget ? TransformBetween(*end_widget, *board_widget)
                                                 : SkMatrix()) {}

  Object* StartObj() const { return start_obj.Get(); }
  Object* EndObj() const { return end_iface.Owner<Object>(); }
};

// Recomputes pos_dir & to_points. Shared by Tick (animation) and TextureAnchors (texture stretch).
static void UpdateEndpoints(ConnectionWidget& w, ConnectionWidgetLocker& a) {
  if (a.start_widget && a.start_arg) {
    w.pos_dir = a.start_widget->ArgStart(*a.start_arg.table, a.board_widget);
  }

  w.to_points.clear();

  if (a.end_widget) {
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

  if (isa<NextArg::Table>(w.iface)) {
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

static Optional<Tock> TickLifecycle(ConnectionWidget& w, ConnectionWidgetLocker& a,
                                    time::Timer& timer) {
  if (!a.start_arg) {
    Tock tock;
    tock.drawing |= animation::ExponentialApproach(0, timer.d, 0.1, w.alpha);
    if (!tock.ing) {
      w.MarkDead(timer.now);
    }
    return tock;
  }
  if (a.start_widget == nullptr) {
    Tock tock;
    tock.shaping |= animation::LinearApproach(1, timer.d, 5, w.transparency);
    tock.drawing |= animation::ExponentialApproach(0, timer.d, 0.1, w.alpha);
    return tock;
  }
  return std::nullopt;
}

static void UpdateShapes(ConnectionWidget& w, ConnectionWidgetLocker& a) {
  auto* start_base_widget = a.start_widget->BaseToy();
  w.from_shape = start_base_widget->Shape();
  if (a.board_widget) {
    auto transform_from_to_board = TransformBetween(*start_base_widget, *a.board_widget);
    w.from_shape = w.from_shape.makeTransform(transform_from_to_board);
  }

  UpdateEndpoints(w, a);

  if (a.end_iface && a.end_widget) {
    w.to_shape = a.end_widget->Shape().makeTransform(a.end_transform);
  } else {
    w.to_shape.reset();
  }
}

static Tock TickVisibility(ConnectionWidget& w, ConnectionWidgetLocker& a, time::Timer& timer) {
  // Don't draw the cable if one of the to_points is over from_shape
  bool overlapping = false;
  if (a.EndObj() != a.StartObj()) {
    overlapping = w.to_shape.contains(w.pos_dir.pos.x, w.pos_dir.pos.y);
    if (!overlapping && !w.from_shape.isEmpty()) {
      for (auto& to_point : w.to_points) {
        if (w.from_shape.contains(to_point.pos.x, to_point.pos.y)) {
          overlapping = true;
          break;
        }
      }
    }
  }

  w.hidden = overlapping;

  if (a.end_iface && a.end_widget == nullptr) {
    w.hidden = true;
  }

  if (IsIconified(a.StartObj()) && !a.end_iface) {
    w.hidden = true;
  }

  if (w.manual_position.has_value()) {
    w.hidden = false;
  }

  Tock tock;
  tock.shaping |= animation::LinearApproach(w.hidden ? 1 : 0, timer.d, 5, w.transparency);

  float loc_transparency = 0.f;
  if (auto* start_loc = FindOnSameBoard(w, *a.StartObj())) {
    if (start_loc->widget) {
      loc_transparency = start_loc->widget->transparency;
    }
  }
  w.alpha = (1.f - loc_transparency) * (1.f - w.transparency);
  return tock;
}

static void RouteArcline(ConnectionWidget& w, bool stub_when_empty) {
  if (stub_when_empty && w.to_points.empty()) {
    Vec<Vec2AndDir> stub;
    stub.push_back(Vec2AndDir{.pos = w.pos_dir.pos + Vec2(0, -8_mm), .dir = -90_deg});
    w.arcline = RouteCable(w.pos_dir, stub, nullptr);
  } else {
    w.arcline = RouteCable(w.pos_dir, w.to_points, nullptr);
  }
}

static Tock TickRefusal(ConnectionWidget& w, time::Timer& timer) {
  Tock tock;
  if (!w.refusal_text.empty()) {
    if (timer.now >= w.refusal_until) {
      w.refusal_text.clear();
    }
    tock |= Tock::Drawing;
  }
  return tock;
}

static void ComputeEndAnchor(ConnectionWidget& w, ConnectionWidgetLocker& a,
                             const Optional<ArcLine>& route) {
  w.end_anchor_local.reset();
  if (a.end_widget && route) {
    ArcLine::Iterator it = *route;
    it.AdvanceToEnd();
    SkMatrix inv;
    if (a.end_transform.invert(&inv)) {
      w.end_anchor_local = inv.mapPoint(it.Position());
    }
  }
}

ui::Tock ConnectionWidget::Tick(time::Timer& timer) {
  ConnectionWidgetLocker a(*this);
  if (auto early = TickLifecycle(*this, a, timer)) {
    return *early;
  }
  UpdateShapes(*this, a);
  Tock tock = TickVisibility(*this, a, timer);
  RouteArcline(*this, false);
  tock |= TickRefusal(*this, timer);
  ComputeEndAnchor(*this, a, arcline);
  TickAutoconnectUI(timer);
  TickSplits();
  return tock;
}

ui::Tock CableWidget::Tick(time::Timer& timer) {
  ConnectionWidgetLocker a(*this);
  if (auto early = TickLifecycle(*this, a, timer)) {
    state.reset();
    return *early;
  }
  Argument arg = a.start_arg;
  UpdateShapes(*this, a);

  if (icon == nullptr) {
    icon = arg.MakeIcon(this);
    layers.OrderInside(icon.get());
    auto m = SkMatrix::RectToRect(icon->Shape().getBounds(), Rect(-4_mm, -4_mm, 4_mm, 4_mm),
                                  SkMatrix::kCenter_ScaleToFit);
    // scale is guaranteed to be the same for X & Y
    float s = 1.0f / std::max(m.getScaleX(), 1.0f);
    m.postScale(s, s, 0, 0);
    icon->local_to_parent = SkM44(m);
  }

  if (!state.has_value()) {
    state.emplace(arg, *a.start_widget, pos_dir);
  }

  Tock tock = TickVisibility(*this, a, timer);
  state->hidden = hidden;
  arcline.reset();
  tock |= TickRefusal(*this, timer);

  if (state->stabilized && !state->stabilized_end.has_value()) {
    if (state->sections.front().pos != pos_dir.pos) tock |= Tock::Shape;
    state->stabilized_start = pos_dir.pos;
    state->sections.front().pos = pos_dir.pos;
    state->sections.back().pos = pos_dir.pos;
  } else {
    state->stabilized = false;
  }

  if (a.end_widget) {
    state->steel_insert_hidden.target = 1;
  } else {
    state->steel_insert_hidden.target = 0;
  }
  tock.shaping |= state->steel_insert_hidden.Tick(timer);

  uint32_t last_activity = arg.state->last_activity.load(std::memory_order_relaxed);
  if (state->last_activity != last_activity) {
    state->lightness_pct = 100;
    state->last_activity = last_activity;
  }

  tock.shaping |= SimulateCablePhysics(timer, *state, pos_dir, to_points);

  ComputeEndAnchor(*this, a, state->arcline);
  TickAutoconnectUI(timer);
  TickSplits();
  return tock;
}

ui::Tock RoutedCableWidget::Tick(time::Timer& timer) {
  ConnectionWidgetLocker a(*this);
  if (auto early = TickLifecycle(*this, a, timer)) {
    return *early;
  }
  UpdateShapes(*this, a);
  Tock tock = TickVisibility(*this, a, timer);
  RouteArcline(*this, false);
  tock |= TickRefusal(*this, timer);

  if (alpha > 0.01f) {
    auto new_length = ArcLine::Iterator(*arcline).AdvanceToEnd();
    if (new_length > length + 2_cm) {
      alpha = 0;
      transparency = 1;
      tock |= Tock::Drawing;
    }
    length = new_length;
  }

  cable_width.target = a.EndObj() ? 2_mm : 0;
  cable_width.speed = 5;
  tock.drawing |= cable_width.Tick(timer);

  ComputeEndAnchor(*this, a, arcline);
  TickAutoconnectUI(timer);
  TickSplits();
  return tock;
}

void RoutedCableWidget::Draw(SkCanvas& canvas) const {
  bool using_layer = false;

  if (alpha < 1.0f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, alpha);
  }

  if (cable_width > 0.01_mm && alpha > 0.01f && arcline) {
    auto color = SkColorSetA(tint, 255 * cable_width.value / 2_mm);
    auto color_filter = color::MakeTintFilter(color, 30);
    auto path = arcline->ToPath(false);
    DrawCable(canvas, path, color_filter, CableTexture::Smooth, cable_width, cable_width);
  }

  if (using_layer) {
    canvas.restore();
  }
}

ui::Tock StreamPipeWidget::Tick(time::Timer& timer) {
  ConnectionWidgetLocker a(*this);
  if (auto early = TickLifecycle(*this, a, timer)) {
    return *early;
  }
  UpdateShapes(*this, a);
  Tock tock = TickVisibility(*this, a, timer);
  RouteArcline(*this, true);

  if (auto stream_arg = dyn_cast<StreamArgument>(a.start_arg)) {
    format = stream_arg.Format();
    auto stats = stream_arg.Stats();
    bytes_per_s = (float)byte_rate.Update(timer.NowSeconds(), stats.bytes);
    units_per_s = (float)unit_rate.Update(timer.NowSeconds(), stats.units);
    fill = stats.fill;
    capacity = stats.capacity;
    fill_unit = stats.fill_unit;
    if (stats.blocked != StreamBlocked::None) {
      if (stats.blocked != blocked) {
        blocked = stats.blocked;
        blocked_score = 0;
      }
      blocked_score = std::min(1.f, blocked_score + 3.f * (float)timer.d);
    } else {
      blocked_score = std::max(0.f, blocked_score - 1.5f * (float)timer.d);
    }
    bool now_shown = blocked_shown ? blocked_score > 0.1f : blocked_score > 0.85f;
    bool moving = a.end_iface && bytes_per_s > 1;
    if (moving) {
      float speed = std::min<float>(30_mm, 2_mm * log2f(1 + bytes_per_s / 1024));
      dash_phase -= speed * timer.d;
    }
    float fill_fraction = capacity ? (float)fill / capacity : 0;
    if (moving || fabsf(bytes_per_s - rate_drawn) > 0.5f ||
        fabsf(fill_fraction - fill_drawn) > 0.02f || now_shown != blocked_shown) {
      tock |= Tock::Drawing;
      rate_drawn = bytes_per_s;
      fill_drawn = fill_fraction;
      blocked_shown = now_shown;
    }
    // Keep ticking while connected so the meters keep polling the stream counters.
    if (a.end_iface) tock |= Tock::Ing;
  }

  tock |= TickRefusal(*this, timer);

  if (alpha > 0.01f) {
    auto new_length = ArcLine::Iterator(*arcline).AdvanceToEnd();
    if (new_length > length + 2_cm) {
      alpha = 0;
      transparency = 1;
      tock |= Tock::Drawing;
    }
    length = new_length;
  }

  cable_width.target = a.end_iface ? kStreamBore : 1.2_mm;
  cable_width.speed = 5;
  tock.shaping |= cable_width.Tick(timer);

  ComputeEndAnchor(*this, a, arcline);
  TickAutoconnectUI(timer);
  TickSplits();
  return tock;
}

ui::Tock SpotlightWidget::Tick(time::Timer& timer) {
  ConnectionWidgetLocker a(*this);
  if (!a.start_arg) {
    MarkDead(timer.now);
    return {};
  }
  if (a.start_widget) {
    pos_dir = a.start_widget->ArgStart(*a.start_arg.table, a.board_widget);
  }
  TickAutoconnectUI(timer);
  TickSplits();
  return Tock::Draw;
}

ui::Tock InvisibleWidget::Tick(time::Timer& timer) {
  if (!LockBind<Argument>()) {
    MarkDead(timer.now);
  }
  return {};
}

void ConnectionWidget::Draw(SkCanvas& canvas) const {
  bool using_layer = false;

  if (alpha < 1.0f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, alpha);
  }

  if (to_shape.isEmpty() && !to_points.empty()) {
    SkPath dummy_to_shape = SkPathBuilder().moveTo(to_points[0].pos).detach();
    DrawArrow(canvas, from_shape, dummy_to_shape);
  }
  if (!to_shape.isEmpty()) {
    DrawArrow(canvas, from_shape, to_shape);
  }

  if (using_layer) {
    canvas.restore();
  }
}

void CableWidget::Draw(SkCanvas& canvas) const {
  bool using_layer = false;

  if (alpha < 1.0f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, alpha);
  }

  if (state) {
    if (alpha > 0.01f) {
      float dispenser_scale = state->start_widget->local_to_parent.rc(0, 0);
      SkMatrix connector_matrix = state->ConnectorMatrix();

      SkPathBuilder p_builder;
      if (state->stabilized) {
        if (state->arcline) {
          SkPath p2 = state->arcline->ToPath(false);
          SkPathPriv::ReverseAddPath(&p_builder, p2);
        }
      } else {
        p_builder.moveTo(state->sections[0].pos);
        for (int i = 1; i < state->sections.size(); i++) {
          Vec2 p1 = state->sections[i - 1].pos +
                    Vec2::Polar(state->sections[i - 1].dir + state->sections[i - 1].true_dir_offset,
                                state->sections[i - 1].distance / 3);
          Vec2 p2 = state->sections[i].pos -
                    Vec2::Polar(state->sections[i].dir + state->sections[i].true_dir_offset,
                                state->sections[i].distance / 3);
          p_builder.cubicTo(p1, p2, state->sections[i].pos);
        }
      }
      SkPath p = p_builder.detach();
      p.setIsVolatile(true);

      auto color_filter = color::MakeTintFilter(state->argument.table->tint.toSkColor(), NAN);
      DrawCable(canvas, p, color_filter, CableTexture::Braided,
                state->cable_width * state->connector_scale, state->cable_width * dispenser_scale,
                &state->approx_length);

      Vec2 cable_end = state->PlugTopCenter();
      SinCos connector_dir = state->sections.front().dir + state->sections.front().true_dir_offset;

      canvas.save();
      canvas.concat(connector_matrix);

      {  // Steel insert
        canvas.save();
        canvas.translate(0, 2_mm * state->steel_insert_hidden);

        auto builder = SkVertices::Builder(SkVertices::kTriangleStrip_VertexMode, 4, 0,
                                           SkVertices::kHasTexCoords_BuilderFlag);
        SkPoint* positions = builder.positions();
        positions[0] = kSteelRect.BottomLeftCorner();
        positions[1] = kSteelRect.BottomRightCorner();
        positions[2] = kSteelRect.TopLeftCorner();
        positions[3] = kSteelRect.TopRightCorner();

        SkPoint* tex_coords = builder.texCoords();
        tex_coords[0] = SkPoint::Make(0, 0);
        tex_coords[1] = SkPoint::Make(1, 0);
        tex_coords[2] = SkPoint::Make(0, 1);
        tex_coords[3] = SkPoint::Make(1, 1);

        SkPaint paint;

        Status status;
        static auto effect =
            resources::CompileShader(embedded::assets_connector_insert_rt_sksl, status);
        if (!OK(status)) {
          FATAL << status;
        }
        paint.setShader(effect->makeShader(nullptr, nullptr, 0));
        canvas.drawVertices(builder.detach(), SkBlendMode::kScreen, paint);

        canvas.restore();
      }

      {  // Black metal casing
         // TODO: optimize by caching vertices & shader
        auto builder = SkVertices::Builder(SkVertices::kTriangleStrip_VertexMode, 4, 0,
                                           SkVertices::kHasTexCoords_BuilderFlag);
        constexpr Rect black_case_bounds =
            Rect::MakeCornerZero(kCasingWidth, kCasingHeight).MoveBy({-kCasingWidth / 2, 0});
        SkPoint* positions = builder.positions();
        positions[0] = black_case_bounds.BottomLeftCorner();
        positions[1] = black_case_bounds.BottomRightCorner();
        positions[2] = black_case_bounds.TopLeftCorner();
        positions[3] = black_case_bounds.TopRightCorner();

        SkPoint* tex_coords = builder.texCoords();
        tex_coords[0] = SkPoint::Make(0, 0);
        tex_coords[1] = SkPoint::Make(1, 0);
        tex_coords[2] = SkPoint::Make(0, 1);
        tex_coords[3] = SkPoint::Make(1, 1);

        SkPaint paint;

        Status status;
        static auto effect =
            resources::CompileShader(embedded::assets_connector_case_rt_sksl, status);
        if (!OK(status)) {
          FATAL << status;
        }
        paint.setShader(effect->makeShader(nullptr, nullptr, 0));
        paint.setColorFilter(color_filter);
        canvas.drawVertices(builder.detach(), SkBlendMode::kScreen, paint);
      }

      canvas.restore();

      if (icon) {  // Icon on the metal casing

        Vec2 icon_offset = connector_matrix.mapPoint(Vec2(0, kCasingHeight / 2));

        SkColor4f base_color = color::AdjustLightness(state->argument.table->tint, 30);
        SkColor4f bright_light = color::AdjustLightness(state->argument.table->light, 50);
        SkColor4f adjusted_color = color::AdjustLightness(base_color, state->lightness_pct);
        adjusted_color = color::MixColors(adjusted_color, bright_light, state->lightness_pct / 100);

        auto* icon_paint = PaintMixin::Get(icon.get());
        if (icon_paint) {
          SkPaint paint;
          paint.setColor(adjusted_color);
          paint.setAntiAlias(true);
          *icon_paint = paint;
        }

        canvas.save();
        canvas.translate(icon_offset.x, icon_offset.y);
        canvas.scale(state->connector_scale, state->connector_scale);

        canvas.concat(icon->local_to_parent);
        icon->Draw(canvas);

        // Draw blur
        if (state->lightness_pct > 1) {
          SkPaint glow_paint;
          glow_paint.setColor(state->argument.table->light);
          glow_paint.setAlphaf(state->lightness_pct / 100);
          float sigma = canvas.getLocalToDeviceAs3x3().mapRadius(0.5_mm);
          glow_paint.setMaskFilter(
              SkMaskFilter::MakeBlur(SkBlurStyle::kOuter_SkBlurStyle, sigma, false));
          glow_paint.setBlendMode(SkBlendMode::kScreen);
          if (icon_paint) {
            *icon_paint = glow_paint;
          }

          icon->Draw(canvas);
        }
        canvas.restore();
      }

      {  // Rubber cable holder

        float length_limit = 15_mm * state->connector_scale;
        float length = 0;

        SkPath::Iter iter(p, false);
        SkPath::Verb verb;
        Vec2 last_point = cable_end;
        Vec2 normal = Vec2::Polar(connector_dir - 90_deg, 1);
        do {
          SkPoint points[4];
          verb = iter.next(points);
          bool limit_reached = false;
          if (SkPath::kConic_Verb == verb) {
            float weight = iter.conicWeight();
            float angle = acosf(weight) * 2 * 180 / M_PI;
            int n_steps = ceil(angle * 2 / 5);
            last_point = points[0];
            for (int step = 0; step <= n_steps; step++) {
              float t = (float)step / n_steps;
              Vec2 point = Conic(points[0], points[1], points[2], weight, t);
              float delta_length = Length(point - last_point);
              if (length + delta_length >= length_limit) {
                t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
                point = Conic(points[0], points[1], points[2], weight, t);
                length = length_limit;
                limit_reached = true;
              } else {
                length += delta_length;
              }
              Vec2 tangent = -ConicTangent(points[0], points[1], points[2], weight, t);
              normal = Rotate90DegreesClockwise(tangent) / Length(tangent);
              last_point = point;
              if (limit_reached) {
                break;
              }
            }
          } else if (SkPath::kMove_Verb == verb) {
            // pass
          } else if (SkPath::kLine_Verb == verb) {
            Vec2 diff = points[1] - points[0];
            float segment_length = Length(diff);
            diff = diff / std::max(segment_length, 0.00001f);

            int n_steps = 1;
            for (int step = 0; step <= n_steps; ++step) {
              float t = (float)step / n_steps;

              float delta_length = step ? segment_length / n_steps : 0;
              if (length + delta_length >= length_limit) {
                t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
                length = length_limit;
                limit_reached = true;
              } else {
                length += delta_length;
              }

              last_point = points[0] * (1 - t) + points[1] * t;
              normal = Rotate90DegreesClockwise(diff);
              if (limit_reached) {
                break;
              }
            }
          } else if (SkPath::kCubic_Verb == verb) {
            Vec2 p0 = points[0];
            Vec2 p1 = points[1];
            Vec2 p2 = points[2];
            Vec2 p3 = points[3];
            constexpr int n_steps = 1;
            last_point = p0;
            for (int step = 1; step <= n_steps; step++) {
              float t = (float)step / n_steps;
              Vec2 point = p0 * powf(1 - t, 3) + p1 * 3 * powf(1 - t, 2) * t +
                           p2 * 3 * (1 - t) * t * t + p3 * powf(t, 3);

              float delta_length = Length(point - last_point);
              if (length + delta_length >= length_limit) {
                t = (float)(step - 1 + (length_limit - length) / delta_length) / n_steps;
                point = p0 * powf(1 - t, 3) + p1 * 3 * powf(1 - t, 2) * t +
                        p2 * 3 * (1 - t) * t * t + p3 * powf(t, 3);
                length = length_limit;
                limit_reached = true;
              } else {
                length += delta_length;
              }

              Vec2 tangent = p0 * -3 * powf(1 - t, 2) +
                             p1 * (3 * powf(1 - t, 2) - 6 * t * (1 - t)) +
                             p2 * (6 * t * (1 - t) - 3 * t * t) + p3 * 3 * powf(t, 2);
              normal = Rotate90DegreesClockwise(tangent) / Length(tangent);
              last_point = point;
              if (limit_reached) {
                break;
              }
            }
          }
        } while (SkPath::kDone_Verb != verb && length < length_limit);

        Vec2 top_offset = normal *
                          CosineInterpolate(kCasingWidth / 2, 1.5_mm, length / length_limit) *
                          state->connector_scale;
        Vec2 top_tangent = Rotate90DegreesCounterClockwise(normal);
        Vec2 base_offset =
            Vec2::Polar(connector_dir - 90_deg, kCasingWidth / 2 * state->connector_scale);
        auto top = last_point;
        auto base = cable_end;
        auto base_tangent = Vec2::Polar(connector_dir, 1);
        auto top_left = top - top_offset;
        auto top_right = top + top_offset;
        auto base_left = base - base_offset;
        auto base_right = base + base_offset;
        float vertical_control_point_distance_left = std::min(length, Length(base_left - top_left));
        float vertical_control_point_distance_right =
            std::min(length, Length(base_right - top_right));
        Vec2 positions[12] = {
            top_left,
            top_left + top_tangent * 0.5_mm,
            top_right + top_tangent * 0.5_mm,
            top_right,
            top_right - top_tangent * vertical_control_point_distance_right * 0.2,
            base_right + base_tangent * vertical_control_point_distance_right * 0.6,
            base_right,
            base + base_offset / 3,
            base - base_offset / 3,
            base_left,
            base_left + base_tangent * vertical_control_point_distance_left * 0.6,
            top_left - top_tangent * vertical_control_point_distance_left * 0.2,
        };

        SkPaint paint;
        Status status;
        static auto effect =
            resources::CompileShader(embedded::assets_connector_rubber_rt_sksl, status);
        if (!OK(status)) {
          FATAL << status;
        }
        paint.setShader(effect->makeShader(nullptr, nullptr, 0));
        paint.setColorFilter(color_filter);
        Vec2 tex_coords[4] = {
            {-1, length},
            {1, length},
            {1, 0},
            {-1, 0},
        };
        canvas.drawPatch(&positions[0].sk, nullptr, &tex_coords[0].sk, SkBlendMode::kSrcOver,
                         paint);
      }
    }
  }
  if (using_layer) {
    canvas.restore();
  }
}

void StreamPipeWidget::Draw(SkCanvas& canvas) const {
  bool using_layer = false;

  if (alpha < 1.0f) {
    using_layer = true;
    canvas.saveLayerAlphaf(nullptr, alpha);
  }

  if (cable_width > 0.2_mm && alpha > 0.01f && arcline) {
    SkPath path = arcline->ToPath(false);
    float bore = cable_width;
    SkPaint wall_paint;
    wall_paint.setStyle(SkPaint::kStroke_Style);
    wall_paint.setStrokeWidth(bore + 2 * kStreamWall);
    wall_paint.setColor(tint);
    wall_paint.setStrokeCap(SkPaint::kRound_Cap);
    wall_paint.setAntiAlias(true);
    canvas.drawPath(path, wall_paint);

    SkPaint interior_paint;
    interior_paint.setStyle(SkPaint::kStroke_Style);
    interior_paint.setStrokeWidth(bore);
    interior_paint.setColor(SkColorSetRGB(0xff, 0xfd, 0xf0));
    interior_paint.setStrokeCap(SkPaint::kRound_Cap);
    interior_paint.setAntiAlias(true);
    canvas.drawPath(path, interior_paint);

    bool connected = !to_shape.isEmpty();
    if (connected) {
      SkPaint dash_paint;
      dash_paint.setStyle(SkPaint::kStroke_Style);
      dash_paint.setStrokeWidth(bore * 0.45f);
      dash_paint.setColor(SkColorSetRGB(0x4a, 0x4a, 0x4a));
      dash_paint.setAlphaf(bytes_per_s > 1 ? 0.55f : 0.15f);
      dash_paint.setStrokeCap(SkPaint::kRound_Cap);
      dash_paint.setAntiAlias(true);
      float intervals[2] = {4_mm, 6_mm};
      dash_paint.setPathEffect(SkDashPathEffect::Make(intervals, dash_phase));
      canvas.drawPath(path, dash_paint);
    }

    bool chip_wanted = !format.empty() || bytes_per_s > 1 || capacity > 0 || blocked_shown;
    if (connected && chip_wanted) {
      SkPathMeasure measure(path, false);
      SkPoint mid_pos;
      if (measure.getPosTan(measure.getLength() / 2, &mid_pos, nullptr)) {
        auto& font = GetFont();
        const SkColor kInk = SkColorSetRGB(0x1a, 0x1a, 0x1a);
        const SkColor kInkSoft = SkColorSetRGB(0x4a, 0x4a, 0x4a);
        struct ChipLine {
          Str text;
          SkColor color;
        };
        Vec<ChipLine> lines;
        if (!format.empty()) lines.push_back({format, kInk});
        if (bytes_per_s > 1) {
          Str rate = units_per_s > 0.1f
                         ? f("{:.1f} buf/s · {}", units_per_s, FormatBytesPerSecond(bytes_per_s))
                         : FormatBytesPerSecond(bytes_per_s);
          lines.push_back({rate, kInkSoft});
        }
        int fill_line = -1;
        constexpr float kBarW = 12_mm;
        if (capacity > 0) {
          fill_line = (int)lines.size();
          Str fill_text = fill_unit.empty() ? f("{} / {}", FormatBytes(fill), FormatBytes(capacity))
                                            : f("{} / {} {}", fill, capacity, fill_unit);
          lines.push_back({fill_text, kInkSoft});
        }
        if (blocked_shown) {
          lines.push_back({blocked == StreamBlocked::Producer ? Str("backpressured producer")
                                                              : Str("starved consumer"),
                           kInk});
        }
        float line_height = kLetterSizeMM / 1000;
        float line_step = line_height * 1.5f;
        float pad = 0.8_mm;
        float text_w = 0;
        for (int i = 0; i < (int)lines.size(); ++i) {
          float w = font.MeasureText(lines[i].text);
          if (i == fill_line) w += kBarW + 1_mm;
          text_w = std::max(text_w, w);
        }
        float x0 = mid_pos.fX + bore / 2 + kStreamWall + 1.2_mm;
        Rect back(x0 - pad, mid_pos.fY - ((int)lines.size() - 1) * line_step - pad,
                  x0 + text_w + pad, mid_pos.fY + line_height + pad);
        SkPaint back_paint;
        back_paint.setColor(SkColorSetARGB(0xe6, 0xff, 0xfd, 0xf0));
        back_paint.setAntiAlias(true);
        canvas.drawRRect(RRect::MakeSimple(back, 0.8_mm).sk, back_paint);
        SkPaint back_stroke;
        back_stroke.setStyle(SkPaint::kStroke_Style);
        back_stroke.setStrokeWidth(0.15_mm);
        back_stroke.setColor(SkColorSetRGB(0x7f, 0x7f, 0x7f));
        back_stroke.setAntiAlias(true);
        canvas.drawRRect(RRect::MakeSimple(back, 0.8_mm).sk, back_stroke);
        for (int i = 0; i < (int)lines.size(); ++i) {
          float baseline = mid_pos.fY - i * line_step;
          float text_x = x0;
          if (i == fill_line) {
            Rect bar(x0, baseline, x0 + kBarW, baseline + line_height);
            float fraction = std::min(1.f, (float)fill / std::max<uint64_t>(1, capacity));
            SkPaint bar_fill;
            bar_fill.setColor(SkColorSetRGB(0x99, 0xd9, 0xea));
            bar_fill.setAntiAlias(true);
            canvas.drawRect(
                SkRect::MakeLTRB(bar.left, bar.bottom, bar.left + kBarW * fraction, bar.top),
                bar_fill);
            SkPaint bar_stroke;
            bar_stroke.setStyle(SkPaint::kStroke_Style);
            bar_stroke.setStrokeWidth(0.15_mm);
            bar_stroke.setColor(kInkSoft);
            bar_stroke.setAntiAlias(true);
            canvas.drawRect(bar.sk, bar_stroke);
            text_x += kBarW + 1_mm;
          }
          SkPaint text_paint;
          text_paint.setColor(lines[i].color);
          text_paint.setAntiAlias(true);
          canvas.save();
          canvas.translate(text_x, baseline);
          font.DrawText(canvas, lines[i].text, text_paint);
          canvas.restore();
        }
      }
    }

    if (!connected && !refusal_text.empty()) {
      float remaining = time::ToSeconds(refusal_until - time::SteadyNow());
      float fade = std::clamp(remaining, 0.f, 1.f);
      if (fade > 0) {
        auto& font = GetFont();
        const SkColor kRefusalInk = SkColorSetRGB(0xa0, 0x10, 0x10);
        Vec<Str> lines;
        for (size_t start = 0; start < refusal_text.size();) {
          size_t end = refusal_text.find('\n', start);
          if (end == Str::npos) end = refusal_text.size();
          lines.push_back(refusal_text.substr(start, end - start));
          start = end + 1;
        }
        float line_height = kLetterSizeMM / 1000;
        float line_step = line_height * 1.5f;
        float pad = 0.8_mm;
        float text_w = 0;
        for (auto& line : lines) text_w = std::max(text_w, font.MeasureText(line));
        float x0 = pos_dir.pos.x + 2_mm;
        float y0 = pos_dir.pos.y - 12_mm;
        Rect back(x0 - pad, y0 - ((int)lines.size() - 1) * line_step - pad, x0 + text_w + pad,
                  y0 + line_height + pad);
        SkPaint back_paint;
        back_paint.setColor(SkColorSetARGB((uint8_t)(0xe6 * fade), 0xff, 0xfd, 0xf0));
        back_paint.setAntiAlias(true);
        canvas.drawRRect(RRect::MakeSimple(back, 0.8_mm).sk, back_paint);
        SkPaint back_stroke;
        back_stroke.setStyle(SkPaint::kStroke_Style);
        back_stroke.setStrokeWidth(0.15_mm);
        back_stroke.setColor(SkColorSetARGB((uint8_t)(0xff * fade), 0xa0, 0x10, 0x10));
        back_stroke.setAntiAlias(true);
        canvas.drawRRect(RRect::MakeSimple(back, 0.8_mm).sk, back_stroke);
        SkPaint text_paint;
        text_paint.setColor(kRefusalInk);
        text_paint.setAlphaf(fade);
        text_paint.setAntiAlias(true);
        for (int i = 0; i < (int)lines.size(); ++i) {
          canvas.save();
          canvas.translate(x0, y0 - i * line_step);
          font.DrawText(canvas, lines[i], text_paint);
          canvas.restore();
        }
      }
    }
  }
  if (using_layer) {
    canvas.restore();
  }
}

void ConnectionWidget::ShowRefusal(Str text) {
  refusal_text = std::move(text);
  refusal_until = time::SteadyNow() + std::chrono::seconds(6);
  WakeAnimation();
}

std::unique_ptr<Action> ConnectionWidget::FindAction(Pointer& pointer, ActionTrigger trigger) {
  if (trigger == PointerButton::Left) {
    return std::make_unique<DragConnectionAction>(pointer, *this);
  }
  return nullptr;
}

DragConnectionAction::DragConnectionAction(Pointer& pointer, ConnectionWidget& connection_widget)
    : Action(pointer),
      widget(connection_widget),
      effect(audio::MakeBeginLoopEndEffect(embedded::assets_SFX_cable_start_wav,
                                           embedded::assets_SFX_cable_loop_wav,
                                           embedded::assets_SFX_cable_end_wav)) {
  if (auto arg = widget->LockBind<Argument>()) {
    arg.Disconnect();
  }

  grab_offset = Vec2(0, 0);
  if (auto* cable = dynamic_cast<CableWidget*>(widget.Get()); cable && cable->state) {
    auto* mw = BoardOrNull(*widget);
    auto pointer_pos = mw ? pointer.PositionWithin(*mw) : pointer.PositionOnCanvas();
    auto mat = cable->state->ConnectorMatrix();
    SkMatrix mat_inv;
    if (mat.invert(&mat_inv)) {
      grab_offset = mat_inv.mapPoint(pointer_pos);
    }
    widget->manual_position = pointer_pos - grab_offset * cable->state->connector_scale;
  }

  widget->WakeAnimation();
}

DragConnectionAction::~DragConnectionAction() {
  if (!widget) return;
  auto arg = widget->LockBind<Argument>();
  if (!arg) return;

  Vec2 pos;
  auto* cable = dynamic_cast<CableWidget*>(widget.Get());
  if (cable && cable->state) {
    pos = cable->state->ConnectorMatrix().mapPoint({});
  } else if (widget->manual_position) {
    pos = *widget->manual_position;
  } else {
    return;
  }
  auto* mw = BoardOrNull(*widget);
  if (mw) {
    mw->ConnectAtPoint(arg, pos);
  }
  widget->manual_position.reset();
  widget->WakeAnimation();
}

void DragConnectionAction::Update() {
  if (!widget) {
    pointer.ReplaceAction(*this, nullptr);
    return;
  }
  auto* mw = BoardOrNull(*widget);
  Vec2 new_position = mw ? pointer.PositionWithin(*mw) : pointer.PositionOnCanvas();
  auto* cable = dynamic_cast<CableWidget*>(widget.Get());
  float connector_scale = cable && cable->state ? (float)cable->state->connector_scale : 1.f;
  widget->manual_position = new_position - grab_offset * connector_scale;
  widget->WakeAnimation();
}

bool DragConnectionAction::Highlight(Interface end) const {
  if (!widget) return false;
  if (auto arg = widget->LockBind<Argument>()) {
    return arg.CanConnect(end);
  }
  return false;
}

Optional<Rect> ConnectionWidget::DrawBounds() const {
  if (transparency >= 0.99f) {
    return std::nullopt;
  }
  if (to_points.empty() && !manual_position) {
    return std::nullopt;
  }
  if (arcline) {
    Rect bounds = arcline->Bounds().Outset(cable_width / 2);
    if (bounds.sk.isEmpty()) return std::nullopt;
    return bounds;
  }
  return std::nullopt;
}

Optional<Rect> CableWidget::DrawBounds() const {
  if (transparency >= 0.99f || !state) {
    return std::nullopt;
  }
  Rect bounds = Shape().getBounds();
  float w = state->cable_width / 2 +
            0.5_mm;  // add 0.5mm to account for cable stiffener width (1mm wider than cable)
  for (auto& section : state->sections) {
    bounds.ExpandToInclude(section.pos + Vec2{w, w});
    bounds.ExpandToInclude(section.pos - Vec2{w, w});
  }
  return bounds;
}

Optional<Rect> StreamPipeWidget::DrawBounds() const {
  if (transparency >= 0.99f || !arcline) {
    return std::nullopt;
  }
  Rect bounds = arcline->Bounds().Outset(cable_width / 2 + kStreamWall + 40_mm);
  if (!refusal_text.empty()) {
    bounds.ExpandToInclude(Rect::MakeCenter(pos_dir.pos - Vec2(0, 2_cm), 24_cm, 6_cm));
  }
  return bounds;
}

Vec<Vec2> ConnectionWidget::TextureAnchors() {
  ConnectionWidgetLocker a(*this);
  UpdateEndpoints(*this, a);
  Vec<Vec2> anchors;
  anchors.push_back(pos_dir.pos);
  Optional<Vec2> end_pos;
  if (manual_position.has_value()) {
    end_pos = *manual_position;
  } else if (a.end_widget && end_anchor_local.has_value()) {
    end_pos = a.end_transform.mapPoint(*end_anchor_local);
  }
  if (end_pos) {
    anchors.push_back(*end_pos);
  }
  return anchors;
}

}  // namespace automat::ui

namespace automat {

Location* ArgumentToy::StartLocation() const {
  if (auto obj = LockOwner<Object>()) {
    return ui::FindOnSameBoard(*this, *obj);
  }
  return nullptr;
}

Location* ArgumentToy::EndLocation() const {
  if (auto arg = LockBind<Argument>()) {
    if (auto* end_obj = arg.Find().Owner<Object>()) {
      return ui::FindOnSameBoard(*this, *end_obj);
    }
  }
  return nullptr;
}

void ArgumentToy::TickSplits() {
  Location* start_loc = StartLocation();
  Location* end_loc = EndLocation();
  Vec<ui::Widget*> wanted;
  AppendObscurers(start_loc, end_loc, wanted);
  AppendObscurers(end_loc, start_loc, wanted);
  TickSplits(wanted);
}

void ArgumentToy::TickSplits(const Vec<ui::Widget*>& wanted) {
  int matching = 0;
  bool foreign = false;
  for (ui::Widget& over : splits_over) {
    if (std::find(wanted.begin(), wanted.end(), &over) != wanted.end()) {
      ++matching;
    } else {
      foreign = true;
    }
  }
  if (foreign || matching != (int)wanted.size()) {
    while (!splits_over.empty()) {
      UnsplitUnder(*splits_over.begin());
    }
    for (auto* cover : wanted) {
      SplitUnder(*cover);
    }
    if (parent) parent->WakeAnimation();
  }
}

void ArgumentToy::TickAutoconnectUI(time::Timer& timer) {
  if (radar && radar->dead) radar.reset();
  if (prototype_ghost && prototype_ghost->dead) prototype_ghost.reset();
  auto arg = LockBind<Argument>();
  if (!arg || arg.table->autoconnect_radius <= 0) {
    return;
  }
  if (!radar && radar_alpha_target > 0) {
    radar = std::make_unique<ui::AutoconnectRadar>(this);
    layers.OrderBelow(radar.get());
  }
  if (!prototype_ghost && prototype_alpha_target > 0 && !arg.IsConnected()) {
    prototype_ghost = std::make_unique<ui::PrototypeGhost>(this, *arg.table);
    layers.OrderBelow(prototype_ghost.get());
  }
  if (radar) radar->WakeAnimation(&timer);
  if (prototype_ghost) prototype_ghost->WakeAnimation(&timer);
}

}  // namespace automat
