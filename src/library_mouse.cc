// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "library_mouse.hh"

#include <include/core/SkMatrix.h>
#include <include/core/SkShader.h>

#include <atomic>
#include <tracy/Tracy.hpp>

#include "textures.hh"

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__linux__)
#include <xcb/xproto.h>
#include <xcb/xtest.h>
#endif

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

constexpr float kTextureScale = 0.00005;

PersistentImage base_texture = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_base_webp, PersistentImage::MakeArgs{.scale = kTextureScale});

PersistentImage lmb_mask_texture = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_lmb_mask_webp, PersistentImage::MakeArgs{.scale = kTextureScale});

PersistentImage rmb_mask_texture = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_rmb_mask_webp, PersistentImage::MakeArgs{.scale = kTextureScale});

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

struct ObjectIcon : ui::Widget {
  std::unique_ptr<Object::WidgetBase> object_widget;
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
    auto matrix = TransformBetween(*pointer.hover, *root_machine);
    auto loc = MAKE_PTR(Location, root_machine.get(), root_location->AcquireWeakPtr());

    loc->Create(*proto);
    audio::Play(embedded::assets_SFX_toolbar_pick_wav);
    loc->animation_state.scale = matrix.get(0);
    auto contact_point = pointer.PositionWithin(*pointer.hover);
    loc->position = loc->animation_state.position =
        pointer.PositionWithinRootMachine() - contact_point;
    return std::make_unique<DragLocationAction>(pointer, std::move(loc), contact_point);
  }
};

struct MouseWidget : Object::WidgetBase {
  MouseWidget(ui::Widget* parent, WeakPtr<Object>&& object) : WidgetBase(parent) {
    this->object = std::move(object);
  }

  std::string_view Name() const override { return "Mouse"; }

  static float WidgetScale() {
    float texture_height = mouse::base_texture.height();
    float desired_height = 3_cm;
    return desired_height / texture_height;
  }

  Optional<Rect> TextureBounds() const override {
    float scale = WidgetScale();
    float width = mouse::base_texture.width();
    float height = mouse::base_texture.height();
    Rect bounds =
        Rect(-width / 2 * scale, -height / 2 * scale, width / 2 * scale, height / 2 * scale);
    return bounds;
  }

  SkPath Shape() const override {
    Rect bounds = *TextureBounds();
    float width = bounds.Width();
    SkRRect rrect;
    SkVector radii[4] = {
        {width / 2, width / 2},
        {width / 2, width / 2},
        {width / 3, width / 3},
        {width / 3, width / 3},
    };
    rrect.setRectRadii(bounds.sk, radii);
    return SkPath::RRect(rrect);
  }

  void Draw(SkCanvas& canvas) const override {
    auto bounds = *TextureBounds();
    canvas.translate(bounds.left, bounds.bottom);
    float scale = WidgetScale();
    canvas.scale(scale, scale);
    mouse::base_texture.draw(canvas);
  }

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    Object::WidgetBase::VisitOptions(options_visitor);
    static MakeObjectOption lmb_down_option = []() {
      return MakeObjectOption(MAKE_PTR(MouseButtonEvent, ui::PointerButton::Left, true));
    }();
    static MakeObjectOption lmb_up_option = []() {
      return MakeObjectOption(MAKE_PTR(MouseButtonEvent, ui::PointerButton::Left, false));
    }();
    static MakeObjectOption rmb_down_option = []() {
      return MakeObjectOption(MAKE_PTR(MouseButtonEvent, ui::PointerButton::Right, true));
    }();
    static MakeObjectOption rmb_up_option = []() {
      return MakeObjectOption(MAKE_PTR(MouseButtonEvent, ui::PointerButton::Right, false));
    }();
    static MakeObjectOption lmb_presser_option = []() {
      return MakeObjectOption(MAKE_PTR(MouseButtonPresser, ui::PointerButton::Left));
    }();
    static MakeObjectOption rmb_presser_option = []() {
      return MakeObjectOption(MAKE_PTR(MouseButtonPresser, ui::PointerButton::Right));
    }();
    options_visitor(lmb_down_option);
    options_visitor(lmb_up_option);
    options_visitor(rmb_down_option);
    options_visitor(rmb_up_option);
    options_visitor(lmb_presser_option);
    options_visitor(rmb_presser_option);
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
    MouseWidget::Draw(canvas);
    Ptr<MouseButtonEvent> object = LockObject<MouseButtonEvent>();
    auto button = object->button;
    auto down = object->down;

