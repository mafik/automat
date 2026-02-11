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
#include "menu.hh"
#include "optional.hh"

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__linux__)
#include <xcb/xinput.h>
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
#include "root_widget.hh"
#include "svg.hh"
#include "textures.hh"
#include "widget.hh"

#if defined(__linux__)
#include "xcb.hh"
#endif

namespace automat::library::mouse {
SkRuntimeEffect& GetPixelGridRuntimeEffect() {
  static const auto runtime_effect = [] {
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

struct MakeObjectOption : Option {
  Ptr<Object> proto;
  Dir dir;
  TrackedPtr<ui::Widget> icon;
  MakeObjectOption(Ptr<Object> proto, Dir dir = DIR_NONE) : proto(proto), dir(dir), icon(nullptr) {}
  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override {
    auto new_icon = proto->MakeToy(parent);
    icon = new_icon.get();
    return new_icon;
  }
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MakeObjectOption>(proto, dir);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    // Idea: reuse the `icon` widget.
    // The icon is the right widget type for the given proto, so theoretically it could be
    // reattached to the newly cloned object.
    // This could be even handled by the "Create" method - it could take existing widget to "adopt".
    auto loc = MAKE_PTR(Location, root_location);
    auto obj = proto->Clone();
    pointer.root_widget.toys.FindOrMake(*obj, icon.get());
    loc->InsertHere(std::move(obj));
    audio::Play(embedded::assets_SFX_toolbar_pick_wav);
    auto action = std::make_unique<DragLocationAction>(pointer, std::move(loc));
    // Resetting the anchor makes the object dragged by the center point
    if (action->locations.front()->widget)
      action->locations.front()->widget->local_anchor = Vec2(0, 0);
    return action;
  }
  Dir PreferredDir() const override { return dir; }
};

static float FindLoD(const SkMatrix& ctm, float local_x, float min_x_px, float max_x_px) {
  float device_height_px = ctm.mapRadius(krita::mouse::base.height());
  float lod = GetRatio(device_height_px, 40, 80);
  return CosineInterpolate(0, 1, lod);
}

struct PresserWidget : ui::Widget {
  bool is_on = false;

  using ui::Widget::Widget;

  SkPath Shape() const override {
    static SkPath shape = krita::hand::Shape();
    return shape;
  }

  void Draw(SkCanvas& canvas) const override {
    (is_on ? krita::hand::pressing : krita::hand::pointing).draw(canvas);
  }
};

struct MouseWidgetCommon {
  static RRect CoarseBounds() { return RRect::MakeSimple(krita::mouse::base.rect, 0); }

  static Optional<Rect> TextureBounds() { return krita::mouse::base.rect; }

  static SkPath Shape() {
    static auto shape = krita::mouse::Shape();
    return shape;
  }

  // size_ratio at 1 means normal size, 0 means iconified
  static SkMatrix GetPresserMatrix(Vec2 button_center, float size_ratio) {
    SkMatrix presser_widget_normal = SkMatrix::Translate(button_center);
    SkMatrix presser_widget_iconified = SkMatrix::Scale(2, 2).postTranslate(-3_mm, 5_mm);
    return MatrixMix(presser_widget_iconified, presser_widget_normal, size_ratio);
  }

  static animation::Phase Tick(time::Timer& timer, ui::Widget& widget, ui::PointerButton button,
                               Optional<bool> down_opt, PresserWidget* presser_widget) {
    SkPath mask = ButtonShape(button);
    float lod = FindLoD(TransformUp(widget), krita::mouse::base.height(), 40, 80);

    if (mask.isEmpty()) {
      lod = 0;
    }

    if (presser_widget) {
      auto transform_mix = GetPresserMatrix(mask.getBounds().center(), lod);
      presser_widget->local_to_parent = SkM44(transform_mix);
      presser_widget->WakeAnimation();
    }
    return animation::Finished;
  }

  static void Draw(SkCanvas& canvas, ui::PointerButton button, Optional<bool> down_opt,
                   PresserWidget* presser_widget, bool scroll) {
    krita::mouse::base.draw(canvas);
    if (scroll) {
      krita::mouse::large_wheel.draw(canvas);
    }
    SkPath mask = ButtonShape(button);
    float lod = FindLoD(canvas.getLocalToDeviceAs3x3(), krita::mouse::base.height(), 40, 80);
    ;

    if (mask.isEmpty()) {
      lod = 0;
    } else {
      {  // Highlight the button
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kOverlay);
        SkColor overlay_color = "#9f8100"_color;  // ivory by default
        if (down_opt.has_value()) {
          overlay_color =
              *down_opt ? "#f07a72"_color : "#1e74fd"_color;  // red & blue for down & up
        }
        paint.setColor(overlay_color);
        canvas.drawPath(mask, paint);
      }
    }

    if (button == ui::PointerButton::Unknown) {
      krita::mouse::head.draw(canvas);
    }

    if (down_opt.has_value()) {
      bool down = *down_opt;

      {  // Draw arrow
        SkPath path = PathFromSVG(kArrowShape);
        SkPaint paint;
        paint.setAlphaf(0.9f);
        paint.setImageFilter(
            SkImageFilters::DropShadow(0, 0, 0.5_mm, 0.5_mm, SK_ColorWHITE, nullptr));
        Vec2 center = mask.getBounds().center();

        SkMatrix transformSmall = SkMatrix::Translate(center.x, center.y);

        if (button == ui::PointerButton::Middle || button == ui::PointerButton::Back ||
            button == ui::PointerButton::Forward) {
          transformSmall.postTranslate(0, path.getBounds().bottom());
          transformSmall.preScale(1.2, 1.2);
        } else {
          transformSmall.preScale(1.3, 1.3);
        }
        if (!down) {
          transformSmall.preScale(1, -1);
        }

        SkMatrix transformLarge =
            SkMatrix::Translate(Rect(krita::mouse::DpadWindow().getBounds()).TopCenter());
        transformLarge.preScale(3, 3);
        if (!down) {
          transformLarge.preScale(1, -1);
        }

        auto transform_mix = MatrixMix(transformLarge, transformSmall, lod);

        if (down) {
          paint.setColor("#d0413c"_color);
        } else {
          paint.setColor("#1e74fd"_color);
        }

        canvas.concat(transform_mix);
        canvas.drawPath(path, paint);
      }
    }
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

struct MouseIcon : ui::Widget {
  ui::PointerButton button;
  Optional<bool> down;
  std::unique_ptr<PresserWidget> presser_widget;
  bool scroll;

  MouseIcon(ui::Widget* parent, ui::PointerButton button, Optional<bool> down, bool presser,
            bool scroll = false)
      : ui::Widget(parent), button(button), down(down), presser_widget(nullptr), scroll(scroll) {
    if (presser) {
      presser_widget.reset(new PresserWidget(this));
    }
  }

  RRect CoarseBounds() const override { return MouseWidgetCommon::CoarseBounds(); }

  Optional<Rect> TextureBounds() const override { return MouseWidgetCommon::TextureBounds(); }

  animation::Phase Tick(time::Timer& timer) override {
    return MouseWidgetCommon::Tick(timer, *this, button, std::nullopt, presser_widget.get());
  }

  void TransformUpdated() override { WakeAnimation(); }

  SkPath Shape() const override { return MouseWidgetCommon::Shape(); }

  void Draw(SkCanvas& canvas) const override {
    MouseWidgetCommon::Draw(canvas, ui::PointerButton::Unknown, down, presser_widget.get(), scroll);
    DrawChildren(canvas);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (presser_widget) children.push_back(presser_widget.get());
  }
};

struct MouseDownMenuOption : Option, OptionsProvider {
  bool down;
  mutable std::vector<MakeObjectOption> button_options;
  MouseDownMenuOption(bool down) : down(down) {
    auto AddButtonOption = [&](ui::PointerButton btn, Dir dir) {
      button_options.emplace_back(MAKE_PTR(MouseButtonEvent, btn, down), dir);
    };
    using enum ui::PointerButton;
    AddButtonOption(Left, SW);
    AddButtonOption(Middle, S);
    AddButtonOption(Right, SE);
    AddButtonOption(Back, NW);
    AddButtonOption(Forward, NE);
  }

  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override {
    return std::make_unique<MouseIcon>(parent, ui::PointerButton::Unknown, down, false);
  }
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MouseDownMenuOption>(down);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    for (auto& option : button_options) {
      visitor(option);
    }
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
  Dir PreferredDir() const override { return down ? SW : SE; }
};

struct MousePresserMenuOption : Option, OptionsProvider {
  MousePresserMenuOption() {}

  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override {
    return std::make_unique<MouseIcon>(parent, ui::PointerButton::Unknown, std::nullopt, true);
  }
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MousePresserMenuOption>();
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
#define BUTTON(button, dir)                                                           \
  static MakeObjectOption button##_presser_option =                                   \
      MakeObjectOption(MAKE_PTR(MouseButtonPresser, ui::PointerButton::button), dir); \
  visitor(button##_presser_option);
    BUTTON(Left, SW);
    BUTTON(Middle, S);
    BUTTON(Right, SE);
    BUTTON(Back, NW);
    BUTTON(Forward, NE);
#undef BUTTON
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
  Dir PreferredDir() const override { return S; }
};

struct MouseScrollMenuOption : Option, OptionsProvider {
  MouseScrollMenuOption() {}

  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) override {
    return std::make_unique<MouseIcon>(parent, ui::PointerButton::Unknown, std::nullopt, false,
                                       true);
  }
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<MouseScrollMenuOption>();
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    static MakeObjectOption x = MakeObjectOption(MAKE_PTR(MouseScrollX), Option::N);
    visitor(x);
    static MakeObjectOption y = MakeObjectOption(MAKE_PTR(MouseScrollY), Option::S);
    visitor(y);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
  Dir PreferredDir() const override { return E; }
};

struct MouseWidgetBase : Object::Toy {
  MouseWidgetBase(ui::Widget* parent, Object& object) : Object::Toy(parent, object) {}

  RRect CoarseBounds() const override { return MouseWidgetCommon::CoarseBounds(); }

  Optional<Rect> TextureBounds() const override { return MouseWidgetCommon::TextureBounds(); }

  SkPath Shape() const override { return MouseWidgetCommon::Shape(); }
};

struct MouseWidget : MouseWidgetBase {
  using MouseWidgetBase::MouseWidgetBase;

  std::string_view Name() const override { return "Mouse"; }

  void Draw(SkCanvas& canvas) const override {
    MouseWidgetCommon::Draw(canvas, ui::PointerButton::Unknown, std::nullopt, nullptr, false);
  }

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    Object::Toy::VisitOptions(options_visitor);
    static MousePresserMenuOption presser_option;
    options_visitor(presser_option);
    static MouseDownMenuOption down_option(true);
    options_visitor(down_option);
    static MouseDownMenuOption up_option(false);
    options_visitor(up_option);
    static MakeObjectOption move_option = MakeObjectOption(MAKE_PTR(MouseMove), Option::W);
    options_visitor(move_option);
    static MouseScrollMenuOption scroll_option;
    options_visitor(scroll_option);
  }
};

struct MouseButtonEventWidget : MouseWidgetBase {
  using MouseWidgetBase::MouseWidgetBase;

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    Ptr<MouseButtonEvent> object = LockObject<MouseButtonEvent>();
    auto button = object->button;
    auto down = object->down;
    MouseWidgetCommon::Draw(canvas, button, down, nullptr, false);
  }
};

#if defined(_WIN32)
static void WIN32_SendMouseInput(int32_t dx, int32_t dy, uint32_t mouseData, uint32_t dwFlags) {
  INPUT input;
  input.type = INPUT_MOUSE;
  input.mi.dx = dx;
  input.mi.dy = dy;
  input.mi.mouseData = mouseData;
  input.mi.dwFlags = dwFlags;
  input.mi.time = 0;
  input.mi.dwExtraInfo = 0;
  SendInput(1, &input, sizeof(INPUT));
}
#endif

static void SendMouseButtonEvent(ui::PointerButton button, bool down) {
  using enum ui::PointerButton;
#if defined(_WIN32)
  switch (button) {
    case Left:
      WIN32_SendMouseInput(
          0, 0, 0, MOUSEEVENTF_ABSOLUTE | (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP));
      break;
    case Middle:
      WIN32_SendMouseInput(
          0, 0, 0, MOUSEEVENTF_ABSOLUTE | (down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP));
      break;
    case Right:
      WIN32_SendMouseInput(
          0, 0, 0, MOUSEEVENTF_ABSOLUTE | (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP));
      break;
    case Back:
      WIN32_SendMouseInput(0, 0, XBUTTON1,
                           MOUSEEVENTF_ABSOLUTE | (down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP));
      break;
    case Forward:
      WIN32_SendMouseInput(0, 0, XBUTTON2,
                           MOUSEEVENTF_ABSOLUTE | (down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP));
      break;
    default:
      return;
  }
#endif
#if defined(__linux__)
  U8 type = down ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
  U8 detail;
  switch (button) {
    case Left:
      detail = 1;
      break;
    case Middle:
      detail = 2;
      break;
    case Right:
      detail = 3;
      break;
    case Back:
      detail = 8;
      break;
    case Forward:
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
void MouseButtonEvent::Atoms(const std::function<LoopControl(Atom&)>& cb) {
  if (LoopControl::Break == cb(runnable)) return;
  if (LoopControl::Break == cb(next_arg)) return;
}
void MouseButtonEvent::MyRunnable::OnRun(std::unique_ptr<RunTask>&) {
  auto& event = MouseButtonEvent();
  ZoneScopedN("MouseClick");
  SendMouseButtonEvent(event.button, event.down);
}
audio::Sound& MouseButtonEvent::NextSound() {
  return down ? embedded::assets_SFX_mouse_down_wav : embedded::assets_SFX_mouse_up_wav;
}

std::unique_ptr<Object::Toy> MouseButtonEvent::MakeToy(ui::Widget* parent) {
  return std::make_unique<MouseButtonEventWidget>(parent, *this);
}

void MouseButtonEvent::SerializeState(ObjectSerializer& writer) const {
  writer.Key("button");
  writer.String(ButtonEnumToName(button));
  writer.Key("event");
  if (down) {
    writer.String("down");
  } else {
    writer.String("up");
  }
}
bool MouseButtonEvent::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
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
  } else {
    return false;
  }
  if (!OK(status)) {
    ReportError("Failed to deserialize MouseButtonEvent. " + status.ToStr());
  }
  return true;
}

string_view MouseMove::Name() const { return "Mouse Move"; }

Ptr<Object> MouseMove::Clone() const { return MAKE_PTR(MouseMove); }

// A turtle with a pixelated cursor on its back
struct MouseMoveWidget : MouseWidget {
  constexpr static size_t kMaxTrailPoints = 256;
  std::atomic<int> trail_end_idx = 0;
  std::atomic<Vec2> trail[kMaxTrailPoints] = {};

