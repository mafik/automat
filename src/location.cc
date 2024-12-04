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
#include <include/pathops/SkPathOps.h>
#include <include/utils/SkShadowUtils.h>

#include <cmath>

#include "animation.hh"
#include "base.hh"
#include "color.hh"
#include "drag_action.hh"
#include "font.hh"
#include "format.hh"
#include "gui_connection_widget.hh"
#include "gui_constants.hh"
#include "math.hh"
#include "root.hh"
#include "timer_thread.hh"
#include "widget.hh"
#include "window.hh"

using namespace automat::gui;
using namespace maf;
using namespace std;

namespace automat {

constexpr float kFrameCornerRadius = 0.001;

Location::Location(std::weak_ptr<Location> parent) : parent_location(parent) {}

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
  if (Pointer* ptr = object->AsPointer()) {
    return ptr->Follow(*this);
  }
  return object.get();
}

void Location::Put(shared_ptr<Object> obj) {
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

shared_ptr<Object> Location::Take() {
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

void Location::ScheduleRun() {
  if (run_task == nullptr) {
    run_task = make_unique<RunTask>(SharedPtr<Location>());
  }
  run_task->Schedule();
}

void Location::ScheduleLocalUpdate(Location& updated) {
  (new UpdateTask(SharedPtr<Location>(), updated.SharedPtr<Location>()))->Schedule();
}

void Location::ScheduleErrored(Location& errored) {
  (new ErroredTask(SharedPtr<Location>(), errored.SharedPtr<Location>()))->Schedule();
}

SkPath Location::Shape() const {
  if constexpr (false) {  // Gray box shape
    // Keeping this around because locations will eventually be toggleable between frame & no frame
    // modes.
    SkRect object_bounds;
    if (object) {
      object_bounds = object->Shape().getBounds();
    } else {
      object_bounds = SkRect::MakeEmpty();
    }
    float outset = 0.001 - kBorderWidth / 2;
    SkRect bounds = object_bounds.makeOutset(outset, outset);
    return SkPath::RRect(bounds, kFrameCornerRadius, kFrameCornerRadius);
  }
  static SkPath empty_path = SkPath();
  return empty_path;
}

SkPath Location::FieldShape(Object& field) const {
  if (object) {
    auto object_field_shape = object->FieldShape(field);
    if (!object_field_shape.isEmpty()) {
      return object_field_shape;
    } else {
      return object->Shape();
    }
  }
  return SkPath();
}

void Location::FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) {
  if (object) {
    children.push_back(object);
  }
}

Optional<Rect> Location::TextureBounds() const { return nullopt; }

SkPath Outset(const SkPath& path, float distance) {
  SkRRect rrect;
  if (path.isRRect(&rrect)) {
    rrect.outset(distance, distance);
    return SkPath::RRect(rrect);
  } else {
    SkPaint outset_paint;
    outset_paint.setStyle(SkPaint::kStrokeAndFill_Style);
    outset_paint.setStrokeWidth(distance);
    SkPath outset_path;
    skpathutils::FillPathWithPaint(path, outset_paint, &outset_path);
    Simplify(outset_path, &outset_path);
    return outset_path;
  }
}

animation::Phase Location::Update(time::Timer& timer) {
  auto phase = animation::Finished;

  auto& state = GetAnimationState();
  if (state.Tick(timer.d, position, scale) == animation::Animating) {
    phase = animation::Animating;
    InvalidateConnectionWidgets(true, false);
  }

  phase |= animation::ExponentialApproach(state.highlight_target, timer.d, 0.1, state.highlight);
  phase |= animation::ExponentialApproach(0, timer.d, 0.1, state.transparency);
  if (state.highlight > 0.01f) {
    phase = animation::Animating;
    state.time_seconds = timer.NowSeconds();
  }
  {
    float target_elevation = 0;
    for (auto* window : windows) {
      for (auto* pointer : window->pointers) {
        if (auto& action = pointer->action) {
          if (auto* drag_action = dynamic_cast<DragLocationAction*>(action.get())) {
            if (drag_action->location.get() == this) {
              target_elevation = 1;
            }
          }
        }
      }
    }
    phase |= state.elevation.SineTowards(target_elevation, timer.d, 0.2);
  }
  return phase;
}

void Location::Draw(SkCanvas& canvas) const {
  SkPath my_shape;
  if (object) {
    my_shape = object->Shape();
  } else {
    my_shape = Shape();
  }
  SkRect bounds = my_shape.getBounds();
  auto& state = GetAnimationState();

  bool using_layer = false;
  if (state.transparency > 0.01) {
    using_layer = true;
    canvas.saveLayerAlphaf(&bounds, 1.f - state.transparency);
  }

  if (state.highlight > 0.01f) {  // Draw dashed highlight outline
    SkPath outset_shape = Outset(my_shape, 2.5_mm * state.highlight);
    outset_shape.setIsVolatile(true);
    auto from_child = TransformFromChild(*object);
    outset_shape.transform(from_child);

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
    double ignore;
    float period_seconds = 200;
    float phase = std::fmod(state.time_seconds, period_seconds) / period_seconds;
    dash_paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, phase));
    canvas.drawPath(outset_shape, dash_paint);
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
  float line_height = gui::kLetterSize * 1.5;
  auto& font = gui::GetFont();

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
  // in window and check if they're connected to this location.
  for (auto& w : gui::window->connection_widgets) {
    if (&w->from == this) {  // updates all outgoing connection widgets
      if (moved && !value_changed) {
        w->FromMoved();
      } else {
        w->InvalidateDrawCache();
        if (w->state) {
          w->state->stabilized = false;
        }
      }
    } else {
      auto [begin, end] = incoming.equal_range(&w->arg);
      for (auto it = begin; it != end; ++it) {
        auto* connection = *it;
        if (&w->from == &connection->from) {
          w->InvalidateDrawCache();
        }
      }
    }
  }
}

