// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "location.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkPoint3.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkImageFilters.h>
#include <include/utils/SkShadowUtils.h>

#include <cmath>

#include "animation.hh"
#include "automat.hh"
#include "base.hh"
#include "color.hh"
#include "drag_action.hh"
#include "embedded.hh"
#include "font.hh"
#include "format.hh"
#include "global_resources.hh"
#include "math.hh"
#include "object_iconified.hh"
#include "raycast.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "time.hh"
#include "timer_thread.hh"
#include "ui_connection_widget.hh"
#include "ui_constants.hh"
#include "widget.hh"

using namespace automat::ui;
using namespace std;

namespace automat {

constexpr float kFrameCornerRadius = 0.001;

///////////////////////////////////////////////////////////////////////////////
// Location
///////////////////////////////////////////////////////////////////////////////

Location::Location(WeakPtr<Location> parent_location)
    : parent_location(std::move(parent_location)) {}

Location::~Location() {
  parent_location = {};
  for (auto other : update_observers) {
    other->observing_updates.erase(this);
  }
  for (auto other : observing_updates) {
    other->update_observers.erase(this);
  }
  CancelScheduledAt(*this);
}

std::unique_ptr<LocationWidget> Location::MakeToy(ui::Widget* parent) {
  return std::make_unique<LocationWidget>(parent, *this);
}

Object::Toy& Location::ToyForObject() {
  if (!widget) {
    // Widget hasn't been created yet (before first render).
    // Create it eagerly through ToyStore.
    auto parent = parent_location.lock();
    ui::Widget* parent_widget = nullptr;
    if (parent) {
      // First try direct Widget cast (for future Widget-based containers).
      parent_widget = dynamic_cast<ui::Widget*>(parent->object.get());
      // If that fails, look up the object's Toy in ToyStore.
      if (!parent_widget) {
        parent_widget = ui::root_widget->toys.FindOrNull(*parent->object);
      }
    }
    ui::root_widget->toys.FindOrMake(*this, parent_widget);
    // MakeToy was called, widget is now set.
  }
  return widget->ToyForObject();
}

Object* Location::Follow() {
  if (object == nullptr) {
    return nullptr;
  }
  return object.get();
}

void Location::Put(Ptr<Object> obj) {
  if (object == nullptr) {
    object = std::move(obj);
    return;
  }
  object = std::move(obj);
}

Ptr<Object> Location::Take() { return std::move(object); }

Ptr<Object> Location::InsertHere(Ptr<Object>&& object) {
  this->object.Swap(object);
  this->object->Relocate(this);
  if (widget) widget->toy = nullptr;
  return object;
}

Ptr<Object> Location::Create(const Object& prototype) { return InsertHere(prototype.Clone()); }

void Location::SetNumber(double number) { SetText(f("{:g}", number)); }

void Location::Iconify() {
  automat::Iconify(*object);
  if (widget && widget->toy) {
    scale = widget->toy->GetBaseScale();
    widget->toy->WakeAnimation();
  }
  WakeToys();
}

void Location::Deiconify() {
  automat::Deiconify(*object);
  if (widget && widget->toy) {
    scale = widget->toy->GetBaseScale();
    widget->toy->WakeAnimation();
  }
  WakeToys();
}

SkMatrix Location::ToMatrix(Vec2 position, float scale, Vec2 anchor) {
  return SkMatrix::Translate(position.x, position.y).preScale(scale, scale, anchor.x, anchor.y);
}

void Location::FromMatrix(const SkMatrix& matrix, const Vec2& anchor, Vec2& out_position,
                          float& out_scale) {
  out_scale = matrix.getScaleX();
  auto matrix_copy = matrix;
  matrix_copy.preScale(1.f / out_scale, 1.f / out_scale, anchor.x, anchor.y);
  out_position = Vec2(matrix_copy.getTranslateX(), matrix_copy.getTranslateY());
}

///////////////////////////////////////////////////////////////////////////////
// LocationWidget
///////////////////////////////////////////////////////////////////////////////

LocationWidget::LocationWidget(ui::Widget* parent, Location& loc)
    : Toy(parent, loc, loc), elevation(0), location_weak(loc.AcquireWeakPtr()) {
  loc.widget = this;
  local_to_parent = SkM44(root_widget->CanvasToWindow());
  if (auto* obj_toy = ToyStore().FindOrNull(*loc.object)) {
    // If the object already has a toy, reparent it to keep transform
    toy = obj_toy;
    toy->Reparent(*this);
  }
}

LocationWidget::~LocationWidget() {
  if (auto loc = LockLocation()) {
    loc->widget = nullptr;
  }
}

Object::Toy& LocationWidget::ToyForObject() {
  if (!toy) {
    if (auto loc = LockLocation()) {
      if (loc->object) {
        toy = &ToyStore().FindOrMake(*loc->object, this);
        loc->scale = toy->GetBaseScale();
        toy->local_to_parent = SkM44(Location::ToMatrix(loc->position, loc->scale, LocalAnchor()));
      }
    }
  }
  return *toy;
}

Vec2 LocationWidget::LocalAnchor() const {
  if (local_anchor.has_value()) {
    return local_anchor.value();
  }
  if (toy) {
    return toy->LocalAnchor();
  }
  return Vec2();
}

SkPath LocationWidget::Shape() const {
  static SkPath empty_path = SkPath();
  return empty_path;
}

SkPath LocationWidget::ShapeRigid() const {
  auto toy_shape = toy->ShapeRigid();
  toy_shape.transform(toy->local_to_parent.asM33());
  return toy_shape;
}

void LocationWidget::FillChildren(Vec<Widget*>& children) {
  for (auto* overlay : overlays) {
    children.push_back(overlay);
  }
  auto loc = LockLocation();
  if (loc && loc->object) {
    children.push_back(&ToyForObject());
  }
}

Optional<Rect> LocationWidget::TextureBounds() const { return nullopt; }

animation::Phase LocationWidget::Tick(time::Timer& timer) {
  auto phase = animation::Finished;
  auto loc = LockLocation();
  if (!loc) return phase;

  phase |= animation::ExponentialApproach(0, timer.d, 0.1, transparency);

  ToyForObject();  // fills toy

  if (toy) {
    Vec2 local_pivot = LocalAnchor();
    Vec2 position_curr;
    float scale_curr;
    Location::FromMatrix(toy->local_to_parent.asM33(), local_pivot, position_curr, scale_curr);

    phase |= animation::LowLevelSpringTowards(loc->scale, timer.d, kScaleSpringPeriod,
                                              kSpringHalfTime, scale_curr, scale_vel);
    phase |= animation::LowLevelSineTowards(loc->position.x, timer.d, kPositionSpringPeriod,
                                            position_curr.x, position_vel.x);
    phase |= animation::LowLevelSineTowards(loc->position.y, timer.d, kPositionSpringPeriod,
                                            position_curr.y, position_vel.y);

    toy->local_to_parent = SkM44(Location::ToMatrix(position_curr, scale_curr, local_pivot));
  }

  // Connection widgets rely on position, scale & transparency so make sure they're updated.
  loc->InvalidateConnectionWidgets(true, false);

  {
    float target_elevation = IsDragged(*this) ? 1 : 0;
    phase |= elevation.SineTowards(target_elevation, timer.d, 0.2);
  }
  if (HasError(*loc->object)) {
    phase |= animation::Animating;
  }
  return phase;
}

void LocationWidget::Draw(SkCanvas& canvas) const {
  if (toy == nullptr) {
    return;  // TODO: Draw a placeholder, we should support empty locations
  }
  SkPath my_shape = toy->Shape();
  Rect bounds = Rect(my_shape.getBounds());
  toy->local_to_parent.asM33().mapRect(&bounds.sk);

  auto loc = LockLocation();
  if (!loc) return;

  bool using_layer = false;
  if (transparency > 0.01) {
    using_layer = true;
    canvas.saveLayerAlphaf(&bounds.sk, 1.f - transparency);
  }

  if constexpr (false) {  // Gray frame
    SkPaint frame_bg;
    SkColor frame_bg_colors[2] = {0xffcccccc, 0xffaaaaaa};
    SkPoint gradient_pts[2] = {{0, bounds.top}, {0, bounds.bottom}};
    sk_sp<SkShader> frame_bg_shader =
        SkGradientShader::MakeLinear(gradient_pts, frame_bg_colors, nullptr, 2, SkTileMode::kClamp);
    frame_bg.setShader(frame_bg_shader);
    canvas.drawPath(my_shape, frame_bg);

    SkPaint frame_border;
    SkColor frame_border_colors[2] = {color::AdjustLightness(frame_bg_colors[0], 5),
                                      color::AdjustLightness(frame_bg_colors[1], -5)};
    sk_sp<SkShader> frame_border_shader = SkGradientShader::MakeLinear(
        gradient_pts, frame_border_colors, nullptr, 2, SkTileMode::kClamp);
    frame_border.setShader(frame_border_shader);
    frame_border.setStyle(SkPaint::kStroke_Style);
    frame_border.setStrokeWidth(0.00025);
    canvas.drawRoundRect(bounds, kFrameCornerRadius, kFrameCornerRadius, frame_border);
  }

  // Draw debug text log below the Location
  float n_lines = 1;
  float offset_y = bounds.bottom;
  float offset_x = bounds.left;
  float line_height = ui::kLetterSize * 1.5;
  auto& font = ui::GetFont();

  DrawChildren(canvas);

  HasError(*loc->object, [&](Error& error) {
    constexpr float b = 0.00025;
    SkPaint error_paint;
    error_paint.setColor(SK_ColorRED);
    error_paint.setStyle(SkPaint::kStroke_Style);
    error_paint.setStrokeWidth(2 * b);
    error_paint.setAntiAlias(true);
    offset_x -= b;
    offset_y -= 3 * b;
    error_paint.setStyle(SkPaint::kFill_Style);
    canvas.translate(offset_x, offset_y - n_lines * line_height);
    font.DrawText(canvas, error.text, error_paint);
    canvas.translate(-offset_x, -offset_y + n_lines * line_height);
    n_lines += 1;

    auto ctm = canvas.getLocalToDeviceAs3x3().preConcat(toy->local_to_parent.asM33());
    auto my_shape_px = my_shape.makeTransform(ctm);
    Rect bounds_px = my_shape_px.getBounds();
    float blur_radius = ctm.mapRadius(7_mm);
    auto clip = bounds.Outset(blur_radius * 3);
    auto clip_px = bounds_px.Outset(blur_radius * 3);
    Status status;
    static auto shader = resources::CompileShader(embedded::assets_error_sksl, status);
    if (!OK(status)) {
      ERROR << status;
    }

    SkRuntimeEffectBuilder builder(shader);
    static auto t0 = time::SecondsSinceEpoch();
    builder.uniform("iTime") = (float)(time::SecondsSinceEpoch() - t0) * 3;
    builder.uniform("iLeft") = bounds_px.left;
    builder.uniform("iRight") = bounds_px.right;
    builder.uniform("iTop") = bounds_px.top;
    builder.uniform("iBottom") = bounds_px.bottom;

    SkPaint fire_paint;
    fire_paint.setImageFilter(SkImageFilters::RuntimeShader(builder, "iMask", nullptr));

    // Note that we're saving the canvas state twice.
    // This is because of https://issues.skia.org/issues/447458443
    // Otherwise the coordinates passed to the shader will be messed up.
    canvas.save();
    canvas.resetMatrix();
    canvas.clipRect(clip_px.sk);
    canvas.saveLayer(&clip_px.sk, &fire_paint);

    SkPaint paint;
    paint.setAntiAlias(false);

    SkPaint stroke_paint;
    stroke_paint.setAntiAlias(false);
    stroke_paint.setStyle(SkPaint::kStroke_Style);
    stroke_paint.setStrokeWidth(1);
    canvas.drawPath(my_shape_px, stroke_paint);

    paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, blur_radius, false));
    canvas.drawPath(my_shape_px, paint);

    paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, blur_radius / 4, false));
    my_shape_px.toggleInverseFillType();
    canvas.drawPath(my_shape_px, paint);

    canvas.restore();
    canvas.restore();
  });

  if (using_layer) {
    canvas.restore();
  }
}