  MouseMoveWidget(ui::Widget* parent, Object& weak_mouse_move)
      : MouseWidget(parent, weak_mouse_move) {}

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    // Note that we're not calling the MouseWidget's VisitOptions because we don't want to offer
    // other objects from here.
    Object::Toy::VisitOptions(options_visitor);
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

std::unique_ptr<Object::Toy> MouseMove::MakeToy(ui::Widget* parent) {
  return std::make_unique<MouseMoveWidget>(parent, *this);
}

Vec2 mouse_move_accumulator;

void MouseMove::OnMouseMove(Vec2 vec) {
  mouse_move_accumulator += vec;
  vec = Vec2(truncf(mouse_move_accumulator.x), truncf(mouse_move_accumulator.y));
  mouse_move_accumulator -= vec;
#if defined(_WIN32)
  if (vec.x != 0 || vec.y != 0) {
    WIN32_SendMouseInput((LONG)vec.x, (LONG)vec.y, 0, MOUSEEVENTF_MOVE);
  }
#elif defined(__linux__)
  if (vec.x != 0 || vec.y != 0) {
    xcb_test_fake_input(xcb::connection, XCB_MOTION_NOTIFY, true, XCB_CURRENT_TIME, XCB_WINDOW_NONE,
                        vec.x, vec.y, 0);
    xcb::flush();
  }
#endif
  ForEachToy([vec](ui::RootWidget& root, ui::Widget& widget) {
    MouseMoveWidget& mouse_move_widget = static_cast<MouseMoveWidget&>(widget);
    int new_start = mouse_move_widget.trail_end_idx.fetch_add(1, std::memory_order_relaxed);
    int i = (new_start + MouseMoveWidget::kMaxTrailPoints - 1) % MouseMoveWidget::kMaxTrailPoints;
    mouse_move_widget.trail[i].store(vec, std::memory_order_relaxed);
    widget.WakeAnimation();
  });
}

string_view MouseScrollY::Name() const { return "Scroll Y"; }
Ptr<Object> MouseScrollY::Clone() const { return MAKE_PTR(MouseScrollY); }

string_view MouseScrollX::Name() const { return "Scroll X"; }
Ptr<Object> MouseScrollX::Clone() const { return MAKE_PTR(MouseScrollX); }

struct MouseScrollYWidget : MouseWidgetBase {
  MouseScrollYWidget(ui::Widget* parent, Object& obj) : MouseWidgetBase(parent, obj) {}

