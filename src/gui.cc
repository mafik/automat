#include "gui.h"

#include <bitset>
#include <condition_variable>
#include <vector>

#include <include/effects/SkRuntimeEffect.h>

#include "animation.h"
#include "root.h"
#include "time.h"

using namespace automaton;

namespace automaton::gui {

// Ensures that the 1x1m canvas is at least 1mm on screen.
constexpr float kMinZoom = 0.001f;
constexpr time::duration kClickTimeout = std::chrono::milliseconds(300);
constexpr float kClickRadius = 0.002f; // 2mm

SkColor background_color = SkColorSetRGB(0x80, 0x80, 0x80);
SkColor tick_color = SkColorSetRGB(0x40, 0x40, 0x40);

struct Pointer::Impl {
  Window::Impl &window;
  vec2 pointer_position;

  vec2 button_down_position[kButtonCount];
  time::point button_down_time[kButtonCount];

  std::unique_ptr<Action> action;
  Widget *hovered_widget = nullptr;
  SkMatrix hovered_widget_transform;

  Impl(Window::Impl &window, vec2 position);
  ~Impl();
  void Move(vec2 position);
  void Wheel(float delta);
  void ButtonDown(Button btn);
  void ButtonUp(Button btn);
  void Draw(SkCanvas &canvas, animation::State &animation_state) {
    if (action) {
      action->Draw(canvas, animation_state);
    }
  }
};

vec2 RoundToMilimeters(vec2 v) {
  return Vec2(round(v.X * 1000) / 1000., round(v.Y * 1000) / 1000.);
}

struct AnimatedRound : animation::Approach {
  AnimatedRound() : animation::Approach(0) { speed = 50; }
};

struct DragAction : Action {
  std::unique_ptr<Object> object;
  vec2 contact_point;
  vec2 current_position;
  dual_ptr<AnimatedRound> round_x;
  dual_ptr<AnimatedRound> round_y;

  void Begin(vec2 position) override { current_position = position; }
  void Update(vec2 position) override {
    auto old_pos = current_position - contact_point;
    auto old_round = RoundToMilimeters(old_pos);
    current_position = position;
    auto new_pos = current_position - contact_point;
    auto new_round = RoundToMilimeters(new_pos);
    if (old_round.X == new_round.X) {
      for (AnimatedRound &rx : round_x) {
        rx.value -= new_pos.X - old_pos.X;
      }
    }
    if (old_round.Y == new_round.Y) {
      for (AnimatedRound &ry : round_y) {
        ry.value -= new_pos.Y - old_pos.Y;
      }
    }
  }
  void End() override {
    RunOnAutomatonThread([pos = current_position - contact_point,
                          obj = object.release()]() mutable {
      Location &loc = root_machine->CreateEmpty();
      loc.position = RoundToMilimeters(pos);
      loc.InsertHere(std::unique_ptr<Object>(obj));
    });
  }
  void Draw(SkCanvas &canvas, animation::State &animation_state) override {
    auto original = current_position - contact_point;
    auto rounded = RoundToMilimeters(original);

    auto &rx = round_x[animation_state];
    auto &ry = round_y[animation_state];
    rx.target = rounded.X - original.X;
    ry.target = rounded.Y - original.Y;
    rx.Tick(animation_state);
    ry.Tick(animation_state);

    canvas.save();
    auto pos = current_position - contact_point + Vec2(rx, ry);
    canvas.translate(pos.X, pos.Y);
    object->Draw(canvas, animation_state);
    canvas.restore();
  }
};

struct PrototypeButton : Widget {
  const Object *proto;
  PrototypeButton(const Object *proto) : proto(proto) {}
  void Draw(SkCanvas &canvas,
            animation::State &animation_state) const override {
    proto->Draw(canvas, animation_state);
  }
  SkPath Shape() const override { return proto->Shape(); }
  std::unique_ptr<Action> ButtonDownAction(Button btn,
                                           vec2 contact_point) override {
    if (btn != kMouseLeft) {
      return nullptr;
    }
    auto drag_action = std::make_unique<DragAction>();
    drag_action->object = proto->Clone();
    drag_action->contact_point = contact_point;
    return drag_action;
  }
};

struct Window::Impl : Widget {
  vec2 position = Vec2(0, 0); // center of the window
  vec2 size;
  float display_pixels_per_meter =
      96 / kMetersPerInch; // default value assumes 96 DPI