void LocationWidget::PreDraw(SkCanvas& canvas) const {
  constexpr float kMinElevation = 1_mm;
  constexpr float kElevationRange = 8_mm;

  if (!toy->pack_frame_texture_bounds) {
    return;  // no shadow for non-cached widgets
  }
  auto& rw = FindRootWidget();
  auto window_size_px = rw.size * rw.display_pixels_per_meter;
  float elevation_mm = kMinElevation + elevation * kElevationRange;
  float shadow_sigma_mm = elevation_mm / 2;

  SkMatrix local_to_device = canvas.getLocalToDeviceAs3x3();
  SkMatrix device_to_local;
  (void)local_to_device.invert(&device_to_local);

  // Place some control points on the screen
  Vec2 control_points[2] = {
      Vec2{window_size_px.width / 2, 0},                     // top
      Vec2{window_size_px.width / 2, window_size_px.height}  // bottom
  };
  // Move them into local coordinates
  device_to_local.mapPoints(SkSpan<SkPoint>(&control_points[0].sk, 2));
  // Keep the top point in place, move the bottom point down to follow the shadow
  Vec2 dst[2] = {control_points[0], control_points[1] - Vec2{0, elevation_mm}};

  SkMatrix matrix;
  if (!matrix.setPolyToPoly(&control_points[0].sk, &dst[0].sk, 2)) {
    matrix = SkMatrix::I();
  }

  SkPaint shadow_paint;
  // Simulate shadow & ambient occlusion.
  shadow_paint.setImageFilter(SkImageFilters::Merge(
      SkImageFilters::MatrixTransform(
          matrix, kFastSamplingOptions,
          SkImageFilters::DropShadowOnly(0, 0, shadow_sigma_mm, shadow_sigma_mm, "#09000c5b"_color,
                                         nullptr)),
      SkImageFilters::Blur(
          elevation_mm / 10, elevation_mm / 10,
          SkImageFilters::ColorFilter(SkColorFilters::Lighting("#c9ced6"_color, "#000000"_color),
                                      nullptr))));
  shadow_paint.setAlphaf(1.f - transparency);
  canvas.saveLayer(nullptr, &shadow_paint);
  canvas.concat(toy->local_to_parent);
  canvas.drawDrawable(toy->sk_drawable.get());
  canvas.restore();
}

