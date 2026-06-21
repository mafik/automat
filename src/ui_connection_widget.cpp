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
#include <include/core/SkRRect.h>
#include <include/core/SkRSXform.h>
#include <include/core/SkShader.h>
#include <include/core/SkTextBlob.h>
#include <include/core/SkTileMode.h>
#include <include/core/SkVertices.h>
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

Location* ConnectionWidget::StartLocation() const {
  if (auto obj = LockOwner<Object>()) {
    return obj->MyLocation();
  }
  return nullptr;
}

Location* ConnectionWidget::EndLocation() const {
  if (auto arg = LockBind<Argument>()) {
    if (auto* end_obj = arg.Find().Owner<Object>()) {
      return end_obj->MyLocation();
    }
  }
  return nullptr;
}

ConnectionWidget::ConnectionWidget(Widget* parent, Object& start, Argument::Table& arg)
    : ArgumentToy(parent, start, &arg) {}

SkPath ConnectionWidget::Shape() const {
  if (state && transparency < 0.99f) {
    return state->Shape();
  } else {
    return SkPath();
  }
}

struct ConnectionSpotlight : Widget {
  ConnectionWidget& connection;
  ConnectionSpotlight(ConnectionWidget* parent) : Widget(parent), connection(*parent) {}
  StrView Name() const override { return "ConnectionSpotlight"; }
  SkPath Shape() const override { return SkPath(); }

  Optional<Rect> TextureBounds() const override {
    auto arg = connection.LockBind<Argument>();
    if (!arg) return std::nullopt;
    auto* from = arg.object_ptr->MyLocation();
    if (!from) return std::nullopt;
    float radius = from->ToyForObject().CoarseBounds().rect.Hypotenuse() / 2;
    Rect bounds = Rect::MakeCenter(from->position, radius * 2, radius * 2);
    if (auto* source = arg.ObjectOrNull()) {
      bounds.ExpandToInclude(source->MyLocation()->position);
    }
    return bounds;
  }

  void Draw(SkCanvas& canvas) const override {
    auto arg = connection.LockBind<Argument>();
    if (!arg) return;
    auto* from_ptr = arg.object_ptr->MyLocation();
    if (!from_ptr) return;
    Location& from = *from_ptr;

    auto target_bounds = from.ToyForObject().CoarseBounds();
    Vec2 target = from.position;
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

    if (auto* source_object = arg.ObjectOrNull()) {  // Ray from the source to the target
      Vec2 source = source_object->MyLocation()->position;
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
          &ray_positions[0].sk,
          SkGradient{SkGradient::Colors{ray_colors, SkTileMode::kClamp}, {}}));
      ray_paint.setMaskFilter(SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle, 1_mm));
      canvas.drawPath(path, ray_paint);
    }
  }
};

struct AutoconnectRadar : Widget {
  ConnectionWidget& connection;
  AutoconnectRadar(ConnectionWidget* parent) : Widget(parent), connection(*parent) {}
  StrView Name() const override { return "AutoconnectRadar"; }
  SkPath Shape() const override { return SkPath(); }

  Optional<Rect> TextureBounds() const override {
    auto arg = connection.LockBind<Argument>();
    if (!arg) return std::nullopt;
    float reach = arg.table->autoconnect_radius * 2 + 10_cm;
    return Rect::MakeCenter(connection.pos_dir.pos, reach * 2, reach * 2);
  }

