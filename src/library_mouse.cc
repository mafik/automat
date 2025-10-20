// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "library_mouse.hh"

#include <include/core/SkBlendMode.h>
#include <include/core/SkColor.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkShader.h>
#include <include/pathops/SkPathOps.h>

#include <atomic>
#include <tracy/Tracy.hpp>

#include "animation.hh"
#include "textures.hh"

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__linux__)
#include <xcb/xproto.h>
#include <xcb/xtest.h>
#endif

#include "../build/generated/krita_hand.hh"
#include "../build/generated/krita_mouse.hh"
#include "automat.hh"
#include "embedded.hh"
#include "global_resources.hh"
#include "log.hh"
#include "math.hh"
#include "svg.hh"
#include "textures.hh"
#include "widget.hh"

#if defined(__linux__)
#include "xcb.hh"
#endif

namespace automat::library::mouse {
SkRuntimeEffect& GetPixelGridRuntimeEffect() {
  static const auto runtime_effect = []() {
    Status status;
    auto runtime_effect = resources::CompileShader(embedded::assets_pixel_grid_rt_sksl, status);
    if (!OK(status)) {
      FATAL << status;
    }
    return runtime_effect;
  }();
  return *runtime_effect;
}

}  // namespace automat::library::mouse

namespace automat::library {

ui::PointerButton ButtonNameToEnum(const std::string& name) {
  static const std::map<std::string, ui::PointerButton> button_name_to_enum = {
      {"left", ui::PointerButton::Left},       {"middle", ui::PointerButton::Middle},
      {"right", ui::PointerButton::Right},     {"back", ui::PointerButton::Back},
      {"forward", ui::PointerButton::Forward},
  };
  auto it = button_name_to_enum.find(name);
  if (it != button_name_to_enum.end()) {
    return it->second;
  }
  return ui::PointerButton::Unknown;
}

const char* ButtonEnumToName(ui::PointerButton button) {
  switch (button) {
    case ui::PointerButton::Left:
      return "left";
    case ui::PointerButton::Middle:
      return "middle";
    case ui::PointerButton::Right:
      return "right";
    case ui::PointerButton::Back:
      return "back";
    case ui::PointerButton::Forward:
      return "forward";
    default:
      return "unknown";
  }
}

struct ObjectIcon : ui::Widget {
  std::unique_ptr<Object::WidgetInterface> object_widget;
  ObjectIcon(ui::Widget* parent) : ui::Widget(parent) {}
  SkPath Shape() const override { return SkPath(); }
  Optional<Rect> TextureBounds() const override { return std::nullopt; }

  static std::unique_ptr<ObjectIcon> Make(ui::Widget* parent, Object& object) {
    auto object_icon = std::make_unique<ObjectIcon>(parent);
    object_icon->object_widget = object.MakeWidget(object_icon.get());
    auto coarse_bounds = object_icon->object_widget->CoarseBounds().rect;
    auto desired_size = Rect::MakeCenterZero(12_mm, 12_mm);
    object_icon->object_widget->local_to_parent =
        SkM44(SkMatrix::RectToRect(coarse_bounds, desired_size, SkMatrix::kCenter_ScaleToFit));
    return object_icon;
  }
  void FillChildren(Vec<Widget*>& children) override { children.push_back(object_widget.get()); }
};

struct MakeObjectOption : Option {
  Ptr<Object> proto;
  MakeObjectOption(Ptr<Object> proto) : proto(proto) {}
  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override {
    return ObjectIcon::Make(parent, *proto);
  }
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MakeObjectOption>(proto);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    auto loc = MAKE_PTR(Location, pointer.hover, root_location->AcquireWeakPtr());
    loc->Create(*proto);
    audio::Play(embedded::assets_SFX_toolbar_pick_wav);
    return std::make_unique<DragLocationAction>(pointer, std::move(loc));
  }
};

struct MouseWidget : Object::WidgetBase {
  MouseWidget(ui::Widget* parent, WeakPtr<Object>&& object) : WidgetBase(parent) {
    this->object = std::move(object);
  }

