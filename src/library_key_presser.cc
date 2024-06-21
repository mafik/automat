
#include "library_key_presser.hh"

#include <include/core/SkSamplingOptions.h>
#include <include/pathops/SkPathOps.h>

#include <memory>

#include "../build/generated/embedded.hh"
#include "key_button.hh"
#include "keyboard.hh"
#include "library_macros.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"

namespace automat::library {

DEFINE_PROTO(KeyPresser);

static sk_sp<SkImage>& PointingHandColor() {
  static auto image =
      MakeImageFromAsset(embedded::assets_pointing_hand_color_webp)->withDefaultMipmaps();
  return image;
}

static sk_sp<SkImage>& PressingHandColor() {
  static auto image =
      MakeImageFromAsset(embedded::assets_pressing_hand_color_webp)->withDefaultMipmaps();
  return image;
}

constexpr static char kHandShapeSVG[] =
    "M9 19.9C7.9 20.1 7.9 19.2 8.4 18.6 7.9 17.1 5.9 16.3 5.3 14.8 3.7 11.4.7 10.2 1.1 9.3 1.2 8.9 "
    "2.2 6.6 7 10.9 7.8 10.4 6.5 1.2 7.8.4 9.1-.3 10.4 0 10.3 3.2L10.5 5.5C12 5.4 12.3 5.4 13.2 "
    "6.5 13.8 6.2 15 6.1 16 7.4 16.8 7 19.2 7.1 18.9 10.3L18.7 11 18.3 15.9 17.8 16.6 17.8 "
    "17.6C18.7 17.7 18.3 18.8 17.8 18.8L13 19.3Z";

static SkPath GetHandShape() {
  static SkPath path = []() {
    auto path = PathFromSVG(kHandShapeSVG);
    SkMatrix matrix = SkMatrix::I();
    float s = 1.67;
    matrix.preTranslate(-0.4_mm, 3_mm);
    matrix.preScale(s, s);
    path.transform(matrix);
    return path;
  }();
  return path;
}

float KeyPresserButton::PressRatio() const {
  if (key_presser->key_selector || key_presser->key_pressed) {
    return 1;
  }
  return 0;
}

KeyPresser::KeyPresser(gui::AnsiKey key)
    : key(key), shortcut_button(MakeKeyLabelWidget(ToStr(key)), KeyColor(false), kBaseKeyWidth) {
  shortcut_button.key_presser = this;
  shortcut_button.activate = [this](gui::Pointer& pointer) {
    if (key_selector) {
      key_selector->Release();
    } else {
      key_selector = &pointer.keyboard->RequestGrab(*this);
    }
  };
}
KeyPresser::KeyPresser() : KeyPresser(AnsiKey::F) {}
string_view KeyPresser::Name() const { return "Key Presser"; }
std::unique_ptr<Object> KeyPresser::Clone() const { return std::make_unique<KeyPresser>(key); }
void KeyPresser::Draw(gui::DrawContext& dctx) const {
  shortcut_button.fg = key_selector ? kKeyGrabbingColor : KeyColor(false);
  DrawChildren(dctx);
  auto& canvas = dctx.canvas;
  auto img = key_pressed ? PressingHandColor() : PointingHandColor();
  canvas.save();
  float s = shortcut_button.width / img->width();
  canvas.translate(0, 3_mm);
  canvas.scale(s, -s);
  canvas.drawImage(img.get(), 0, 0, kDefaultSamplingOptions);
  canvas.restore();
}
SkPath KeyPresser::Shape() const {
  auto button_shape = shortcut_button.Shape();
  auto hand_shape = GetHandShape();
  SkPath joined_shape;
  Op(button_shape, hand_shape, kUnion_SkPathOp, &joined_shape);
  return joined_shape;
}

void KeyPresser::ReleaseGrab(gui::KeyboardGrab&) { key_selector = nullptr; }
void KeyPresser::KeyboardGrabberKeyDown(gui::KeyboardGrab&, gui::Key k) {
  key_selector->Release();
  key = k.physical;
  dynamic_cast<LabelMixin*>(shortcut_button.Child())->SetLabel(ToStr(key));
}

ControlFlow KeyPresser::VisitChildren(gui::Visitor& visitor) {
  Widget* children[] = {&shortcut_button};
  return visitor(children);
}

SkMatrix KeyPresser::TransformToChild(const Widget& child, animation::Context&) const {
  return SkMatrix::I();
}

struct DragAndClickAction : Action {
  gui::Pointer& pointer;
  gui::PointerButton btn;
  std::unique_ptr<Action> drag_action;
  std::unique_ptr<Action> click_action;
  time::SystemPoint press_time;
  Vec2 press_pos;
  DragAndClickAction(gui::Pointer& pointer, gui::PointerButton btn,
                     std::unique_ptr<Action>&& drag_action, std::unique_ptr<Action>&& click_action)
      : pointer(pointer),
        btn(btn),
        drag_action(std::move(drag_action)),
        click_action(std::move(click_action)) {}
  void Begin(gui::Pointer& p) {
    press_pos = p.button_down_position[btn];
    press_time = p.button_down_time[btn];
    drag_action->Begin(p);
  }
  void Update(gui::Pointer& p) {
    if (drag_action) {
      drag_action->Update(p);
    }
  }
  void End() {
    if (click_action && (time::SystemNow() - press_time < 0.2s)) {
      gui::Pointer fake_pointer(pointer.window, press_pos);
      click_action->Begin(fake_pointer);
      click_action->End();
    }
    if (drag_action) {
      drag_action->End();
    }
  }
  void DrawAction(gui::DrawContext& dctx) {}
};

struct RunAction : Action {
  Location& location;
  RunAction(Location& location) : location(location) {}
  void Begin(gui::Pointer& p) {
    if (location.long_running) {
      location.long_running->Cancel();
      location.long_running = nullptr;
    } else {
      location.ScheduleRun();
    }
  }
  void Update(gui::Pointer& p) {}
  void End() {}
  void DrawAction(gui::DrawContext& dctx) {}
};

std::unique_ptr<Action> KeyPresser::CaptureButtonDownAction(gui::Pointer& p,
                                                            gui::PointerButton btn) {
  if (btn != gui::kMouseLeft) return nullptr;
  auto hand_shape = GetHandShape();
  auto local_pos = p.PositionWithin(*this);
  if (hand_shape.contains(local_pos.x, local_pos.y)) {
    return std::make_unique<DragAndClickAction>(
        p, btn, Object::ButtonDownAction(p, btn),
        std::make_unique<RunAction>(*Closest<Location>(p.path)));
  } else {
    return std::make_unique<DragAndClickAction>(p, btn, Object::ButtonDownAction(p, btn),
                                                shortcut_button.ButtonDownAction(p, btn));
  }
}

LongRunning* KeyPresser::OnRun(Location& here) {
  SendKeyEvent(key, true);
  key_pressed = true;
  return this;
}

void KeyPresser::Cancel() {
  SendKeyEvent(key, false);
  key_pressed = false;
}

}  // namespace automat::library