  animation::Approach zoom = animation::Approach(1.0, 0.01);
  animation::Approach camera_x = animation::Approach(0.0, 0.005);
  animation::Approach camera_y = animation::Approach(0.0, 0.005);

  std::vector<Pointer::Impl *> pointers;

  std::bitset<kKeyCount> pressed_keys;

  animation::State animation_state;

  std::vector<PrototypeButton> prototype_buttons;
  std::vector<vec2> prototype_button_positions;

  Impl(vec2 size, float display_pixels_per_meter)
      : size(size), display_pixels_per_meter(display_pixels_per_meter) {
    prototype_buttons.reserve(Prototypes().size());
    for (auto &proto : Prototypes()) {
      prototype_buttons.emplace_back(proto);
      prototype_button_positions.emplace_back(Vec2(0, 0));
    }
    ArrangePrototypeButtons();
  }

  void ArrangePrototypeButtons() {
    float max_w = size.Width;
    vec2 cursor = Vec2(0, 0);
    for (int i = 0; i < prototype_buttons.size(); i++) {
      auto &btn = prototype_buttons[i];
      vec2 &pos = prototype_button_positions[i];
      SkPath shape = btn.Shape();
      SkRect bounds = shape.getBounds();
      if (cursor.X + bounds.width() + 0.001 > max_w) {
        cursor.X = 0;
        cursor.Y += bounds.height() + 0.001;
      }
      pos = cursor + Vec2(0.001, 0.001) - Vec2(bounds.left(), bounds.top());
      cursor.X += bounds.width() + 0.001;
    }
  }

  float PxPerMeter() { return display_pixels_per_meter * zoom; }

  SkRect GetCameraRect() {
    return SkRect::MakeXYWH(camera_x - size.Width / 2,
                            camera_y - size.Height / 2, size.Width,
                            size.Height);
  }

