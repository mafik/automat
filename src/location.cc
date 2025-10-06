// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "location.hh"

#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkPoint3.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkImageFilters.h>
#include <include/pathops/SkPathOps.h>
#include <include/utils/SkShadowUtils.h>

#include <cmath>

#include "animation.hh"
#include "automat.hh"
#include "base.hh"
#include "color.hh"
#include "drag_action.hh"
#include "font.hh"
#include "format.hh"
#include "math.hh"
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

Location::Location(Widget* parent_widget, WeakPtr<Location> parent_location)
    : Widget(parent_widget), parent_location(std::move(parent_location)) {}

bool Location::HasError() {
  if (error != nullptr) return true;
  if (auto machine = ThisAs<Machine>()) {
    if (!machine->children_with_errors.empty()) return true;
  }
  return false;
}

Error* Location::GetError() {
  if (error != nullptr) return error.get();
  if (auto machine = ThisAs<Machine>()) {
    if (!machine->children_with_errors.empty())
      return (*machine->children_with_errors.begin())->GetError();
  }
  return nullptr;
}

void Location::ClearError() {
  if (error == nullptr) {
    return;
  }
  error.reset();
  if (auto machine = ParentAs<Machine>()) {
    machine->ClearChildError(*this);
  }
}

Object* Location::Follow() {
  if (object == nullptr) {
    return nullptr;
  }
  if (Pointer* ptr = object->AsPointer()) {
    return ptr->Follow(*this);
  }
  return object.get();
}

void Location::Put(Ptr<Object> obj) {
  if (object == nullptr) {
    object = std::move(obj);
    return;
  }
  if (Pointer* ptr = object->AsPointer()) {
    ptr->Put(*this, std::move(obj));
  } else {
    object = std::move(obj);
  }
}

Ptr<Object> Location::Take() {
  if (Pointer* ptr = object->AsPointer()) {
    return ptr->Take(*this);
  }
  return std::move(object);
}

Connection* Location::ConnectTo(Location& other, Argument& arg,
                                Connection::PointerBehavior pointer_behavior) {
  if (arg.precondition >= Argument::kRequiresConcreteType) {
    std::string error;
    arg.CheckRequirements(*this, &other, other.object.get(), error);
    if (error.empty()) {
      pointer_behavior = Connection::kTerminateHere;
    }
  }
  Connection* c = new Connection(arg, *this, other, pointer_behavior);
  outgoing.emplace(c);
  other.incoming.emplace(c);
  object->ConnectionAdded(*this, *c);
  return c;
}

void Location::ScheduleRun() { GetRunTask().Schedule(); }

void Location::ScheduleLocalUpdate(Location& updated) {
  (new UpdateTask(AcquirePtr<Location>(), updated.AcquirePtr<Location>()))->Schedule();
}

void Location::ScheduleErrored(Location& errored) {
  (new ErroredTask(AcquirePtr<Location>(), errored.AcquirePtr<Location>()))->Schedule();
}

SkPath Location::Shape() const {
  static SkPath empty_path = SkPath();
  return empty_path;
}

SkPath Location::FieldShape(Object& field) const {
  if (object) {
    auto object_field_shape = WidgetForObject().FieldShape(field);
    if (!object_field_shape.isEmpty()) {
      return object_field_shape;
    } else {
      return WidgetForObject().Shape();
    }
  }
  return SkPath();
}

void Location::FillChildren(Vec<Widget*>& children) {
  if (object) {
    children.push_back(&WidgetForObject());
  }
}

Optional<Rect> Location::TextureBounds() const { return nullopt; }

SkPath Outset(const SkPath& path, float distance) {
  SkRRect rrect;
  if (path.isRRect(&rrect)) {
    rrect.outset(distance, distance);
    return SkPath::RRect(rrect);
  } else {
    SkPath combined_path;
    bool simplified = Simplify(path, &combined_path);
    ArcLine arcline = ArcLine::MakeFromPath(simplified ? combined_path : path);
    arcline.Outset(distance);
    return arcline.ToPath();
  }
}