  std::string_view Name() const override { return "Mouse"; }

  RRect CoarseBounds() const override { return RRect::MakeSimple(krita::mouse::base.rect, 0); }

  Optional<Rect> TextureBounds() const override { return krita::mouse::base.rect; }

  SkPath Shape() const override {
    static auto shape = krita::mouse::Shape();
    return shape;
  }

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    krita::mouse::head.draw(canvas);
  }

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    Object::WidgetBase::VisitOptions(options_visitor);
#define BUTTON(button)                                                                \
  static MakeObjectOption button##_down_option =                                      \
      MakeObjectOption(MAKE_PTR(MouseButtonEvent, ui::PointerButton::button, true));  \
  static MakeObjectOption button##_up_option =                                        \
      MakeObjectOption(MAKE_PTR(MouseButtonEvent, ui::PointerButton::button, false)); \
  static MakeObjectOption button##_presser_option =                                   \
      MakeObjectOption(MAKE_PTR(MouseButtonPresser, ui::PointerButton::button));      \
  options_visitor(button##_down_option);                                              \
  options_visitor(button##_up_option);                                                \
  options_visitor(button##_presser_option);
    BUTTON(Left);
    BUTTON(Middle);
    BUTTON(Right);
    BUTTON(Back);
    BUTTON(Forward);
#undef BUTTON
    static MakeObjectOption move_option = MakeObjectOption(MAKE_PTR(MouseMove));
    options_visitor(move_option);
  }

  static SkPath ButtonShape(ui::PointerButton btn) {
    switch (btn) {
      case ui::PointerButton::Left:
        return krita::mouse::Left();
      case ui::PointerButton::Middle:
        return krita::mouse::Middle();
      case ui::PointerButton::Right:
        return krita::mouse::Right();
      case ui::PointerButton::Back:
        return krita::mouse::Back();
      case ui::PointerButton::Forward:
        return krita::mouse::Forward();
      default:
        return SkPath();
    }
  }
};

struct MouseButtonEventWidget : MouseWidget {
  MouseButtonEventWidget(ui::Widget* parent, WeakPtr<Object>&& object)
      : MouseWidget(parent, std::move(object)) {}

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    // Note that we're not calling the MouseWidget's VisitOptions because we don't want to offer
    // other objects from here.
    WidgetBase::VisitOptions(options_visitor);
  }

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    Ptr<MouseButtonEvent> object = LockObject<MouseButtonEvent>();
    auto button = object->button;
    auto down = object->down;

    SkPath mask = ButtonShape(button);
    {  // Highlight the button
      SkPaint paint;
      paint.setBlendMode(SkBlendMode::kOverlay);
      paint.setColor(down ? "#f07a72"_color : "#1e74fd"_color);
      canvas.drawPath(mask, paint);
    }

    {  // Draw arrow
      SkPath path = PathFromSVG(kArrowShape);
      SkPaint paint;
      paint.setAlphaf(0.9f);
      paint.setImageFilter(
          SkImageFilters::DropShadow(0, 0, 0.5_mm, 0.5_mm, SK_ColorWHITE, nullptr));
      auto center = mask.getBounds().center();
      float scale = 1.3;
      if (button == ui::PointerButton::Middle) {
        center = Rect(mask.getBounds()).TopCenter();
        scale = 1.2;
      }
      canvas.translate(center.x(), center.y());
      if (down) {
        paint.setColor("#d0413c"_color);
      } else {
        paint.setColor("#1e74fd"_color);
        canvas.scale(1, -1);
      }
      canvas.scale(scale, scale);

      canvas.drawPath(path, paint);
    }
  }
};