    auto& mask =
        button == ui::PointerButton::Left ? mouse::lmb_mask_texture : mouse::rmb_mask_texture;
    {  // Highlight the button
      SkPaint layer_paint;
      layer_paint.setBlendMode(SkBlendMode::kScreen);
      canvas.saveLayer(nullptr, &layer_paint);
      mouse::base_texture.draw(canvas);
      mask.paint.setBlendMode(SkBlendMode::kSrcIn);
      mask.draw(canvas);
      canvas.drawColor(down ? SK_ColorRED : SK_ColorCYAN, SkBlendMode::kSrcIn);
      canvas.restore();
    }

    {  // Draw arrow
      SkPath path = PathFromSVG(kArrowShape);
      SkPaint paint;
      paint.setBlendMode(SkBlendMode::kMultiply);
      paint.setAlphaf(0.9f);
      canvas.translate(button == ui::PointerButton::Left ? 3.7_mm : 14.3_mm, 24_mm);
      if (down) {
        paint.setColor(SkColorSetARGB(255, 255, 128, 128));
      } else {
        paint.setColor(SkColorSetARGB(255, 118, 235, 235));
        canvas.scale(1, -1);
      }
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
  U8 detail = button == ui::PointerButton::Left ? 1 : 3;
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

std::unique_ptr<Object::WidgetBase> MouseButtonEvent::MakeWidget(ui::Widget* parent) {
  return std::make_unique<MouseButtonEventWidget>(parent, AcquireWeakPtr());
}

void MouseButtonEvent::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("button");
  switch (button) {
    case ui::PointerButton::Left:
      writer.String("left");
      break;
    case ui::PointerButton::Right:
      writer.String("right");
      break;
    default:
      writer.String("unknown");
      break;
  }
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
      if (button_name == "left") {
        button = ui::PointerButton::Left;
      } else if (button_name == "right") {
        button = ui::PointerButton::Right;
      } else {
        AppendErrorMessage(status) += "Unknown button name: " + button_name;
        button = ui::PointerButton::Unknown;
      }
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

PersistentImage dpad_image = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_dpad_webp, PersistentImage::MakeArgs{.scale = mouse::kTextureScale});

// A turtle with a pixelated cursor on its back
struct MouseMoveWidget : Object::WidgetBase {
  constexpr static size_t kMaxTrailPoints = 256;
  std::atomic<int> trail_end_idx = 0;
  std::atomic<Vec2> trail[kMaxTrailPoints] = {};