animation::Phase Location::Tick(time::Timer& timer) {
  auto phase = animation::Finished;

  auto& state = GetAnimationState();
  phase |= animation::ExponentialApproach(0, timer.d, 0.1, state.transparency);

  if (object_widget) {
    SkMatrix target_transform;
    target_transform.setScaleTranslate(scale, scale, position.x, position.y);

    object_widget->local_to_parent = SkM44(target_transform);

    /*
    Vec2 scale_curr;
    SkMatrix transform_curr = object_widget->local_to_parent.asM33();
    SkMatrix transform_curr_inv;
    (void)transform_curr.invert(&transform_curr_inv);
    LOG << "Initial State:";
    LOG << " local_to_parent=" << transform_curr;
    Vec2 target_position = position;
    LOG << " target_position=" << target_position.ToStrMetric();
    Vec2 local_pivot = {};
    */

    /*
    if (animation_state.scale_pivot) {
      LOG << "Shifting by scale pivot (" << animation_state.scale_pivot->ToStrMetric() << ")...";
      local_pivot = transform_curr_inv.mapPoint(*animation_state.scale_pivot);
      LOG << " local_pivot=" << local_pivot.ToStrMetric();
      transform_curr.preTranslate(local_pivot.x, local_pivot.y);

      LOG << " transform_curr=" << transform_curr;

      // transform_curr =
      // = local_to_parent x Translate(local_pivot) =
      // = Translate(position_curr)
      // x Translate(local_pivot)
      // x Scale(scale)

      transform_curr.postTranslate(-animation_state.scale_pivot->x,
                                   -animation_state.scale_pivot->y);
      target_position -= *animation_state.scale_pivot;
      LOG << " transform_curr=" << transform_curr;
      LOG << " target_position=" << target_position.ToStrMetric();
    }

    LOG << "Decomposing into scale and translation...";
    transform_curr.decomposeScale(&scale_curr.sk_size, &transform_curr);

    Vec2 position_curr = Vec2(transform_curr.getTranslateX(), transform_curr.getTranslateY());
    LOG << " scale_curr=" << scale_curr.ToStr();
    LOG << " transform_curr=" << transform_curr;

    auto scale_vel = Vec2(animation_state.local_to_parent_velocity.rc(0, 0),
                          animation_state.local_to_parent_velocity.rc(1, 1));

    auto position_vel = Vec2(animation_state.local_to_parent_velocity.rc(0, 3),
                             animation_state.local_to_parent_velocity.rc(1, 3));

    // LOG << "Location animation:";
    // LOG << "  target position: " << position.ToStrMetric();
    // LOG << "  curr position: " << position_curr.ToStrMetric();
    // LOG << "  position velocity: " << position_vel.ToStrMetric();
    // LOG << "  target scale: " << scale;
    // LOG << "  curr scale: " << scale_curr.ToStr();
    // LOG << "  scale velocity: " << scale_vel.ToStr();

    phase |= animation::LowLevelSineTowards(target_position.x, timer.d, kPositionSpringPeriod,
                                            position_curr.x, position_vel.x);
    phase |= animation::LowLevelSineTowards(target_position.y, timer.d, kPositionSpringPeriod,
                                            position_curr.y, position_vel.y);
    phase |= animation::LowLevelSpringTowards(scale, timer.d, kScaleSpringPeriod, kSpringHalfTime,
                                              scale_curr.x, scale_vel.x);
    phase |= animation::LowLevelSpringTowards(scale, timer.d, kScaleSpringPeriod, kSpringHalfTime,
                                              scale_curr.y, scale_vel.y);
    // LOG << "  curr position2: " << position_curr.ToStrMetric();
    // LOG << "  position velocity2: " << position_vel.ToStrMetric();
    // LOG << "  curr scale2: " << scale_curr.ToStr();
    // LOG << "  scale velocity2: " << scale_vel.ToStr();

    transform_curr.setScaleTranslate(scale_curr.x, scale_curr.y, position_curr.x, position_curr.y);

    if (animation_state.scale_pivot) {
      transform_curr.postTranslate(animation_state.scale_pivot->x, animation_state.scale_pivot->y);
    }
    object_widget->local_to_parent = SkM44(transform_curr);
    animation_state.local_to_parent_velocity.setRC(0, 0, scale_vel.x);
    animation_state.local_to_parent_velocity.setRC(1, 1, scale_vel.y);
    animation_state.local_to_parent_velocity.setRC(0, 3, position_vel.x);
    animation_state.local_to_parent_velocity.setRC(1, 3, position_vel.y);
    */
    object_widget->RecursiveTransformUpdated();
  }

  // Connection widgets rely on position, scale & transparency so make sure they're updated.
  InvalidateConnectionWidgets(true, false);

  phase |= animation::ExponentialApproach(state.highlight_target, timer.d, 0.1, state.highlight);
  if (state.highlight > 0.01f) {
    phase = animation::Animating;
    state.time_seconds = timer.NowSeconds();
  }
  {
    float target_elevation = IsDragged(*this) ? 1 : 0;
    phase |= state.elevation.SineTowards(target_elevation, timer.d, 0.2);
  }
  return phase;
}

