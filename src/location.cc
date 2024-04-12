#include "location.hh"

#include <include/core/SkPath.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPathUtils.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradientShader.h>
#include <include/pathops/SkPathOps.h>

#include "base.hh"
#include "color.hh"
#include "control_flow.hh"
#include "drag_action.hh"
#include "font.hh"
#include "format.hh"
#include "gui_constants.hh"
#include "span.hh"
#include "svg.hh"

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
    // expand the bounds to include the run button
    SkPath run_button_shape = run_button.Shape();
    bounds.fTop -= run_button_shape.getBounds().height() + 0.001;
    return SkPath::RRect(bounds, kFrameCornerRadius, kFrameCornerRadius);
  }
  static SkPath empty_path = SkPath();
  return empty_path;
}

SkPath Location::ArgShape(Argument& arg) const {
  if (object) {
    auto object_arg_shape = object->ArgShape(arg);
    if (!object_arg_shape.isEmpty()) {
      return object_arg_shape;
    } else {
      return object->Shape();
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

SkMatrix Location::TransformToChild(const Widget& child, animation::Context&) const {
  if constexpr (false) {
    // Keeping this around because locations will eventually be toggleable between frame & no frame
    // modes.
    SkPath my_shape = Shape();
    SkRect my_bounds = my_shape.getBounds();
    if (&child == &run_button) {
      SkPath run_button_shape = run_button.Shape();
      SkRect run_bounds = run_button_shape.getBounds();
      return SkMatrix::Translate(-(my_bounds.centerX() - run_bounds.centerX()),
                                 -(my_bounds.top() - run_bounds.fTop) - 0.001);
    }
  }
  return SkMatrix::I();
}

Vec2 Location::AnimatedPosition(animation::Context& actx) const {
  Vec2 ret = position;
  if (drag_action) {
    ret.x += drag_action->round_x[actx];
    ret.y += drag_action->round_y[actx];
  }
  return ret;
}

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
  if constexpr (true) {
    if (object) {
      my_shape = object->Shape();
    } else {
      my_shape = Shape();
    }
  } else {
    my_shape = Shape();
  }
  SkRect bounds = my_shape.getBounds();

  {  // Draw dashed highlight outline
    auto& highlight = highlight_ptr[ctx.animation_context];
    highlight.Tick(ctx.animation_context);
    SkPath outset_shape = Outset(my_shape, 0.0025 * highlight.value);
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
    dash_paint.setAlphaf(highlight.value);
    float intervals[] = {0.0035, 0.0015};
    double ignore;
    time::duration period = 200s;
    float phase =
        std::fmod(ctx.animation_context.timer.now.time_since_epoch().count(), period.count()) /
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
}

std::unique_ptr<Action> Location::ButtonDownAction(gui::Pointer& p, gui::PointerButton btn) {
  if constexpr (false) {
    if (btn == gui::PointerButton::kMouseLeft) {
      auto a = std::make_unique<DragLocationAction>(this);
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
    runnable->RunAndScheduleNext(*this);
  }
}

}  // namespace automat
