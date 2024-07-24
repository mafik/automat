#include "location.hh"

#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkPictureRecorder.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradientShader.h>
#include <include/pathops/SkPathOps.h>

#include "animation.hh"
#include "base.hh"
#include "color.hh"
#include "control_flow.hh"
#include "drag_action.hh"
#include "font.hh"
#include "format.hh"
#include "gui_constants.hh"
#include "math.hh"
#include "span.hh"
#include "timer_thread.hh"
#include "widget.hh"

using namespace automat::gui;

namespace automat {

constexpr float kFrameCornerRadius = 0.001;

Location::Location(Location* parent) : parent(parent), run_button(this), run_task(this) {}

void* Location::Nearby(std::function<void*(Location&)> callback) {
  if (auto parent_machine = ParentAs<Machine>()) {
    // TODO: sort by distance
    for (auto& other : parent_machine->locations) {
      if (auto ret = callback(*other)) {
        return ret;
      }
    }
  }
  return nullptr;
}

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

Connection* Location::ConnectTo(Location& other, string_view label,
                                Connection::PointerBehavior pointer_behavior) {
  object->Args([&](Argument& arg) {
    if (arg.name == label && arg.precondition >= Argument::kRequiresConcreteType) {
      std::string error;
      arg.CheckRequirements(*this, &other, other.object.get(), error);
      if (error.empty()) {
        pointer_behavior = Connection::kTerminateHere;
      }
    }
  });
  Connection* c = new Connection(*this, other, pointer_behavior);
  outgoing.emplace(label, c);
  other.incoming.emplace(label, c);
  object->ConnectionAdded(*this, label, *c);
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
  if constexpr (false) {
    if (btn == gui::PointerButton::kMouseLeft) {
      auto a = std::make_unique<DragLocationAction>(p, this);
      a->contact_point = p.PositionWithin(*this);
      return a;
    }
  }
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

ObjectAnimationState::ObjectAnimationState() : scale(1), position(Vec2{}) {
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
}
}  // namespace automat