  void Draw(SkCanvas& canvas) const override {
    auto arg = connection.LockBind<Argument>();
    if (!arg) return;
    auto* from_ptr = arg.object_ptr->MyLocation();
    if (!from_ptr) return;
    Location& from = *from_ptr;
    auto* anim = &connection.animation_state;
    if (anim->radar_alpha < 0.01f) return;
    auto& pos_dir = connection.pos_dir;

    SkPaint radius_paint;
    SkColor4f tint = arg.table->tint;
    SkColor4f colors[] = {{tint.fR, tint.fG, tint.fB, 0},
                          {tint.fR, tint.fG, tint.fB, anim->radar_alpha * 96 / 255.f},
                          SkColors::kTransparent};
    float pos[] = {0, 1, 1};
    constexpr float kPeriod = 2.f;
    double t = anim->time_seconds;
    auto local_matrix = SkMatrix::RotateRad(fmod(t * 2 * M_PI / kPeriod, 2 * M_PI))
                            .postTranslate(pos_dir.pos.x, pos_dir.pos.y);
    radius_paint.setShader(SkShaders::SweepGradient(
        SkPoint::Make(0, 0), 0, 60,
        SkGradient{SkGradient::Colors{colors, pos, SkTileMode::kClamp}, {}}, &local_matrix));
    // TODO: switch to drawArc instead
    float autoconnect_radius = arg.table->autoconnect_radius;

    float crt_width =
        animation::SinInterp(anim->radar_alpha, 0.2f, 0.1f, 0.5f, 1.f) * autoconnect_radius * 2;
    float crt_height =
        animation::SinInterp(anim->radar_alpha, 0.4f, 0.1f, 0.8f, 1.f) * autoconnect_radius * 2;
    SkRect crt_oval = Rect::MakeCenter(pos_dir.pos, crt_width, crt_height);
    canvas.drawArc(crt_oval, 0, 360, true, radius_paint);

    SkPaint stroke_paint;
    stroke_paint.setColor(SkColor4f{tint.fR, tint.fG, tint.fB, anim->radar_alpha * 128 / 255.f});
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
    float text_alpha = animation::SinInterp(anim->radar_alpha, 0.5f, 0.0f, 1.f, 1.f);
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
            float total_length = it.AdvanceToEnd() * anim->radar_alpha;
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
  ConnectionWidget& connection;
  std::unique_ptr<ObjectToy> prototype_widget;

  PrototypeGhost(ConnectionWidget* parent, Argument::Table& table)
      : Widget(parent), connection(*parent), prototype_widget(table.prototype()->MakeToy(this)) {
    layers.OrderInside(prototype_widget.get());
  }
  StrView Name() const override { return "PrototypeGhost"; }
  SkPath Shape() const override { return SkPath(); }
  Optional<Rect> TextureBounds() const override { return prototype_widget->Shape().getBounds(); }

  void Draw(SkCanvas& canvas) const override {
    Rect bounds = prototype_widget->Shape().getBounds();
    canvas.saveLayerAlphaf(&bounds.sk, connection.animation_state.prototype_alpha * 0.4f);
    BakeChildren(canvas);
    canvas.restore();
  }
};

// Helper for methods of ConnectionWidget that need to access the start/end of the connection.
// It performs the locking of weak pointers and locates the widgets of connected objects.
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
  ConnectionWidgetLocker(ConnectionWidget& w)
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

// Updates ConnectionWidget.pos_dir & ConnectionWidget.to_points. This is shared among Tick &
// TextureAnchors. Tick uses it for connection animation and TextureAnchors uses it to stretch
// the texture into most up-to-date position.
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

ui::Tock ConnectionWidget::Tick(time::Timer& timer) {
  ConnectionWidgetLocker a(*this);

  if (!a.start_arg) {
    Tock tock;
    tock.drawing |= animation::ExponentialApproach(0, timer.d, 0.1, alpha);
    if (!tock.ing) {
      MarkDead(timer.now);
    }
    return tock;
  }
  Argument arg = a.start_arg;

  style = arg.table->style;
  tint = arg.table->tint.toSkColor();
  if (style == Argument::Style::Spotlight) {
    if (!spotlight) {
      spotlight = std::make_unique<ConnectionSpotlight>(this);
      layers.OrderBelow(spotlight.get());
    }
    spotlight->RedrawThisFrame();
    return Tock::Draw;
  }
  if (style == Argument::Style::Invisible) {
    return Tock::Draw;
  }

  if (a.start_widget == nullptr) {
    LOG << "ConnectionWidget::Tick: start widget not found for object " << a.StartObj()->Name();
    return Tock::Draw;
  }

  auto* start_base_widget = a.start_widget->BaseToy();
  from_shape = start_base_widget->Shape();
  if (a.board_widget) {
    auto transform_from_to_board = TransformBetween(*start_base_widget, *a.board_widget);
    from_shape = from_shape.makeTransform(transform_from_to_board);
  }

  UpdateEndpoints(*this, a);

  if (icon == nullptr && a.start_arg && style == Argument::Style::Cable) {
    icon = arg.MakeIcon(this);
    layers.OrderInside(icon.get());
    auto m = SkMatrix::RectToRect(icon->Shape().getBounds(), Rect(-4_mm, -4_mm, 4_mm, 4_mm),
                                  SkMatrix::kCenter_ScaleToFit);
    // scale is guaranteed to be the same for X & Y
    float s = 1.0f / std::max(m.getScaleX(), 1.0f);
    m.postScale(s, s, 0, 0);
    icon->local_to_parent = SkM44(m);
  }

  // Lazy initialization of cable physics state
  if (!state.has_value() && style == Argument::Style::Cable) {
    state.emplace(arg, *a.start_widget, pos_dir);
  }

  if (a.end_iface) {
    to_shape = a.end_widget->Shape().makeTransform(a.end_transform);
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

  bool should_be_hidden = overlapping;

  bool start_iconified = IsIconified(a.StartObj());
  // Hide the disconnected connectors if the object is iconified
  if (start_iconified && !a.end_iface) {
    should_be_hidden = true;
  }

  if (manual_position.has_value()) {
    // cable is held by the pointer - keep it visible
    should_be_hidden = false;
  }

  if (state) {
    state->hidden = should_be_hidden;
  }

  Tock tock;
  tock.drawing |= animation::LinearApproach(should_be_hidden ? 1 : 0, timer.d, 5, transparency);

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
      tock |= Tock::Drawing;
    }
    length = new_length;
  }