static void SendMouseButtonEvent(ui::PointerButton button, bool down) {
#if defined(_WIN32)
  INPUT input;
  input.type = INPUT_MOUSE;
  input.mi.dx = 0;
  input.mi.dy = 0;
  input.mi.mouseData = 0;
  input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE;
  input.mi.time = 0;
  input.mi.dwExtraInfo = 0;
  switch (button) {
    case ui::PointerButton::Left:
      input.mi.dwFlags |= down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
      break;
    case ui::PointerButton::Middle:
      input.mi.dwFlags |= down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
      break;
    case ui::PointerButton::Right:
      input.mi.dwFlags |= down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
      break;
    default:
      return;
  }
  SendInput(1, &input, sizeof(INPUT));
#endif
#if defined(__linux__)
  U8 type = down ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
  U8 detail;
  switch (button) {
    case ui::PointerButton::Left:
      detail = 1;
      break;
    case ui::PointerButton::Middle:
      detail = 2;
      break;
    case ui::PointerButton::Right:
      detail = 3;
      break;
    case ui::PointerButton::Back:
      detail = 8;
      break;
    case ui::PointerButton::Forward:
      detail = 9;
      break;
    default:
      return;
  }
  xcb_test_fake_input(xcb::connection, type, detail, XCB_CURRENT_TIME, XCB_WINDOW_NONE, 0, 0, 0);
  xcb::flush();
#endif
}

string_view MouseButtonEvent::Name() const { return "Mouse Button Event"sv; }

Ptr<Object> MouseButtonEvent::Clone() const { return MAKE_PTR(MouseButtonEvent, button, down); }
void MouseButtonEvent::Args(std::function<void(Argument&)> cb) { cb(next_arg); }
void MouseButtonEvent::OnRun(Location&, RunTask&) {
  ZoneScopedN("MouseClick");
  SendMouseButtonEvent(button, down);
}
audio::Sound& MouseButtonEvent::NextSound() {
  return down ? embedded::assets_SFX_mouse_down_wav : embedded::assets_SFX_mouse_up_wav;
}

std::unique_ptr<Object::WidgetInterface> MouseButtonEvent::MakeWidget(ui::Widget* parent) {
  return std::make_unique<MouseButtonEventWidget>(parent, AcquireWeakPtr());
}

void MouseButtonEvent::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("button");
  writer.String(ButtonEnumToName(button));
  writer.Key("event");
  if (down) {
    writer.String("down");
  } else {
    writer.String("up");
  }
  writer.EndObject();
}
void MouseButtonEvent::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "button") {
      Str button_name;
      d.Get(button_name, status);
      button = ButtonNameToEnum(button_name);
    } else if (key == "event") {
      Str event_name;
      d.Get(event_name, status);
      if (event_name == "down") {
        down = true;
      } else if (event_name == "up") {
        down = false;
      } else {
        down = false;
        AppendErrorMessage(status) += "Unknown event name: " + event_name;
      }
    }
  }
  if (!OK(status)) {
    l.ReportError("Failed to deserialize MouseButtonEvent. " + status.ToStr());
  }
}

string_view MouseMove::Name() const { return "Mouse Move"; }

Ptr<Object> MouseMove::Clone() const { return MAKE_PTR(MouseMove); }

// A turtle with a pixelated cursor on its back
struct MouseMoveWidget : MouseWidget {
  constexpr static size_t kMaxTrailPoints = 256;
  std::atomic<int> trail_end_idx = 0;
  std::atomic<Vec2> trail[kMaxTrailPoints] = {};