  animation::SpringV2<SinCos> rotation;

  animation::Phase Tick(time::Timer& t) override {
    auto phase = animation::Finished;

    auto target = this->LockObject<MouseScrollY>()->rotation;
    phase |= rotation.SineTowards(target, t.d, 0.6);

    return phase;
  }

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    krita::mouse::large_wheel.draw(canvas);
    auto wheel_shape = krita::mouse::Wheel();

    auto wheel_bounds = wheel_shape.getBounds();

    SkPaint paint;
    paint.setColor("#0b9e0e"_color);
    paint.setBlendMode(SkBlendMode::kColorBurn);

    canvas.drawPath(krita::mouse::ScrollY(), paint);

    canvas.save();
    canvas.clipPath(wheel_shape);

    Rect rect = Rect::MakeCenter(wheel_bounds.center(), wheel_bounds.width() * 2,
                                 wheel_bounds.height() / 5);

    float cy = wheel_bounds.centerY();
    float r = wheel_bounds.height() / 2;
    auto alpha = rotation.value;

    float c = 1.5 * r;
    Vec2 left = wheel_bounds.center() - Vec2(c, 0);
    Vec2 right = wheel_bounds.center() + Vec2(c, 0);

    // Note: This code is copied in MouseScrollXWidget.
    // If you're doing any changes, make sure that both copies are in sync.
    for (int i = 0; i < 12; ++i) {
      if ((alpha + 7.5_deg).cos > (alpha - 7.5_deg).cos) {
        // a0 and a1 are the distances along Y axis where the arcs cross the center line
        float a0 = r * (float)(alpha + 7.5_deg).cos;
        float a1 = r * (float)(alpha - 7.5_deg).cos;
        // r0 and r1 are the radii of the circles
        float r0 = a0 + (c * c - a0 * a0) / 2 / a0;
        float r1 = a1 + (c * c - a1 * a1) / 2 / a1;
        // x0 and x1 are the distances along Y axis where the arc control point lies
        float x0 = 2 * a0 * c * c / (c * c - a0 * a0);
        float x1 = 2 * a1 * c * c / (c * c - a1 * a1);
        // s0 and s1 are the arc control points
        Vec2 s0 = Vec2(wheel_bounds.centerX(), cy + x0);
        Vec2 s1 = Vec2(wheel_bounds.centerX(), cy + x1);
        SkPath path;
        path.moveTo(left);
        path.arcTo(s0, right, r0);
        path.arcTo(s1, left, r1);
        canvas.drawPath(path, paint);
      }
      alpha = alpha + 30_deg;
    }