void Location::Draw(SkCanvas& canvas) const {
  if (object == nullptr) {
    return;  // TODO: Draw a placeholder, we should support empty locations
  }
  SkPath my_shape;
  auto& object_widget = WidgetForObject();
  if (object) {
    my_shape = object_widget.Shape();
  } else {
    my_shape = Shape();
  }
  SkRect bounds = my_shape.getBounds();
  object_widget.local_to_parent.asM33().mapRect(&bounds);

  auto& state = GetAnimationState();

  if (state.highlight > 0.01f) {  // Draw dashed highlight outline
    SkPath outset_shape = Outset(my_shape, 2.5_mm * state.highlight);
    outset_shape.setIsVolatile(true);
    canvas.save();
    canvas.concat(object_widget.local_to_parent);
    static const SkPaint kHighlightPaint = [] {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(0.0005);
      paint.setColor(0xffa87347);
      return paint;
    }();
    SkPaint dash_paint(kHighlightPaint);
    dash_paint.setAlphaf(state.highlight);
    float intervals[] = {0.0035, 0.0015};
    float period_seconds = 200;
    float phase = std::fmod(state.time_seconds, period_seconds) / period_seconds;
    dash_paint.setPathEffect(SkDashPathEffect::Make(intervals, phase));
    canvas.drawPath(outset_shape, dash_paint);
    canvas.restore();
  }

  bool using_layer = false;
  if (state.transparency > 0.01) {
    using_layer = true;
    canvas.saveLayerAlphaf(&bounds, 1.f - state.transparency);
  }

  if constexpr (false) {  // Gray frame
    SkPaint frame_bg;
    SkColor frame_bg_colors[2] = {0xffcccccc, 0xffaaaaaa};
    SkPoint gradient_pts[2] = {{0, bounds.bottom()}, {0, bounds.top()}};
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

  DrawChildren(canvas);

  // Draw debug text log below the Location
  float n_lines = 1;
  float offset_y = bounds.top();
  float offset_x = bounds.left();
  float line_height = ui::kLetterSize * 1.5;
  auto& font = ui::GetFont();

  if (error) {
    constexpr float b = 0.00025;
    SkPaint error_paint;
    error_paint.setColor(SK_ColorRED);
    error_paint.setStyle(SkPaint::kStroke_Style);
    error_paint.setStrokeWidth(2 * b);
    error_paint.setAntiAlias(true);
    canvas.drawPath(my_shape, error_paint);
    offset_x -= b;
    offset_y -= 3 * b;
    error_paint.setStyle(SkPaint::kFill_Style);
    canvas.translate(offset_x, offset_y - n_lines * line_height);
    font.DrawText(canvas, error->text, error_paint);
    canvas.translate(-offset_x, -offset_y + n_lines * line_height);
    n_lines += 1;
  }

  if (using_layer) {
    canvas.restore();
  }
}

void Location::InvalidateConnectionWidgets(bool moved, bool value_changed) const {
  // We don't have backlinks to connection widgets so we have to iterate over all connection widgets
  // in root_widget and check if they're connected to this location.
  for (auto& w : ui::root_widget->connection_widgets) {
    if (&w->from == this) {  // updates all outgoing connection widgets
      if (moved && !value_changed) {
        w->FromMoved();
      } else {
        w->WakeAnimation();
        if (w->state) {
          w->state->stabilized = false;
        }
      }
    } else {
      auto [begin, end] = incoming.equal_range(&w->arg);
      for (auto it = begin; it != end; ++it) {
        auto* connection = *it;
        if (&w->from == &connection->from) {
          w->WakeAnimation();
        }
      }
    }
  }
}

std::unique_ptr<Action> Location::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  return nullptr;
}