  SkPaint &GetBackgroundPaint() {
    static SkRuntimeShaderBuilder builder = []() {
      const char *sksl = R"(
        uniform float px_per_m;

        // Dark theme
        //float4 bg = float4(0.05, 0.05, 0.00, 1);
        //float4 fg = float4(0.0, 0.32, 0.8, 1);

        float4 bg = float4(0.9, 0.9, 0.9, 1);
        float4 fg = float4(0.5, 0.5, 0.5, 1);

        float grid(vec2 coord_m, float dots_per_m, float r_px) {
          float r = r_px / px_per_m;
          vec2 grid_coord = fract(coord_m * dots_per_m + 0.5) - 0.5;
          return smoothstep(r, r - 1/px_per_m, length(grid_coord) / dots_per_m) * smoothstep(1./(3*r), 1./(32*r), dots_per_m);
        }

        half4 main(vec2 fragcoord) {
          float dm_grid = grid(fragcoord, 10, 3);
          float cm_grid = grid(fragcoord, 100, 3) * 0.6;
          float mm_grid = grid(fragcoord, 1000, 2) * 0.4;
          float d = max(max(mm_grid, cm_grid), dm_grid);
          return mix(bg, fg, d);
        }
      )";

      auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(sksl));
      if (!err.isEmpty()) {
        FATAL() << err.c_str();
      }
      SkRuntimeShaderBuilder builder(effect);
      return builder;
    }();
    static SkPaint paint;
    builder.uniform("px_per_m") = PxPerMeter();
    paint.setShader(builder.makeShader());
    return paint;
  }

  vec2 WindowToCanvas(vec2 window) {
    return (window - size / 2) / zoom + Vec2(camera_x, camera_y);
  }

  SkMatrix WindowToCanvas() {
    SkMatrix m;
    m.setTranslate(-size.Width / 2, -size.Height / 2);
    m.postScale(1 / zoom, 1 / zoom);
    m.postTranslate(camera_x, camera_y);
    return m;
  }

  vec2 CanvasToWindow(vec2 canvas) {
    return (canvas - Vec2(camera_x, camera_y)) * zoom + size / 2;
  }

  void Resize(vec2 size) {
    this->size = size;
    ArrangePrototypeButtons();
  }
  void DisplayPixelDensity(float pixels_per_meter) {
    this->display_pixels_per_meter = pixels_per_meter;
  }
  SkPath Shape() const override {
    return SkPath::Rect(SkRect::MakeXYWH(0, 0, size.Width, size.Height));
  }
  void Draw(SkCanvas &, animation::State &animation_state) const override {
    FATAL() << "Window::Impl::Draw() should never be called";
  }
  void Draw(SkCanvas &canvas) {
    animation_state.timer.Tick();
    RunOnAutomatonThreadSynchronous([&] {
      float rx = camera_x.Remaining();
      float ry = camera_y.Remaining();
      float rz = zoom.Remaining();
      float r = sqrt(rx * rx + ry * ry);
      float rpx = PxPerMeter() * r;
      bool stabilize_mouse = rpx < 1;

      if (stabilize_mouse) {
        if (pointers.size() > 0) {
          Pointer::Impl *first_pointer = *pointers.begin();
          vec2 mouse_position = first_pointer->pointer_position;
          vec2 focus_pre = WindowToCanvas(mouse_position);
          zoom.Tick(animation_state);
          vec2 focus_post = WindowToCanvas(mouse_position);
          vec2 focus_delta = focus_post - focus_pre;
          camera_x.Shift(-focus_delta.X);
          camera_y.Shift(-focus_delta.Y);
        }
      } else { // stabilize camera target
        vec2 focus_pre = Vec2(camera_x.target, camera_y.target);
        vec2 target_screen = CanvasToWindow(focus_pre);
        zoom.Tick(animation_state);
        vec2 focus_post = WindowToCanvas(target_screen);
        vec2 focus_delta = focus_post - focus_pre;
        camera_x.value -= focus_delta.X;
        camera_y.value -= focus_delta.Y;
      }

      camera_x.Tick(animation_state);
      camera_y.Tick(animation_state);

      if (pressed_keys.test(kKeyW)) {
        camera_y.Shift(0.1 * animation_state.timer.d);
      }
      if (pressed_keys.test(kKeyS)) {
        camera_y.Shift(-0.1 * animation_state.timer.d);
      }
      if (pressed_keys.test(kKeyA)) {
        camera_x.Shift(-0.1 * animation_state.timer.d);
      }
      if (pressed_keys.test(kKeyD)) {
        camera_x.Shift(0.1 * animation_state.timer.d);
      }

      SkRect work_area = SkRect::MakeXYWH(-0.5, -0.5, 1, 1);

      // Make sure that work area doesn't leave the window bounds (so the user
      // doesn't get lost)
      {
        // Leave 1mm of margin so that the user can still see the edge of the
        // work area
        vec2 bottom_left = WindowToCanvas(Vec2(0.001, 0.001));
        vec2 top_right = WindowToCanvas(size - Vec2(0.001, 0.001));
        SkRect window_bounds = SkRect::MakeLTRB(bottom_left.X, top_right.Y,
                                                top_right.X, bottom_left.Y);
        if (work_area.left() > window_bounds.right()) {
          camera_x.Shift(work_area.left() - window_bounds.right());
        }
        if (work_area.right() < window_bounds.left()) {
          camera_x.Shift(work_area.right() - window_bounds.left());
        }
        // The y axis is flipped so `work_area.bottom()` is actually its top
        if (work_area.bottom() < window_bounds.bottom()) {
          camera_y.Shift(work_area.bottom() - window_bounds.bottom());
        }
        if (work_area.top() > window_bounds.top()) {
          camera_y.Shift(work_area.top() - window_bounds.top());
        }
      }

      canvas.save();
      canvas.translate(size.Width / 2., size.Height / 2.);
      canvas.scale(zoom, zoom);
      canvas.translate(-camera_x, -camera_y);

      // Draw background
      canvas.clear(background_color);
      canvas.drawRect(work_area, GetBackgroundPaint());
      SkPaint border_paint;
      border_paint.setColor(tick_color);
      border_paint.setStyle(SkPaint::kStroke_Style);
      canvas.drawRect(work_area, border_paint);

      // Draw target window size when zooming in with middle mouse button
      if (zoom.target == 1 && rz > 0.001) {
        SkPaint target_paint(SkColor4f(0, 0.3, 0.8, rz));
        target_paint.setStyle(SkPaint::kStroke_Style);
        target_paint.setStrokeWidth(0.001); // 1mm
        float target_width = size.Width;
        float target_height = size.Height;
        SkRect target_rect = SkRect::MakeXYWH(
            camera_x.target - target_width / 2,
            camera_y.target - target_height / 2, target_width, target_height);
        canvas.drawRect(target_rect, target_paint);
      }

      root_machine->DrawContents(canvas, animation_state);

      for (auto &pointer : pointers) {
        pointer->Draw(canvas, animation_state);
      }

      canvas.restore();
    });

    // Draw prototype shelf
    for (int i = 0; i < prototype_buttons.size(); i++) {
      vec2 &position = prototype_button_positions[i];
      canvas.save();
      canvas.translate(position.X, position.Y);
      prototype_buttons[i].Draw(canvas, animation_state);
      canvas.restore();
    }
  }
  VisitResult VisitImmediateChildren(WidgetVisitor &visitor) override {
    for (int i = 0; i < prototype_buttons.size(); i++) {
      SkMatrix matrix = SkMatrix::Translate(-prototype_button_positions[i].X,
                                            -prototype_button_positions[i].Y);
      auto result = visitor(prototype_buttons[i], matrix);
      if (result != VisitResult::kContinue)
        return result;
    }
    VisitResult result = VisitResult::kContinue;
    RunOnAutomatonThreadSynchronous([&]() {
      SkMatrix matrix = WindowToCanvas();
      result = visitor(*root_machine, matrix);
    });
    return result;
  }
  void KeyDown(Key key) {
    if (key == kKeyUnknown || key >= kKeyCount)
      return;
    pressed_keys.set(key);
  }
  void KeyUp(Key key) {
    if (key == kKeyUnknown || key >= kKeyCount)
      return;
    pressed_keys.reset(key);
  }
  std::unique_ptr<Pointer> MakePointer(vec2 position);
  std::string_view GetState();
};