    canvas.restore();
  }
};

struct MouseScrollXWidget : MouseWidgetBase {
  MouseScrollXWidget(ui::Widget* parent, Object& obj) : MouseWidgetBase(parent, obj) {}

  animation::SpringV2<SinCos> rotation;

  animation::Phase Tick(time::Timer& t) override {
    auto phase = animation::Finished;

    auto target = this->LockObject<MouseScrollY>()->rotation;
    phase |= rotation.SineTowards(target, t.d, 0.6);

    return phase;
  }

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    krita::mouse::large_wheel.draw(canvas);
    auto wheel_shape = krita::mouse::Wheel();

    auto wheel_bounds = wheel_shape.getBounds();

    SkPaint paint;
    paint.setColor("#bf220d"_color);
    paint.setBlendMode(SkBlendMode::kColorBurn);

    canvas.drawPath(krita::mouse::ScrollX(), paint);

    canvas.save();
    canvas.clipPath(wheel_shape);

    Rect rect = Rect::MakeCenter(wheel_bounds.center(), wheel_bounds.width() * 2,
                                 wheel_bounds.height() / 5);

    float cx = wheel_bounds.centerX();
    float r = wheel_bounds.height() / 2;
    auto alpha = rotation.value;

    float c = 2 * r;
    Vec2 bottom = wheel_bounds.center() - Vec2(0, c);
    Vec2 top = wheel_bounds.center() + Vec2(0, c);