void Location::SetNumber(double number) { SetText(f("{:g}", number)); }

std::string Location::ToStr() const { return Str(object->Name()); }

void Location::ReportMissing(std::string_view property) {
  auto error_message =
      f("Couldn't find \"%*s\". You can create a connection or rename "
        "one of the nearby objects to fix this.",
        property.size(), property.data());
  ReportError(error_message);
}

Vec2AndDir Location::ArgStart(Argument& arg) { return arg.Start(WidgetForObject(), *parent); }

ObjectAnimationState::ObjectAnimationState()
    : local_to_parent_velocity(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), elevation(0) {}
ObjectAnimationState& Location::GetAnimationState() const { return animation_state; }
Location::~Location() {
  // Location can only be destroyed by its parent so we don't have to do anything there.
  parent_location = {};
  while (not incoming.empty()) {
    delete *incoming.begin();
  }
  while (not outgoing.empty()) {
    delete *outgoing.begin();
  }
  for (auto other : update_observers) {
    other->observing_updates.erase(this);
  }
  for (auto other : observing_updates) {
    other->update_observers.erase(this);
  }
  for (auto other : error_observers) {
    other->observing_errors.erase(this);
  }
  for (auto other : observing_errors) {
    other->error_observers.erase(this);
  }
  CancelScheduledAt(*this);
  if (root_widget) {
    for (int i = 0; i < root_widget->connection_widgets.size(); ++i) {
      if (&root_widget->connection_widgets[i]->from == this) {
        root_widget->connection_widgets.erase(root_widget->connection_widgets.begin() + i);
        --i;
      }
    }
  }
}

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