std::unique_ptr<Action> Location::FindAction(gui::Pointer& p, gui::ActionTrigger btn) {
  return nullptr;
}

void Location::SetNumber(double number) { SetText(f("%g", number)); }

std::string Location::ToStr() const {
  std::string_view object_name = object->Name();
  if (name.empty()) {
    if (object_name.empty()) {
      auto& o = *object;
      return typeid(o).name();
    } else {
      return std::string(object_name);
    }
  } else {
    return f("%*s \"%s\"", object_name.size(), object_name.data(), name.c_str());
  }
}

void Location::ReportMissing(std::string_view property) {
  auto error_message =
      f("Couldn't find \"%*s\". You can create a connection or rename "
        "one of the nearby objects to fix this.",
        property.size(), property.data());
  ReportError(error_message);
}

void Location::Run() {
  if (Runnable* runnable = As<Runnable>()) {
    runnable->Run(*this);
  }
}

Vec2AndDir Location::ArgStart(Argument& arg) {
  auto pos_dir = object ? object->ArgStart(arg) : Vec2AndDir{};
  auto m = ParentAs<Machine>()->TransformFromChild(*this);
  pos_dir.pos = m.mapPoint(pos_dir.pos);
  return pos_dir;
}

SkMatrix Location::TransformToChild(const Widget&) const {
  Vec2 scale_pivot = object->Shape().getBounds().center();
  SkMatrix transform = SkMatrix::I();
  float s = std::max<float>(animation_state.scale, 0.00001f);
  transform.postScale(1 / s, 1 / s, scale_pivot.x, scale_pivot.y);
  transform.preTranslate(-animation_state.position.value.x, -animation_state.position.value.y);
  return transform;
}

animation::Phase ObjectAnimationState::Tick(float delta_time, Vec2 target_position,
                                            float target_scale) {
  auto phase = position.SineTowards(target_position, delta_time, Location::kPositionSpringPeriod);
  phase |= scale.SpringTowards(target_scale, delta_time, Location::kScaleSpringPeriod,
                               Location::kSpringHalfTime);
  return phase;
}