    for (int i = 0; i < 12; ++i) {
      if ((alpha + 7.5_deg).cos > (alpha - 7.5_deg).cos) {
        // The math to draw crescent is taken from MouseScrollYWidget.
        // `left` & `right` have been swapped to `bottom` & `top`.
        // If you're doing any changes, make sure that both copies are in sync.
        float a0 = r * (float)(alpha + 7.5_deg).cos;
        float a1 = r * (float)(alpha - 7.5_deg).cos;
        float r0 = a0 + (c * c - a0 * a0) / 2 / a0;
        float r1 = a1 + (c * c - a1 * a1) / 2 / a1;
        float x0 = 2 * a0 * c * c / (c * c - a0 * a0);
        float x1 = 2 * a1 * c * c / (c * c - a1 * a1);
        Vec2 s0 = Vec2(cx + x0, wheel_bounds.centerY());
        Vec2 s1 = Vec2(cx + x1, wheel_bounds.centerY());
        SkPath path;
        path.moveTo(bottom);
        path.arcTo(s0, top, r0);
        path.arcTo(s1, bottom, r1);
        canvas.drawPath(path, paint);
      }
      alpha = alpha + 30_deg;
    }

    canvas.restore();
  }
};

std::unique_ptr<Object::Toy> MouseScrollY::MakeToy(ui::Widget* parent) {
  return std::make_unique<MouseScrollYWidget>(parent, *this);
}