Vec2 PositionAhead(const Location& origin, const Argument& arg, const ui::Widget& target_widget) {
  auto& origin_widget = origin.WidgetForObject();
  auto origin_shape = origin_widget.Shape();           // origin's local coordinates
  Vec2AndDir arg_start = origin_widget.ArgStart(arg);  // origin's local coordinates
  Vec2 drop_point;

  // Construct a matrix that transforms from the origin's local coordinates to the canvas
  // coordinates. Normally this could be done with TransformUp but that would include the animation.
  // We don't want to include the animation when placing objects around.
  SkMatrix m = SkMatrix::I();
  m.postScale(origin.scale, origin.scale);
  m.postTranslate(origin.position.x, origin.position.y);
  if (auto intersection = Raycast(origin_shape, arg_start)) {
    // Try to drop the target location so that it overlaps with the origin shape by 1mm.
    drop_point = m.mapPoint(*intersection - Vec2::Polar(arg_start.dir, 1_mm));
  } else {
    // Otherwise put it 3cm ahead of the argument start point.
    drop_point = m.mapPoint(arg_start.pos + Vec2::Polar(arg_start.dir, 3_cm));
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

  return Round((drop_point - best_connector_pos) * 1000) / 1000;
}

void PositionAhead(const Location& origin, const Argument& arg, Location& target) {
  target.position = PositionAhead(origin, arg, target.WidgetForObject());
}

void AnimateGrowFrom(Location& source, Location& grown) {
  grown.animation_state.transparency = 1;
  grown.WidgetForObject().local_to_parent =
      SkM44(source.WidgetForObject().local_to_parent).preScale(0.5, 0.5);
  grown.WakeAnimation();
}

void Location::PreDraw(SkCanvas& canvas) const {
  // Draw shadow
  if (object == nullptr) {
    return;
  }
  constexpr float kMinElevation = 1_mm;
  constexpr float kElevationRange = 8_mm;

  auto& child_widget = WidgetForObject();

  if (!child_widget.pack_frame_texture_bounds) {
    return;  // no shadow for non-cached widgets
  }
  auto& anim = GetAnimationState();
  auto& root_widget = FindRootWidget();
  auto window_size_px = root_widget.size * root_widget.display_pixels_per_meter;
  float elevation = kMinElevation + anim.elevation * kElevationRange;
  float shadow_sigma = elevation / 2;

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
  Vec2 dst[2] = {control_points[0], control_points[1] - Vec2{0, elevation}};

  SkMatrix matrix;
  if (!matrix.setPolyToPoly(&control_points[0].sk, &dst[0].sk, 2)) {
    matrix = SkMatrix::I();
  }

  SkPaint shadow_paint;
  // Simulate shadow & ambient occlusion.
  shadow_paint.setImageFilter(SkImageFilters::Merge(
      SkImageFilters::MatrixTransform(
          matrix, kFastSamplingOptions,
          SkImageFilters::DropShadowOnly(0, 0, shadow_sigma, shadow_sigma, "#09000c5b"_color,
                                         nullptr)),
      SkImageFilters::Blur(
          elevation / 10, elevation / 10,
          SkImageFilters::ColorFilter(SkColorFilters::Lighting("#c9ced6"_color, "#000000"_color),
                                      nullptr))));
  shadow_paint.setAlphaf(1.f - anim.transparency);
  canvas.saveLayer(nullptr, &shadow_paint);
  canvas.concat(child_widget.local_to_parent);
  canvas.drawDrawable(child_widget.sk_drawable.get());
  canvas.restore();
}

void Location::UpdateAutoconnectArgs() {
  if (object == nullptr) {
    return;
  }
  auto& object_widget = WidgetForObject();
  auto parent_machine = root_machine.get();
  object->Args([&](Argument& arg) {
    if (arg.autoconnect_radius <= 0) {
      return;
    }

    auto start = arg.Start(object_widget, *parent_machine);

    // Find the current distance & target of this connection
    float old_dist2 = HUGE_VALF;
    Location* old_target = nullptr;
    if (auto it = outgoing.find(&arg); it != outgoing.end()) {
      Vec<Vec2AndDir> to_positions;
      auto conn = *it;
      auto& to_object_widget = conn->to.WidgetForObject();
      to_object_widget.ConnectionPositions(to_positions);
      auto other_up = TransformBetween(to_object_widget, *parent_machine);
      for (auto& to : to_positions) {
        Vec2 to_pos = other_up.mapPoint(to.pos);
        float dist2 = LengthSquared(start.pos - to_pos);
        if (dist2 <= old_dist2) {
          old_target = &conn->to;
          old_dist2 = dist2;
        }
      }
    }

    // Find the new distance & target
    float new_dist2 = arg.autoconnect_radius * arg.autoconnect_radius;
    Location* new_target = nullptr;
    arg.NearbyCandidates(
        *this, arg.autoconnect_radius, [&](Location& other, Vec<Vec2AndDir>& to_points) {
          auto other_up = TransformBetween(other.WidgetForObject(), *parent_machine);
          for (auto& to : to_points) {
            Vec2 to_pos = other_up.mapPoint(to.pos);
            float dist2 = LengthSquared(start.pos - to_pos);
            if (dist2 <= new_dist2) {
              new_dist2 = dist2;
              new_target = &other;
            }
          }
        });

    if (new_target == old_target) {
      return;
    }
    if (old_target) {
      auto old_conn = *outgoing.find(&arg);
      delete old_conn;
    }
    if (new_target) {
      ConnectTo(*new_target, arg);
    }
  });

  // Now check other locatinos & their arguments that might want to connect to this location

  auto here_up = TransformBetween(object_widget, *parent_machine);
  Vec<Vec2AndDir> to_points;
  object_widget.ConnectionPositions(to_points);
  for (auto& to : to_points) {
    to.pos = here_up.mapPoint(to.pos);
  }

  for (auto& other : root_machine->locations) {
    if (other.get() == this) {
      continue;
    }
    auto& other_widget = other->WidgetForObject();
    auto other_up = TransformBetween(other_widget, *parent_machine);
    other->object->Args([&](Argument& arg) {
      if (arg.autoconnect_radius <= 0) {
        return;
      }
      Str error;
      arg.CheckRequirements(*other, this, object.get(), error);
      if (!error.empty()) {
        return;  // `this` location can't be connected to `other`s `arg`
      }

      // Wake the animation loop of the ConnectionWidget
      if (auto connection_widget = ConnectionWidget::Find(*other, arg)) {
        connection_widget->WakeAnimation();
      }

      auto start = other_widget.ArgStart(arg);
      start.pos = other_up.mapPoint(start.pos);

      // Find the current distance & target of this connection
      float old_dist2 = HUGE_VALF;
      Location* old_target = nullptr;
      if (auto it = other->outgoing.find(&arg); it != other->outgoing.end()) {
        Vec<Vec2AndDir> to_positions;
        auto conn = *it;
        auto& to_object_widget = conn->to.WidgetForObject();
        to_object_widget.ConnectionPositions(to_positions);
        auto to_up = TransformBetween(to_object_widget, *parent_machine);
        for (auto& to : to_positions) {
          Vec2 to_pos = to_up.mapPoint(to.pos);
          float dist2 = LengthSquared(start.pos - to_pos);
          if (dist2 <= old_dist2) {
            old_target = &conn->to;
            old_dist2 = dist2;
          }
        }
      }

      // Find the new distance & target
      float new_dist2 = std::min(arg.autoconnect_radius * arg.autoconnect_radius, old_dist2);
      Location* new_target = old_target;
      for (auto& to : to_points) {
        float dist2 = LengthSquared(start.pos - to.pos);
        if (dist2 <= new_dist2) {
          new_dist2 = dist2;
          new_target = this;
        }
      }

      if (new_target == old_target) {
        return;
      }
      if (old_target) {
        auto old_conn = *other->outgoing.find(&arg);
        delete old_conn;
      }
      if (new_target) {
        other->ConnectTo(*new_target, arg);
      }
    });
  }
}

Ptr<Object> Location::InsertHere(Ptr<Object>&& object) {
  this->object.Swap(object);
  this->object->Relocate(this);
  object_widget = nullptr;
  return object;
}
Ptr<Object> Location::Create(const Object& prototype) { return InsertHere(prototype.Clone()); }

Vec2 Location::ScalePivot() const {
  if (animation_state.scale_pivot.has_value()) {
    return animation_state.scale_pivot.value();
  }
  if (object_widget) {
    return object_widget->ScalePivot();
  }
  return Vec2();
}

RunTask& Location::GetRunTask() {
  if (run_task == nullptr) {
    run_task = make_unique<RunTask>(AcquirePtr<Location>());
    run_task->keep_alive = true;
  }
  return *run_task;
}

float Location::GetBaseScale() const {
  if (iconified && object_widget && !object_widget->CustomIconification()) {
    auto bounds = object_widget->CoarseBounds().rect;
    return std::min<float>(1_cm / bounds.Width(), 1_cm / bounds.Height());
  }
  return 1;
}

void Location::Iconify() {
  iconified = true;
  if (object_widget && object_widget->CustomIconification()) {
    object_widget->WakeAnimation();
  } else {
    scale = GetBaseScale();
    WakeAnimation();
  }
}

void Location::Deiconify() {
  iconified = false;
  if (object_widget && object_widget->CustomIconification()) {
    object_widget->WakeAnimation();
  } else {
    scale = GetBaseScale();
    WakeAnimation();
  }
}

}  // namespace automat