  MouseMoveWidget(ui::Widget* parent, WeakPtr<MouseMove>&& weak_mouse_move)
      : MouseWidget(parent, std::move(weak_mouse_move)) {}

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    // Note that we're not calling the MouseWidget's VisitOptions because we don't want to offer
    // other objects from here.
    WidgetBase::VisitOptions(options_visitor);
  }

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    krita::mouse::dpad.draw(canvas);
    SkPath path;
    Vec2 cursor = {0, 0};
    SkPath dpad_window = krita::mouse::DpadWindow();
    Rect dpad_window_bounds = dpad_window.getBounds();
    float kDisplayRadius = dpad_window_bounds.Width() / 2;
    float trail_scale =
        kDisplayRadius / 15;  // initial scale shows at least 15 pixels (0 and 10 pixel axes)
    path.moveTo(cursor.x, cursor.y);
    int end = trail_end_idx.load(std::memory_order_relaxed);
    for (int i = end + kMaxTrailPoints - 1; i != end; --i) {
      Vec2 delta = trail[i % kMaxTrailPoints].load(std::memory_order_relaxed);
      cursor += delta;
      path.lineTo(-cursor.x, cursor.y);
      float cursor_dist = Length(cursor);
      float trail_scale_new = kDisplayRadius / cursor_dist;
      if (trail_scale_new < trail_scale) {
        trail_scale = trail_scale_new;
      }
    }
    canvas.translate(dpad_window_bounds.CenterX(),
                     dpad_window_bounds.CenterY());  // move the trail end to the center of display

    canvas.scale(trail_scale, trail_scale);

    auto matrix = canvas.getLocalToDeviceAs3x3();
    SkMatrix inverse;
    (void)matrix.invert(&inverse);

    SkVector dpd[2] = {SkVector(1, 0), SkVector(0, 1)};
    inverse.mapVectors(dpd);
    SkPaint display_paint;
    display_paint.setShader(mouse::GetPixelGridRuntimeEffect().makeShader(
        SkData::MakeWithCopy((void*)&dpd, sizeof(dpd)), nullptr, 0));

    canvas.drawCircle(0, 0, kDisplayRadius / trail_scale, display_paint);
    SkPaint trail_paint;
    trail_paint.setColor("#CCCCCC"_color);
    trail_paint.setStyle(SkPaint::kStroke_Style);
    if (dpd[0].x() < 1) {
      trail_paint.setStrokeWidth(1);
      trail_paint.setStrokeCap(SkPaint::kSquare_Cap);
      trail_paint.setStrokeJoin(SkPaint::kMiter_Join);
      trail_paint.setStrokeMiter(2);
    }
    canvas.drawPath(path, trail_paint);
  }
};

std::unique_ptr<Object::WidgetInterface> MouseMove::MakeWidget(ui::Widget* parent) {
  return std::make_unique<MouseMoveWidget>(parent, AcquireWeakPtr());
}

Vec2 mouse_move_accumulator;

void MouseMove::OnMouseMove(Vec2 vec) {
  mouse_move_accumulator += vec;
  vec = Vec2(truncf(mouse_move_accumulator.x), truncf(mouse_move_accumulator.y));
  mouse_move_accumulator -= vec;
#if defined(__linux__)
  if (vec.x != 0 || vec.y != 0) {
    xcb_test_fake_input(xcb::connection, XCB_MOTION_NOTIFY, true, XCB_CURRENT_TIME, XCB_WINDOW_NONE,
                        vec.x, vec.y, 0);
    xcb::flush();
  }
#endif
  ForEachWidget([vec](ui::RootWidget& root, ui::Widget& widget) {
    MouseMoveWidget& mouse_move_widget = static_cast<MouseMoveWidget&>(widget);
    int new_start = mouse_move_widget.trail_end_idx.fetch_add(1, std::memory_order_relaxed);
    int i = (new_start + MouseMoveWidget::kMaxTrailPoints - 1) % MouseMoveWidget::kMaxTrailPoints;
    mouse_move_widget.trail[i].store(vec, std::memory_order_relaxed);
    widget.WakeAnimation();
  });
}

std::unique_ptr<Object::WidgetInterface> Mouse::MakeWidget(ui::Widget* parent) {
  return std::make_unique<MouseWidget>(parent, AcquireWeakPtr());
}

Ptr<Object> Mouse::Clone() const { return MAKE_PTR(Mouse); }

struct PresserWidget : ui::Widget {
  PresserWidget(Object::WidgetBase* parent) : ui::Widget(parent) {}

  SkPath Shape() const override {
    static SkPath shape = krita::hand::Shape();
    return shape;
  }