  MouseMoveWidget(ui::Widget* parent, WeakPtr<MouseMove>&& weak_mouse_move) : WidgetBase(parent) {
    object = std::move(weak_mouse_move);
  }
  SkPath Shape() const override {
    Rect bounds = *TextureBounds();
    float width = bounds.Width();
    SkRRect rrect;
    SkVector radii[4] = {
        {width / 2, width / 2},
        {width / 2, width / 2},
        {width / 3, width / 3},
        {width / 3, width / 3},
    };
    rrect.setRectRadii(bounds.sk, radii);
    return SkPath::RRect(rrect);
  }
  void Draw(SkCanvas& canvas) const override {
    auto bounds = *TextureBounds();
    canvas.save();
    canvas.translate(bounds.left, bounds.bottom);
    float scale = WidgetScale();
    canvas.scale(scale, scale);
    mouse::base_texture.draw(canvas);
    dpad_image.draw(canvas);
    canvas.restore();
    canvas.save();
    SkPath path;
    Vec2 cursor = {0, 0};
    constexpr float kDisplayRadius = 1.6_mm;
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
    canvas.translate(-0.05_mm, -2.65_mm);  // move the trail end to the center of display

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

  // Mouse Move is supposed to be much smaller than regular mouse widget.
  //
  // This function returns the proper scaling factor.
  static float WidgetScale() {
    float texture_height = mouse::base_texture.height();
    float desired_height = 1.2_cm;
    return desired_height / texture_height;
  }

  Optional<Rect> TextureBounds() const override {
    float scale = WidgetScale();
    float width = mouse::base_texture.width();
    float height = mouse::base_texture.height();
    Rect bounds =
        Rect(-width / 2 * scale, -height / 2 * scale, width / 2 * scale, height / 2 * scale);
    return bounds;
  }

  void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override {
    Rect bounds = *TextureBounds();
    out_positions.push_back(Vec2AndDir{.pos = bounds.TopCenter(), .dir = -90_deg});
    out_positions.push_back(Vec2AndDir{.pos = bounds.LeftCenter(), .dir = 0_deg});
    out_positions.push_back(Vec2AndDir{.pos = bounds.RightCenter(), .dir = 180_deg});
  }
};

std::unique_ptr<Object::WidgetBase> MouseMove::MakeWidget(ui::Widget* parent) {
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

std::unique_ptr<Object::WidgetBase> Mouse::MakeWidget(ui::Widget* parent) {
  return std::make_unique<MouseWidget>(parent, AcquireWeakPtr());
}

Ptr<Object> Mouse::Clone() const { return MAKE_PTR(Mouse); }

struct MouseButtonPresserWidget : MouseWidget {
  MouseButtonPresserWidget(ui::Widget* parent, WeakPtr<Object>&& object)
      : MouseWidget(parent, std::move(object)) {}

  void VisitOptions(const OptionsVisitor& options_visitor) const override {
    // Note that we're not calling the MouseWidget's VisitOptions because we don't want to offer
    // other objects from here.
    WidgetBase::VisitOptions(options_visitor);
  }

  void Draw(SkCanvas& canvas) const override {
    MouseWidget::Draw(canvas);
    ui::PointerButton button;
    bool pressed;
    {
      Ptr<MouseButtonPresser> object = LockObject<MouseButtonPresser>();
      button = object->button;
      pressed = object->IsRunning();
    }

    auto& mask =
        button == ui::PointerButton::Left ? mouse::lmb_mask_texture : mouse::rmb_mask_texture;
    {  // Highlight the button in ivory
      SkPaint layer_paint;
      layer_paint.setBlendMode(SkBlendMode::kScreen);
      canvas.saveLayer(nullptr, &layer_paint);
      mouse::base_texture.draw(canvas);
      mask.paint.setBlendMode(SkBlendMode::kSrcIn);
      mask.draw(canvas);
      canvas.drawColor("#9f8100"_color, SkBlendMode::kSrcIn);  // Ivory color
      canvas.restore();
    }

    auto& img = pressed ? textures::PressingHandColor() : textures::PointingHandColor();
    if (button == ui::PointerButton::Left) {
      canvas.translate(0_mm, 16_mm);
    } else {
      canvas.translate(10_mm, 16_mm);
    }
    img.draw(canvas);
  }
};

MouseButtonPresser::MouseButtonPresser(ui::PointerButton button) : button(button) {}
MouseButtonPresser::MouseButtonPresser() : button(ui::PointerButton::Unknown) {}

string_view MouseButtonPresser::Name() const { return "Mouse Button Presser"sv; }

Ptr<Object> MouseButtonPresser::Clone() const { return MAKE_PTR(MouseButtonPresser, button); }

std::unique_ptr<Object::WidgetBase> MouseButtonPresser::MakeWidget(ui::Widget* parent) {
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
  switch (button) {
    case ui::PointerButton::Left:
      writer.String("left");
      break;
    case ui::PointerButton::Right:
      writer.String("right");
      break;
    default:
      writer.String("unknown");
      break;
  }
  writer.EndObject();
}

void MouseButtonPresser::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "button") {
      Str button_name;
      d.Get(button_name, status);
      if (button_name == "left") {
        button = ui::PointerButton::Left;
      } else if (button_name == "right") {
        button = ui::PointerButton::Right;
      } else {
        AppendErrorMessage(status) += "Unknown button name: " + button_name;
        button = ui::PointerButton::Unknown;
      }
    }
  }
  if (!OK(status)) {
    l.ReportError("Failed to deserialize MouseButtonPresser. " + status.ToStr());
  }
}

MouseButtonPresser::~MouseButtonPresser() { OnLongRunningDestruct(); }

}  // namespace automat::library