ObjectAnimationState::ObjectAnimationState() : scale(1), position(Vec2{}), elevation(0) {}
ObjectAnimationState& Location::GetAnimationState() const { return animation_state; }
Location::~Location() {
  if (long_running) {
    long_running->Cancel();
    long_running = nullptr;
  }
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
  if (window) {
    for (int i = 0; i < window->connection_widgets.size(); ++i) {
      if (&window->connection_widgets[i]->from == this) {
        window->connection_widgets.erase(window->connection_widgets.begin() + i);
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

void AnimateGrowFrom(Location& source, Location& grown) {
  auto& animation_state = grown.GetAnimationState();
  animation_state.scale.value = 0.5;
  Vec2 source_center = source.object->Shape().getBounds().center() + source.position;
  animation_state.position.value = source_center;
  animation_state.transparency = 1;
}

void Location::PreDraw(SkCanvas& canvas) const {
  // Draw shadow
  if (object == nullptr) {
    return;
  }
  auto& anim = GetAnimationState();
  auto shape = object->Shape();

  canvas.concat(TransformFromChild(*object));

  auto rect = shape.getBounds();
  auto window = dynamic_cast<gui::Window*>(&RootWidget());
  auto window_size_px = window->size * window->display_pixels_per_meter;
  float s = canvas.getTotalMatrix().getScaleX();
  float min_elevation = 1_mm;
  SkPoint3 z_plane_params = {0, 0, (min_elevation + anim.elevation * 8_mm) * s};
  SkPoint3 light_pos = {window_size_px.width / 2.f, (float)window_size_px.height,
                        (float)window_size_px.height};
  float light_radius = window_size_px.width / 2.f;
  uint32_t flags =
      SkShadowFlags::kTransparentOccluder_ShadowFlag | SkShadowFlags::kConcaveBlurOnly_ShadowFlag;
  SkPaint shadow_paint;
  shadow_paint.setBlendMode(SkBlendMode::kMultiply);
  SkRect shadow_bounds;
  SkShadowUtils::GetLocalBounds(canvas.getTotalMatrix(), shape, z_plane_params, light_pos,
                                light_radius, flags, &shadow_bounds);
  canvas.saveLayer(&shadow_bounds, &shadow_paint);
  // Z plane params are parameters for the height function. h(x, y, z) = param_X * x + param_Y * y +
  // param_Z The height seems to be computed only for the center of the shape.
  // All of the light parameters (position, radius) are specified in pixel coordinates and ignore
  // current canvas transform.
  SkShadowUtils::DrawShadow(&canvas, shape, z_plane_params, light_pos, light_radius,
                            "#c9ced6"_color, "#ada4b0"_color, flags);
  canvas.restore();
}

void Location::UpdateAutoconnectArgs() {
  if (object == nullptr) {
    return;
  }
  auto parent_machine = root_machine.get();
  object->Args([&](Argument& arg) {
    if (arg.autoconnect_radius <= 0) {
      return;
    }

    auto start = arg.Start(*object, *parent_machine);

    // Find the current distance & target of this connection
    float old_dist2 = HUGE_VALF;
    Location* old_target = nullptr;
    if (auto it = outgoing.find(&arg); it != outgoing.end()) {
      Vec<Vec2AndDir> to_positions;
      auto conn = *it;
      conn->to.object->ConnectionPositions(to_positions);
      auto other_up = TransformBetween(*conn->to.object, *parent_machine);
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
    arg.NearbyCandidates(*this, arg.autoconnect_radius,
                         [&](Location& other, maf::Vec<Vec2AndDir>& to_points) {
                           auto other_up = TransformBetween(*other.object, *parent_machine);
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

  auto here_up = TransformBetween(*object, *parent_machine);
  Vec<Vec2AndDir> to_points;
  object->ConnectionPositions(to_points);
  for (auto& to : to_points) {
    to.pos = here_up.mapPoint(to.pos);
  }

  for (auto& other : root_machine->locations) {
    if (other.get() == this) {
      continue;
    }
    auto other_up = TransformBetween(*other->object, *parent_machine);
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
        connection_widget->InvalidateDrawCache();
      }

      auto start = other->object->ArgStart(arg);
      start.pos = other_up.mapPoint(start.pos);

      // Find the current distance & target of this connection
      float old_dist2 = HUGE_VALF;
      Location* old_target = nullptr;
      if (auto it = other->outgoing.find(&arg); it != other->outgoing.end()) {
        Vec<Vec2AndDir> to_positions;
        auto conn = *it;
        conn->to.object->ConnectionPositions(to_positions);
        auto to_up = ParentAs<Machine>()->TransformFromChild(conn->to);
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
      float new_dist2 = arg.autoconnect_radius * arg.autoconnect_radius;
      Location* new_target = nullptr;
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
}  // namespace automat