  if (state) {
    if (state->stabilized && !state->stabilized_end.has_value()) {
      auto& toy = *a.start_widget;
      auto* mw = BoardOrNull(*this);

      auto pos_dir = this->pos_dir;
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
    tock.drawing |= state->steel_insert_hidden.Tick(timer);

    uint32_t last_activity = arg.state->last_activity.load(std::memory_order_relaxed);
    if (state->last_activity != last_activity) {
      state->lightness_pct = 100;
      state->last_activity = last_activity;
    }

    tock.drawing |= SimulateCablePhysics(timer, *state, pos_dir, to_points);
  } else if (style != Argument::Style::Arrow) {
    cable_width.target = a.end_iface ? 2_mm : 0;
    cable_width.speed = 5;
    tock.drawing |= cable_width.Tick(timer);
  }

  if (arg.table->autoconnect_radius > 0) {
    auto& anim = animation_state;
    auto radar_progress =
        animation::LinearApproach(anim.radar_alpha_target, timer.d, 2.f, anim.radar_alpha);
    if (anim.radar_alpha >= 0.01f) {
      if (!radar) {
        radar = std::make_unique<AutoconnectRadar>(this);
        layers.OrderBelow(radar.get());
      }
      anim.time_seconds = timer.NowSeconds();
      radar->RedrawThisFrame();
      tock |= Tock::Ing;
    } else {
      radar.reset();
      if (!radar_progress.settled) tock |= Tock::Ing;
    }

    float prototype_alpha_target = a.end_iface ? 0 : anim.prototype_alpha_target;
    auto prototype_progress =
        animation::LinearApproach(prototype_alpha_target, timer.d, 2.f, anim.prototype_alpha);
    if (anim.prototype_alpha > 0) {
      auto* ghost = static_cast<PrototypeGhost*>(prototype_ghost.get());
      if (!ghost) {
        prototype_ghost = std::make_unique<PrototypeGhost>(this, *arg.table);
        ghost = static_cast<PrototypeGhost*>(prototype_ghost.get());
        layers.OrderBelow(prototype_ghost.get());
      }
      if (auto* from = arg.object_ptr->MyLocation()) {
        Vec2 pos = PositionAhead(*from, *arg.table, *ghost->prototype_widget);
        ghost->local_to_parent = SkM44(SkMatrix::Translate(pos.x, pos.y));
      }
      if (!prototype_progress.settled) {
        prototype_ghost->RedrawThisFrame();
        tock |= Tock::Ing;
      }
    } else {
      prototype_ghost.reset();
      if (!prototype_progress.settled) tock |= Tock::Ing;
    }
  }
  return tock;
}

void ConnectionWidget::Draw(SkCanvas& canvas) const {
  if (style == Argument::Style::Invisible || style == Argument::Style::Spotlight) {
    return;
  }

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

      // Draw the cable
      auto color_filter = color::MakeTintFilter(state->argument.table->tint.toSkColor(), NAN);
      DrawCable(canvas, p, color_filter, CableTexture::Braided,
                state->cable_width * state->connector_scale, state->cable_width * dispenser_scale,
                &state->approx_length);

      Vec2 cable_end = state->PlugTopCenter();
      SinCos connector_dir = state->sections.front().dir + state->sections.front().true_dir_offset;

      canvas.save();
      canvas.concat(connector_matrix);

      constexpr float casing_left = -kCasingWidth / 2;
      constexpr float casing_right = kCasingWidth / 2;
      constexpr float casing_top = kCasingHeight;

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
  } else {
    if (style == Argument::Style::Arrow) {
      if (to_shape.isEmpty()) {
        if (!to_points.empty()) {
          SkPath dummy_to_shape = SkPathBuilder().moveTo(to_points[0].pos).detach();
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
          auto color = SkColorSetA(tint, 255 * cable_width.value / 2_mm);
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
  if (auto arg = widget.LockBind<Argument>()) {
    arg.Disconnect();  // Disconnect existing connection
  }

  grab_offset = Vec2(0, 0);
  if (widget.state) {
    // Position within parent board
    auto pointer_pos = pointer.PositionWithinRootBoard();
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
  auto arg = widget.LockBind<Argument>();
  if (!arg) return;

  Vec2 pos;
  if (widget.state) {
    pos = widget.state->ConnectorMatrix().mapPoint({});
  } else if (widget.manual_position) {
    pos = *widget.manual_position;
  } else {
    return;
  }
  auto* mw = BoardOrNull(widget);
  if (mw) {
    mw->ConnectAtPoint(arg, pos);
  }
  widget.manual_position.reset();
  widget.WakeAnimation();
}

void DragConnectionAction::Update() {
  Vec2 new_position = pointer.PositionWithinRootBoard();
  widget.manual_position = new_position - grab_offset * widget.state->connector_scale;
  widget.WakeAnimation();
}

bool DragConnectionAction::Highlight(Interface end) const {
  if (auto arg = widget.LockBind<Argument>()) {
    return arg.CanConnect(end);
  }
  return false;
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

}  // namespace automat::ui