Pointer::Impl::Impl(Window::Impl &window, vec2 position)
    : window(window), pointer_position(position), button_down_position(),
      button_down_time() {
  window.pointers.push_back(this);
}
Pointer::Impl::~Impl() {
  if (hovered_widget) {
    hovered_widget->OnHover(false, window.animation_state);
  }
  auto it = std::find(window.pointers.begin(), window.pointers.end(), this);
  if (it != window.pointers.end()) {
    window.pointers.erase(it);
  }
}
void Pointer::Impl::Move(vec2 position) {
  vec2 old_mouse_pos = pointer_position;
  pointer_position = position;
  if (button_down_time[kMouseMiddle] > time::kZero) {
    vec2 delta =
        window.WindowToCanvas(position) - window.WindowToCanvas(old_mouse_pos);
    window.camera_x.Shift(-delta.X);
    window.camera_y.Shift(-delta.Y);
  }
  if (action) {
    action->Update(window.WindowToCanvas(position));
  } else {
    Widget *old_hovered_widget = hovered_widget;
    hovered_widget = nullptr;
    window.VisitAtPoint(pointer_position,
                        [&](Widget &widget, const SkMatrix &transform) {
                          hovered_widget = &widget;
                          hovered_widget_transform = transform;
                          return VisitResult::kStop;
                        });
    if (old_hovered_widget != hovered_widget) {
      if (old_hovered_widget) {
        old_hovered_widget->OnHover(false, window.animation_state);
      }
      if (hovered_widget) {
        hovered_widget->OnHover(true, window.animation_state);
      }
    }
  }
}
void Pointer::Impl::Wheel(float delta) {
  float factor = exp(delta / 4);
  window.zoom.target *= factor;
  // For small changes we skip the animation to increase responsiveness.
  if (fabs(delta) < 1.0) {
    vec2 mouse_pre = window.WindowToCanvas(pointer_position);
    window.zoom.value *= factor;
    vec2 mouse_post = window.WindowToCanvas(pointer_position);
    vec2 mouse_delta = mouse_post - mouse_pre;
    window.camera_x.Shift(-mouse_delta.X);
    window.camera_y.Shift(-mouse_delta.Y);
  }
  window.zoom.target = std::max(kMinZoom, window.zoom.target);
}
void Pointer::Impl::ButtonDown(Button btn) {
  if (btn == kButtonUnknown || btn >= kButtonCount)
    return;
  button_down_position[btn] = pointer_position;
  button_down_time[btn] = time::now();

  if (action == nullptr && hovered_widget) {
    SkPoint local_pointer_position_sk =
        hovered_widget_transform.mapXY(pointer_position.X, pointer_position.Y);
    vec2 local_pointer_position{local_pointer_position_sk.fX,
                                local_pointer_position_sk.fY};
    action = hovered_widget->ButtonDownAction(btn, local_pointer_position);
    if (action) {
      action->Begin(window.WindowToCanvas(pointer_position));
    }
  }
}
void Pointer::Impl::ButtonUp(Button btn) {
  if (btn == kButtonUnknown || btn >= kButtonCount)
    return;
  if (btn == kMouseLeft) {
    if (action) {
      action->End();
      action.reset();
    }
  }
  if (btn == kMouseMiddle) {
    time::duration down_duration = time::now() - button_down_time[kMouseMiddle];
    vec2 delta = pointer_position - button_down_position[kMouseMiddle];
    float delta_m = Length(delta);
    if ((down_duration < kClickTimeout) && (delta_m < kClickRadius)) {
      vec2 canvas_pos = window.WindowToCanvas(pointer_position);
      window.camera_x.target = canvas_pos.X;
      window.camera_y.target = canvas_pos.Y;
      window.zoom.target = 1;
    }
  }
  button_down_position[btn] = Vec2(0, 0);
  button_down_time[btn] = time::kZero;
}

