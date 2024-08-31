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
#include "control_flow.hh"
#include "drag_action.hh"
#include "font.hh"
#include "format.hh"
#include "gui_connection_widget.hh"
#include "gui_constants.hh"
#include "math.hh"
#include "root.hh"
#include "span.hh"
#include "timer_thread.hh"
#include "widget.hh"
#include "window.hh"

using namespace automat::gui;
using namespace maf;
using namespace std;

namespace automat {

constexpr float kFrameCornerRadius = 0.001;

Location::Location(Location* parent) : parent(parent), run_button(this), run_task(this) {}

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

void Location::Put(unique_ptr<Object> obj) {
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

unique_ptr<Object> Location::Take() {
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
  Connection* c = new Connection(*this, other, pointer_behavior);
  outgoing.emplace(&arg, c);
  other.incoming.emplace(&arg, c);
  object->ConnectionAdded(*this, arg, *c);
  return c;
}

void Location::ScheduleRun() { run_task.Schedule(); }

void Location::ScheduleLocalUpdate(Location& updated) {
  (new UpdateTask(this, &updated))->Schedule();
}

void Location::ScheduleErrored(Location& errored) { (new ErroredTask(this, &errored))->Schedule(); }

SkPath Location::Shape(animation::Display*) const {
  if constexpr (false) {  // Gray box shape
    // Keeping this around because locations will eventually be toggleable between frame & no frame
    // modes.
    SkRect object_bounds;
    if (object) {
      object_bounds = object->Shape(nullptr).getBounds();
    } else {
      object_bounds = SkRect::MakeEmpty();
    }
    float outset = 0.001 - kBorderWidth / 2;
    SkRect bounds = object_bounds.makeOutset(outset, outset);
    // expand the bounds to include the run button
    SkPath run_button_shape = run_button.Shape(nullptr);
    bounds.fTop -= run_button_shape.getBounds().height() + 0.001;
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
      return object->Shape(nullptr);
    }
  }
  return SkPath();
}

ControlFlow Location::VisitChildren(gui::Visitor& visitor) {
  if (object) {
    Widget* arr[] = {object.get()};
    if (visitor(arr) == ControlFlow::Stop) {
      return ControlFlow::Stop;
    }
  }
  if constexpr (false) {
    // Keeping this around because locations will eventually be toggleable between frame & no frame
    // modes.
    // if (visitor(run_button) == ControlFlow::Stop) {
    //   return ControlFlow::Stop;
    // }
  }
  return ControlFlow::Continue;
}

bool Location::ChildrenOutside() const { return true; }

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

void Location::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  SkPath my_shape;
  if (object) {
    my_shape = object->Shape(nullptr);
  } else {
    my_shape = Shape(nullptr);
  }
  SkRect bounds = my_shape.getBounds();

  auto& state = GetAnimationState(ctx.display);
  state.Tick(ctx.DeltaT(), position, scale);

  state.highlight.Tick(ctx.display);
  state.transparency.Tick(ctx.display);
  bool using_layer = false;
  if (state.transparency > 0.01) {
    using_layer = true;
    ctx.canvas.saveLayerAlphaf(&bounds, 1.f - state.transparency);
  }

  {  // Draw dashed highlight outline
    SkPath outset_shape = Outset(my_shape, 0.0025 * state.highlight.value);
    SkPathMeasure measure(outset_shape, false);
    float length = measure.getLength();

    static const SkPaint kHighlightPaint = [] {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(0.0005);
      paint.setColor(0xffa87347);
      return paint;
    }();
    SkPaint dash_paint(kHighlightPaint);
    dash_paint.setAlphaf(state.highlight.value);
    float intervals[] = {0.0035, 0.0015};
    double ignore;
    time::Duration period = 200s;
    float phase = std::fmod(ctx.display.timer.now.time_since_epoch().count(), period.count()) /
                  period.count();
    dash_paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, phase));
    ctx.canvas.drawPath(outset_shape, dash_paint);
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

  DrawChildren(ctx);

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
    ctx.canvas.restore();
  }
}