  void Draw(SkCanvas& canvas) const override {
    bool is_on = false;
    if (parent) {
      auto parent_object_widget_base = static_cast<Object::WidgetBase*>(parent.Get());
      if (auto object_locked = parent_object_widget_base->LockObject<Object>()) {
        if (auto* long_running = object_locked->AsLongRunning()) {
          is_on = long_running->IsRunning();
        }
      }
    }
    (is_on ? krita::hand::pressing : krita::hand::pointing).draw(canvas);
  }
};

struct MouseButtonPresserWidget : MouseWidget {
  PresserWidget presser_widget;
  SkPath shape;

  MouseButtonPresserWidget(ui::Widget* parent, WeakPtr<Object>&& object)
      : MouseWidget(parent, std::move(object)), presser_widget(this) {
    SkPath mouse_shape = krita::mouse::Shape();

    ui::PointerButton button;
    {
      Ptr<MouseButtonPresser> object = LockObject<MouseButtonPresser>();
      button = object->button;
    }

    auto mask = ButtonShape(button);
    Rect bounds = mask.getBounds();
    Vec2 center = bounds.Center();
    presser_widget.local_to_parent = SkM44::Translate(center.x, center.y);

    auto presser_shape = presser_widget.Shape();
    presser_shape.transform(presser_widget.local_to_parent.asM33());

    Op(mouse_shape, presser_shape, kUnion_SkPathOp, &shape);
  }

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    // Note that we're not calling the MouseWidget's VisitOptions because we don't want to offer
    // other objects from here.
    WidgetBase::VisitOptions(options_visitor);
  }

  RRect CoarseBounds() const override { return RRect::MakeSimple(shape.getBounds(), 0); }

  Optional<Rect> TextureBounds() const override { return shape.getBounds(); }

  SkPath Shape() const override { return shape; }

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    ui::PointerButton button;
    {
      Ptr<MouseButtonPresser> object = LockObject<MouseButtonPresser>();
      button = object->button;
    }

    auto mask = ButtonShape(button);
    {  // Highlight the button in ivory
      SkPaint paint;
      paint.setBlendMode(SkBlendMode::kOverlay);
      paint.setColor("#9f8100"_color);
      canvas.drawPath(mask, paint);
    }

    DrawChildren(canvas);
  }

  void FillChildren(Vec<Widget*>& children) override { children.push_back(&presser_widget); }
};

MouseButtonPresser::MouseButtonPresser(ui::PointerButton button) : button(button) {}
MouseButtonPresser::MouseButtonPresser() : button(ui::PointerButton::Unknown) {}

string_view MouseButtonPresser::Name() const { return "Mouse Button Presser"sv; }

Ptr<Object> MouseButtonPresser::Clone() const { return MAKE_PTR(MouseButtonPresser, button); }

std::unique_ptr<Object::WidgetInterface> MouseButtonPresser::MakeWidget(ui::Widget* parent) {
  return std::make_unique<MouseButtonPresserWidget>(parent, AcquireWeakPtr());
}

void MouseButtonPresser::OnRun(Location& here, RunTask& run_task) {
  ZoneScopedN("MouseButtonPresser");
  audio::Play(embedded::assets_SFX_mouse_down_wav);
  SendMouseButtonEvent(button, true);
  BeginLongRunning(here, run_task);
  WakeWidgetsAnimation();
}

void MouseButtonPresser::OnCancel() {
  audio::Play(embedded::assets_SFX_mouse_up_wav);
  SendMouseButtonEvent(button, false);
  WakeWidgetsAnimation();
}

void MouseButtonPresser::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("button");
  writer.String(ButtonEnumToName(button));
  writer.EndObject();
}

void MouseButtonPresser::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "button") {
      Str button_name;
      d.Get(button_name, status);
      button = ButtonNameToEnum(button_name);
    }
  }
  if (!OK(status)) {
    l.ReportError("Failed to deserialize MouseButtonPresser. " + status.ToStr());
  }
}

MouseButtonPresser::~MouseButtonPresser() { OnLongRunningDestruct(); }

}  // namespace automat::library