std::unique_ptr<Object::Toy> MouseScrollX::MakeToy(ui::Widget* parent) {
  return std::make_unique<MouseScrollXWidget>(parent, *this);
}

void MouseScrollY::OnRelativeFloat64(double delta) {
#if defined(_WIN32)
  WIN32_SendMouseInput(0, 0, round(delta * WHEEL_DELTA), MOUSEEVENTF_WHEEL);
#elif defined(__linux__)
  for (auto type : {XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE}) {
    xcb_test_fake_input(xcb::connection, type, delta > 0 ? 4 : 5, XCB_CURRENT_TIME, XCB_WINDOW_NONE,
                        0, 0, 0);
  }
  xcb::flush();
#endif
  rotation = rotation + SinCos::FromDegrees(delta * 15);
  WakeToys();
}
void MouseScrollX::OnRelativeFloat64(double delta) {
#if defined(_WIN32)
  WIN32_SendMouseInput(0, 0, round(delta * WHEEL_DELTA), MOUSEEVENTF_HWHEEL);
#elif defined(__linux__)
  for (auto type : {XCB_BUTTON_PRESS, XCB_BUTTON_RELEASE}) {
    xcb_test_fake_input(xcb::connection, type, delta > 0 ? 6 : 7, XCB_CURRENT_TIME, XCB_WINDOW_NONE,
                        0, 0, 0);
  }
  xcb::flush();
#endif
  rotation = rotation + SinCos::FromDegrees(delta * 15);
  WakeToys();
}

std::unique_ptr<Object::Toy> Mouse::MakeToy(ui::Widget* parent) {
  return std::make_unique<MouseWidget>(parent, *this);
}

Ptr<Object> Mouse::Clone() const { return MAKE_PTR(Mouse); }

struct MouseButtonPresserWidget : MouseWidgetBase {
  mutable PresserWidget presser_widget;
  SkPath shape;
  ui::PointerButton button;