std::unique_ptr<Action> Location::ButtonDownAction(gui::Pointer& p, gui::PointerButton btn) {
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

Vec2AndDir Location::ArgStart(animation::Display* display, Argument& arg) {
  auto pos_dir = object ? object->ArgStart(arg) : Vec2AndDir{};
  Path path = {ParentAs<Widget>(), (Widget*)this};
  auto m = TransformUp(path, display);
  pos_dir.pos = m.mapPoint(pos_dir.pos);
  return pos_dir;
}

static SkMatrix GetLocationTransform(Vec2 position, float scale, Vec2 scale_pivot) {
  SkMatrix transform = SkMatrix::I();
  float s = std::max<float>(scale, 0.00001f);
  transform.postScale(1 / s, 1 / s, scale_pivot.x, scale_pivot.y);
  transform.preTranslate(-position.x, -position.y);
  return transform;
}

SkMatrix ObjectAnimationState::GetTransform(Vec2 scale_pivot) const {
  return GetLocationTransform(position, scale, scale_pivot);
}

SkMatrix Location::GetTransform(animation::Display* display) const {
  Vec2 scale_pivot = object->Shape(nullptr).getBounds().center();
  if (display) {
    if (auto* anim = animation_state.Find(*display)) {
      return anim->GetTransform(scale_pivot);
    }
  }
  return GetLocationTransform(position, scale, scale_pivot);
}

void ObjectAnimationState::Tick(float delta_time, Vec2 target_position, float target_scale) {
  position.SineTowards(target_position, delta_time, Location::kSpringPeriod);
  scale.SpringTowards(target_scale, delta_time, Location::kSpringPeriod, Location::kSpringHalfTime);
}

ObjectAnimationState::ObjectAnimationState() : scale(1), position(Vec2{}), elevation(0) {
  transparency.speed = 5;
}
ObjectAnimationState& Location::GetAnimationState(animation::Display& display) const {
  if (auto* anim = animation_state.Find(display)) {
    return *anim;
  } else {
    auto& new_anim = animation_state[display];
    new_anim.position.value = position;
    new_anim.scale.value = scale;
    return new_anim;
  }
}
Location::~Location() {
  if (long_running) {
    long_running->Cancel();
    long_running = nullptr;
  }
  // Location can only be destroyed by its parent so we don't have to do anything there.
  parent = nullptr;
  while (not incoming.empty()) {
    delete incoming.begin()->second;
  }
  while (not outgoing.empty()) {
    delete outgoing.begin()->second;
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
  if (no_scheduling.contains(this)) {
    no_scheduling.erase(this);
  }
  CancelScheduledAt(*this);
  if (auto waiting_task = events.peek<Task>()) {
    if (waiting_task->target == this) {
      events.recv<Task>();  // drops the unique_ptr
    }
  }
  for (int i = queue.size() - 1; i >= 0; --i) {
    if (queue[i]->target == this) {
      queue.erase(queue.begin() + i);
    }
  }
  for (int i = global_successors.size() - 1; i >= 0; --i) {
    if (global_successors[i]->target == this) {
      global_successors.erase(global_successors.begin() + i);
    }
  }

  for (int i = 0; i < window->connection_widgets.size(); ++i) {
    if (&window->connection_widgets[i]->from == this) {
      window->connection_widgets.erase(window->connection_widgets.begin() + i);
      --i;
    }
  }
}

gui::DisplayContext GuessDisplayContext(Location& location, animation::Display& display) {
  DisplayContext ctx = {.display = display, .path = {window.get()}};
  if (auto* parent = location.ParentAs<Widget>()) {
    ctx.path.push_back(parent);
  } else {
    // TODO: This is so wrong... Fix it somehow...
    for (auto& pointer : gui::window->pointers) {
      if (auto* action = pointer->action.get()) {
        if (auto* action_widget = action->Widget()) {
          ctx.path.push_back(action_widget);
          break;
        }
      }
    }
  }
  ctx.path.push_back(&location);
  ctx.path.push_back(location.object.get());
  return ctx;
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
  for (auto* display : animation::displays) {
    auto& animation_state = grown.GetAnimationState(*display);
    animation_state.scale.value = 0.5;
    Vec2 source_center = source.object->Shape(nullptr).getBounds().center() + source.position;
    animation_state.position.value = source_center;
    animation_state.transparency.value = 1;
  }
}

void Location::PreDraw(gui::DrawContext& ctx) const {
  // Draw shadow
  if (object == nullptr) {
    return;
  }
  auto& anim = animation_state[ctx.display];
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
  anim.elevation.SineTowards(target_elevation, ctx.DeltaT(), 0.2);
  auto shape = object->Shape(&ctx.display);
  auto rect = shape.getBounds();
  auto surface = ctx.canvas.getSurface();
  float s = ctx.canvas.getTotalMatrix().getScaleX();
  float min_elevation = 1_mm;
  SkPoint3 z_plane_params = {0, 0, (min_elevation + anim.elevation * 8_mm) * s};
  SkPoint3 light_pos = {surface->width() / 2.f, (float)surface->height(), (float)surface->height()};
  float light_radius = surface->width() / 2.f;
  uint32_t flags =
      SkShadowFlags::kTransparentOccluder_ShadowFlag | SkShadowFlags::kConcaveBlurOnly_ShadowFlag;
  SkPaint shadow_paint;
  shadow_paint.setBlendMode(SkBlendMode::kMultiply);
  SkRect shadow_bounds;
  SkShadowUtils::GetLocalBounds(ctx.canvas.getTotalMatrix(), shape, z_plane_params, light_pos,
                                light_radius, flags, &shadow_bounds);
  ctx.canvas.saveLayer(&shadow_bounds, &shadow_paint);
  // Z plane params are parameters for the height function. h(x, y, z) = param_X * x + param_Y * y +
  // param_Z The height seems to be computed only for the center of the shape.
  // All of the light parameters (position, radius) are specified in pixel coordinates and ignore
  // current canvas transform.
  SkShadowUtils::DrawShadow(&ctx.canvas, shape, z_plane_params, light_pos, light_radius,
                            "#c9ced6"_color, "#ada4b0"_color, flags);
  ctx.canvas.restore();
  PreDrawChildren(ctx);
}

void Location::UpdateAutoconnectArgs() {
  auto here_up = TransformUp(Path{root_machine, this}, nullptr);
  object->Args([&](Argument& arg) {
    if (arg.autoconnect_radius <= 0) {
      return;
    }

    auto start = object->ArgStart(arg);
    start.pos = here_up.mapPoint(start.pos);

    // Find the current distance & target of this connection
    float old_dist2 = HUGE_VALF;
    Location* old_target = nullptr;
    if (auto it = outgoing.find(&arg); it != outgoing.end()) {
      Vec<Vec2AndDir> to_positions;
      auto conn = it->second;
      conn->to.object->ConnectionPositions(to_positions);
      auto other_up = TransformUp(Path{root_machine, &conn->to}, nullptr);
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
                           for (auto& to_pos : to_points) {
                             float dist2 = LengthSquared(start.pos - to_pos.pos);
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
      auto old_conn = outgoing.find(&arg)->second;
      delete old_conn;
    }
    if (new_target) {
      ConnectTo(*new_target, arg);
    }
  });

  // Now check other locatinos & their arguments that might want to connect to this location

  Vec<Vec2AndDir> to_points;
  object->ConnectionPositions(to_points);
  for (auto& to : to_points) {
    to.pos = here_up.mapPoint(to.pos);
  }

  for (auto& other : root_machine->locations) {
    if (other.get() == this) {
      continue;
    }
    auto other_up = TransformUp(Path{root_machine, other.get()}, nullptr);
    other->object->Args([&](Argument& arg) {
      if (arg.autoconnect_radius <= 0) {
        return;
      }
      Str error;
      arg.CheckRequirements(*other, this, object.get(), error);
      if (!error.empty()) {
        return;  // `this` location can't be connected to `other`s `arg`
      }
      auto start = other->object->ArgStart(arg);
      start.pos = other_up.mapPoint(start.pos);

      // Find the current distance & target of this connection
      float old_dist2 = HUGE_VALF;
      Location* old_target = nullptr;
      if (auto it = other->outgoing.find(&arg); it != other->outgoing.end()) {
        Vec<Vec2AndDir> to_positions;
        auto conn = it->second;
        conn->to.object->ConnectionPositions(to_positions);
        auto to_up = TransformUp(Path{root_machine, &conn->to}, nullptr);
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
        auto old_conn = other->outgoing.find(&arg)->second;
        delete old_conn;
      }
      if (new_target) {
        other->ConnectTo(*new_target, arg);
      }
    });
  }
}

}  // namespace automat