std::unique_ptr<Action> LocationWidget::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  return nullptr;
}

void Location::InvalidateConnectionWidgets(bool moved, bool value_changed) const {
  if (!ui::root_widget || !object) return;
  // Outgoing: iterate this object's args and look up each in ToyStore
  object->Args([&](Argument& arg) {
    auto arg_of = arg.Of(*object);
    auto* w = ui::root_widget->toys.FindOrNull(arg_of);
    if (!w) return;
    if (moved && !value_changed) {
      w->FromMoved();
    } else {
      w->WakeAnimation();
      if (w->state) {
        w->state->stabilized = false;
      }
    }
  });
  // Incoming: iterate all ToyStore entries to find ConnectionWidgets pointing to this location
  for (auto& [key, toy] : ui::root_widget->toys.container) {
    auto* w = dynamic_cast<ui::ConnectionWidget*>(toy.get());
    if (!w) continue;
    if (w->EndLocation() == this) {
      w->WakeAnimation();
    }
  }
}

void LocationWidget::UpdateAutoconnectArgs() {
  auto loc = LockLocation();
  if (!loc || loc->object == nullptr) {
    return;
  }
  auto& toy = ToyForObject();
  auto* parent_mw = ToyStore().FindOrNull(*root_machine);
  if (!parent_mw) return;
  loc->object->Args([&](Argument& arg) {
    float autoconnect_radius = arg.AutoconnectRadius();
    if (autoconnect_radius <= 0) {
      return;
    }

    auto start = toy.ArgStart(arg, parent_mw);

    // Find the current distance & target of this connection
    Atom* old_atom = nullptr;
    float old_dist2 = HUGE_VALF;
    if (auto end = arg.Find(*loc->object)) {
      old_atom = end.Get();
      Vec<Vec2AndDir> to_positions;
      auto* end_loc = end.Owner<Object>()->MyLocation();
      auto& end_toy = end_loc->ToyForObject();
      end_toy.ConnectionPositions(to_positions);
      auto other_up = TransformBetween(end_toy, *parent_mw);
      for (auto& to : to_positions) {
        Vec2 to_pos = other_up.mapPoint(to.pos);
        float dist2 = LengthSquared(start.pos - to_pos);
        if (dist2 <= old_dist2) {
          old_dist2 = dist2;
        }
      }
    }

    // Find the nearest compatible atom
    float new_dist2 = autoconnect_radius * autoconnect_radius;
    Object::Toy* new_toy = nullptr;
    Atom* new_atom = nullptr;
    parent_mw->NearbyCandidates(
        *loc, arg, autoconnect_radius,
        [&](Object::Toy& toy, Atom& atom, Vec<Vec2AndDir>& to_points) {
          auto other_up = TransformBetween(toy, *parent_mw);
          for (auto& to : to_points) {
            Vec2 to_pos = other_up.mapPoint(to.pos);
            float dist2 = LengthSquared(start.pos - to_pos);
            if (dist2 <= new_dist2) {
              new_dist2 = dist2;
              new_toy = &toy;
              new_atom = &atom;
            }
          }
        });

    if (new_atom == old_atom) {
      return;
    }
    if (new_toy) {
      arg.Connect(*loc->object, NestedPtr<Atom>(new_toy->owner.Lock(), new_atom));
    } else {
      arg.Disconnect(*loc->object);
    }
  });

  // Now check other locations & their arguments that might want to connect to this location

  auto here_up = TransformBetween(toy, *parent_mw);
  Vec<Vec2AndDir> to_points;
  toy.ConnectionPositions(to_points);
  for (auto& to : to_points) {
    to.pos = here_up.mapPoint(to.pos);
  }

  for (auto& other : root_machine->locations) {
    if (other.get() == loc.get()) {
      continue;
    }
    auto& other_widget = other->ToyForObject();
    auto other_up = TransformBetween(other_widget, *parent_mw);
    other->object->Args([&](Argument& arg) {
      float autoconnect_radius = arg.AutoconnectRadius();
      if (autoconnect_radius <= 0) {
        return;
      }
      Atom* this_atom = arg.CanConnect(*other->object, *loc->object);
      if (!this_atom) {
        return;  // `this` location can't be connected to `other`s `arg`
      }

      // Wake the animation loop of the ConnectionWidget
      if (auto connection_widget = ConnectionWidget::FindOrNull(*other->object, arg)) {
        connection_widget->WakeAnimation();
      }

      auto start = other_widget.ArgStart(arg);
      start.pos = other_up.mapPoint(start.pos);

      // Find the current distance & target of this connection
      Atom* old_atom = nullptr;
      float old_dist2 = HUGE_VALF;
      if (auto end = arg.Find(*other->object)) {
        old_atom = end.Get();
        Vec<Vec2AndDir> to_positions;
        auto* end_loc = end.Owner<Object>()->MyLocation();
        auto& end_toy = end_loc->ToyForObject();
        end_toy.ConnectionPositions(to_positions);
        auto to_up = TransformBetween(end_toy, *parent_mw);
        for (auto& to : to_positions) {
          Vec2 to_pos = to_up.mapPoint(to.pos);
          float dist2 = LengthSquared(start.pos - to_pos);
          if (dist2 <= old_dist2) {
            old_dist2 = dist2;
          }
        }
      }

      // Find the new distance & target
      float radius2 = autoconnect_radius * autoconnect_radius;
      bool old_is_here = (old_atom == this_atom);
      float new_dist2 = old_is_here ? radius2 : std::min(radius2, old_dist2);
      Atom* new_atom = old_is_here ? nullptr : old_atom;
      for (auto& to : to_points) {
        float dist2 = LengthSquared(start.pos - to.pos);
        if (dist2 <= new_dist2) {
          new_dist2 = dist2;
          new_atom = this_atom;
        }
      }

      if (new_atom == old_atom) {
        return;
      }
      if (new_atom) {
        arg.Connect(*other->object, NestedPtr<Atom>(loc->object->AcquirePtr(), new_atom));
      } else {
        arg.Disconnect(*other->object);
      }
    });
  }
}