std::vector<Window *> windows = {};

Window::Window(vec2 size, float pixels_per_meter,
               std::string_view initial_state)
    : impl(std::make_unique<Window::Impl>(size, pixels_per_meter)) {
  windows.push_back(this);
}
Window::~Window() {
  windows.erase(std::find(windows.begin(), windows.end(), this));
}
void Window::Resize(vec2 size) { impl->Resize(size); }
void Window::DisplayPixelDensity(float pixels_per_meter) {
  impl->DisplayPixelDensity(pixels_per_meter);
}
void Window::Draw(SkCanvas &canvas) { impl->Draw(canvas); }
void Window::KeyDown(Key key) { impl->KeyDown(key); }
void Window::KeyUp(Key key) { impl->KeyUp(key); }
std::string_view Window::GetState() { return {}; }

Pointer::Pointer(Window &window, vec2 position)
    : impl(std::make_unique<Pointer::Impl>(*window.impl, position)) {}
Pointer::~Pointer() {}
void Pointer::Move(vec2 position) { impl->Move(position); }
void Pointer::Wheel(float delta) { impl->Wheel(delta); }
void Pointer::ButtonDown(Button btn) { impl->ButtonDown(btn); }
void Pointer::ButtonUp(Button btn) { impl->ButtonUp(btn); }

void Widget::VisitAll(WidgetVisitor &visitor) {

  struct RecursiveVisitor : WidgetVisitor {
    WidgetVisitor &orig_visitor;
    SkMatrix transform_accumulator;

    RecursiveVisitor(WidgetVisitor &orig_visitor)
        : orig_visitor(orig_visitor), transform_accumulator() {}

    VisitResult operator()(Widget &widget, const SkMatrix &transform) override {

      SkMatrix backup = transform_accumulator;
      transform_accumulator.postConcat(transform);

      if (widget.VisitImmediateChildren(*this) == VisitResult::kStop)
        return VisitResult::kStop;

      if (orig_visitor(widget, transform_accumulator) == VisitResult::kStop)
        return VisitResult::kStop;

      transform_accumulator = backup;

      return VisitResult::kContinue;
    }
  };

  RecursiveVisitor recursive_visitor(visitor);

  recursive_visitor(*this, SkMatrix::I());
}

void Widget::VisitAtPoint(vec2 point, WidgetVisitor &visitor) {

  struct PointVisitor : WidgetVisitor {
    vec2 point;
    WidgetVisitor &orig_visitor;

    PointVisitor(vec2 point, WidgetVisitor &orig_visitor)
        : point(point), orig_visitor(orig_visitor) {}

    VisitResult operator()(Widget &widget, const SkMatrix &transform) override {
      auto shape = widget.Shape();
      SkPoint mapped_point = transform.mapXY(point.X, point.Y);
      if (shape.isEmpty() || shape.contains(mapped_point.fX, mapped_point.fY)) {
        if (orig_visitor(widget, transform) == VisitResult::kStop)
          return VisitResult::kStop;
      }
      return VisitResult::kContinue;
    }
  };

  PointVisitor point_visitor(point, visitor);

  VisitAll(point_visitor);
}

struct FunctionWidgetVisitor : WidgetVisitor {
  FunctionWidgetVisitor(WidgetVisitorFunc visitor) : func(visitor) {}
  VisitResult operator()(Widget &widget, const SkMatrix &transform) override {
    return func(widget, transform);
  }
  WidgetVisitorFunc func;
};

void Widget::VisitAll(WidgetVisitorFunc visitor) {
  FunctionWidgetVisitor function_visitor(visitor);
  VisitAll(function_visitor);
}

void Widget::VisitAtPoint(vec2 point, WidgetVisitorFunc visitor) {
  FunctionWidgetVisitor function_visitor(visitor);
  VisitAtPoint(point, function_visitor);
}

} // namespace automaton::gui