  MouseButtonPresserWidget(ui::Widget* parent, MouseButtonPresser& object)
      : MouseWidgetBase(parent, object), presser_widget(this) {
    SkPath mouse_shape = krita::mouse::Shape();

    button = object.button;
    presser_widget.is_on = object.state.IsOn();

    auto mask = MouseWidgetCommon::ButtonShape(button);
    Rect bounds = mask.getBounds();
    Vec2 center = bounds.Center();

    presser_widget.local_to_parent = SkM44(MouseWidgetCommon::GetPresserMatrix(center, 1));

    auto presser_shape = presser_widget.Shape();
    presser_shape.transform(presser_widget.local_to_parent.asM33());
    Op(mouse_shape, presser_shape, kUnion_SkPathOp, &shape);
  }

  RRect CoarseBounds() const override { return RRect::MakeSimple(shape.getBounds(), 0); }

  Optional<Rect> TextureBounds() const override { return shape.getBounds(); }

  SkPath Shape() const override { return shape; }

  animation::Phase Tick(time::Timer& timer) override {
    {
      Ptr<MouseButtonPresser> object = LockObject<MouseButtonPresser>();
      button = object->button;
      presser_widget.is_on = object->state.IsOn();
    }
    return MouseWidgetCommon::Tick(timer, *this, button, std::nullopt, &presser_widget);
  }

  void TransformUpdated() override { WakeAnimation(); }

  void Draw(SkCanvas& canvas) const override {
    krita::mouse::base.draw(canvas);
    MouseWidgetCommon::Draw(canvas, button, std::nullopt, &presser_widget, false);
    DrawChildren(canvas);
  }

  void FillChildren(Vec<Widget*>& children) override { children.push_back(&presser_widget); }
};

MouseButtonPresser::MouseButtonPresser(ui::PointerButton button) : button(button) {}
MouseButtonPresser::MouseButtonPresser() : button(ui::PointerButton::Unknown) {}

string_view MouseButtonPresser::Name() const { return "Mouse Button Presser"sv; }

Ptr<Object> MouseButtonPresser::Clone() const { return MAKE_PTR(MouseButtonPresser, button); }

void MouseButtonPresser::Atoms(const std::function<LoopControl(Atom&)>& cb) {
  if (LoopControl::Break == cb(next_arg)) return;
  if (LoopControl::Break == cb(click)) return;
  if (LoopControl::Break == cb(state)) return;
}

std::unique_ptr<Object::Toy> MouseButtonPresser::MakeToy(ui::Widget* parent) {
  return std::make_unique<MouseButtonPresserWidget>(parent, *this);
}

void MouseButtonPresser::Click::OnRun(std::unique_ptr<RunTask>& run_task) {
  ZoneScopedN("MouseButtonPresser");
  audio::Play(embedded::assets_SFX_mouse_down_wav);
  auto& presser = MouseButtonPresser();
  SendMouseButtonEvent(presser.button, true);
  SendMouseButtonEvent(presser.button, false);
}

void MouseButtonPresser::State::OnTurnOn() {
  audio::Play(embedded::assets_SFX_mouse_down_wav);
  auto& presser = MouseButtonPresser();
  SendMouseButtonEvent(presser.button, true);
  presser.WakeToys();
}
void MouseButtonPresser::State::OnTurnOff() {
  audio::Play(embedded::assets_SFX_mouse_up_wav);
  auto& presser = MouseButtonPresser();
  SendMouseButtonEvent(presser.button, false);
  presser.WakeToys();
}

void MouseButtonPresser::SerializeState(ObjectSerializer& writer) const {
  writer.Key("button");
  writer.String(ButtonEnumToName(button));
}

bool MouseButtonPresser::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "button") {
    Status status;
    Str button_name;
    d.Get(button_name, status);
    button = ButtonNameToEnum(button_name);
    if (!OK(status)) {
      ReportError("Failed to deserialize MouseButtonPresser. " + status.ToStr());
    }
    return true;
  }
  return false;
}

MouseButtonPresser::~MouseButtonPresser() {
  if (state.IsOn()) {
    state.OnTurnOff();
  }
}

}  // namespace automat::library