///////////////////////////////////////////////////////////////////////////////
// Free functions
///////////////////////////////////////////////////////////////////////////////

void PositionBelow(Location& origin, Location& below) {
  Machine* m = origin.ParentAs<Machine>();
  Size origin_index = SIZE_MAX;
  Size below_index = SIZE_MAX;
  for (Size i = 0; i < m->locations.size(); i++) {
    if (m->locations[i].get() == &origin) {
      origin_index = i;
      if (below_index != SIZE_MAX) {
        break;
      }
    }
    if (m->locations[i].get() == &below) {
      below_index = i;
      if (origin_index != SIZE_MAX) {
        break;
      }
    }
  }
  if (origin_index > below_index) {
    std::swap(m->locations[origin_index], m->locations[below_index]);
  }
}

Vec2 PositionAhead(Location& origin, const Argument& arg, const Object::Toy& target_widget) {
  auto& origin_widget = origin.ToyForObject();
  auto origin_shape = origin_widget.Shape();           // origin's local coordinates
  Vec2AndDir arg_start = origin_widget.ArgStart(arg);  // origin's local coordinates
  Vec2 drop_point;

  // Construct a matrix that transforms from the origin's local coordinates to the canvas
  // coordinates. Normally this could be done with TransformUp but that would include the animation.
  // We don't want to include the animation when placing objects around.
  {
    SkMatrix m = Location::ToMatrix(origin.position, origin.scale, origin.widget->LocalAnchor());
    if (auto intersection = Raycast(origin_shape, arg_start)) {
      // Try to drop the target location so that it overlaps with the origin shape by 1mm.
      drop_point = m.mapPoint(*intersection - Vec2::Polar(arg_start.dir, 1_mm));
    } else {
      // Otherwise put it 3cm ahead of the argument start point.
      drop_point = m.mapPoint(arg_start.pos + Vec2::Polar(arg_start.dir, 3_cm));
    }
  }

  // Pick the position that allows the cable to come in most horizontally (left to right).
  Vec2 best_connector_pos = Vec2(0, 0);
  float best_angle_diff = 100;
  Vec<Vec2AndDir> connector_positions;
  target_widget.ConnectionPositions(connector_positions);
  for (auto& pos : connector_positions) {
    float angle_diff = (pos.dir - arg_start.dir).ToRadians();
    if (fabs(angle_diff) < fabs(best_angle_diff)) {
      best_connector_pos = pos.pos;
      best_angle_diff = angle_diff;
    }
  }
  {
    SkMatrix m =
        Location::ToMatrix(Vec2{}, target_widget.GetBaseScale(), target_widget.LocalAnchor());
    best_connector_pos = m.mapPoint(best_connector_pos);
  }

  return Round((drop_point - best_connector_pos) * 1000) / 1000;
}

void PositionAhead(Location& origin, const Argument& arg, Location& target) {
  target.position = PositionAhead(origin, arg, target.ToyForObject());
}

void AnimateGrowFrom(Location& source, Location& grown) {
  grown.ForEachToy([&](ui::RootWidget& root, Toy& grown_toy) {
    auto* source_toy = root.toys.FindOrNull(source);
    if (source_toy == nullptr) return;
    auto& source_widget = static_cast<LocationWidget&>(*source_toy);
    auto& grown_widget = static_cast<LocationWidget&>(grown_toy);
    grown_widget.transparency = 1;
    grown_widget.ToyForObject().local_to_parent =
        SkM44(source_widget.ToyForObject().local_to_parent).preScale(0.5, 0.5);
    grown_widget.WakeAnimation();
  });
}

void LocationWidget::OnReparent(ui::Widget& new_parent, SkM44& fix) {
  auto& toy = ToyForObject();
  toy.local_to_parent.postConcat(fix);
  {  // Transform velocities
    auto fix33 = fix.asM33();
    fix33.mapVector(position_vel);
    fix33.mapRadius(scale_vel);
  }
}
}  // namespace automat
