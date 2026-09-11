// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_key_presser.hpp"

#include <include/core/SkSamplingOptions.h>
#include <include/pathops/SkPathOps.h>

#include <memory>
#include <tracy/Tracy.hpp>

#include "../build/generated/embedded.hpp"
#include "audio.hpp"
#include "key_button.hpp"
#include "keyboard.hpp"
#include "root_widget.hpp"
#include "sincos.hpp"
#include "status.hpp"
#include "svg.hpp"
#include "textures.hpp"
#include "time.hpp"

using namespace std;
using namespace automat;

namespace automat::library {

void KeyPresser::StartMonitoring() {
  if (keylogging == nullptr) {
    ui::root_widget->window->BeginLogging(this, &keylogging, nullptr, nullptr);
  }
}

void KeyPresser::StopMonitoring() {
  if (keylogging) {
    keylogging->Release();
  }
}

void KeyPresser::Press() {
  audio::Play(embedded::assets_SFX_key_down_wav);
  if (key_pressed) return;
  key_pressed = true;
  last_pressed_time = time::SteadyNow();
  ui::Keyboard::SendKeyEvent(key, true);
  WakeToys();
}
void KeyPresser::Release() {
  audio::Play(embedded::assets_SFX_key_up_wav);
  if (!key_pressed) return;
  key_pressed = false;
  last_released_time = time::SteadyNow();
  ui::Keyboard::SendKeyEvent(key, false);
  WakeToys();
}

constexpr static char kHandShapeSVG[] =
    "M9 19.9C7.9 20.1 7.9 19.2 8.4 18.6 7.9 17.1 5.9 16.3 5.3 14.8 3.7 11.4.7 10.2 1.1 9.3 1.2 8.9 "
    "2.2 6.6 7 10.9 7.8 10.4 6.5 1.2 7.8.4 9.1-.3 10.4 0 10.3 3.2L10.5 5.5C12 5.4 12.3 5.4 13.2 "
    "6.5 13.8 6.2 15 6.1 16 7.4 16.8 7 19.2 7.1 18.9 10.3L18.7 11 18.3 15.9 17.8 16.6 17.8 "
    "17.6C18.7 17.7 18.3 18.8 17.8 18.8L13 19.3Z";

static SkPath GetHandShape() {
  static SkPath path = [] {
    auto path = PathFromSVG(kHandShapeSVG);
    SkMatrix matrix = SkMatrix::I();
    float s = 1.67;
    matrix.postScale(s, s);
    matrix.postRotate(15);
    matrix.postTranslate(2.6_mm, 1.9_mm);
    path = path.makeTransform(matrix);
    return path;
  }();
  return path;
}

struct DragAndClickAction : Action {
  ui::PointerButton btn;
  std::unique_ptr<Action> drag_action;
  std::function<void(ui::Pointer&)> on_click;
  time::SteadyPoint press_time;
  DragAndClickAction(ui::Pointer& pointer, ui::PointerButton btn,
                     std::unique_ptr<Action>&& drag_action,
                     std::function<void(ui::Pointer&)> on_click)
      : Action(pointer),
        btn(btn),
        drag_action(std::move(drag_action)),
        on_click(std::move(on_click)) {
    press_time = pointer.button_down_time[static_cast<int>(btn)];
  }
  ~DragAndClickAction() override {
    if (on_click && (time::SteadyNow() - press_time < 0.2s)) {
      on_click(pointer);
    }
  }
  void Update() override {
    if (drag_action) {
      drag_action->Update();
    }
  }
  ui::Widget* Widget() override {
    if (drag_action) {
      return drag_action->Widget();
    }
    return nullptr;
  }
  void VisitObjects(std::function<void(Object&)> visitor) override {
    if (drag_action) {
      drag_action->VisitObjects(visitor);
    }
  }
};

struct KeyPresserButton : KeyButton {
  bool is_pressed = false;
  using KeyButton::KeyButton;
  float PressRatio() const override { return is_pressed ? 1 : 0; }
};

struct KeyPresserWidget : ObjectToy {
  mutable std::unique_ptr<KeyPresserButton> shortcut_button;

  // This is used to select the pressed key
  MortalPtr<ui::Caret> key_selector;
  std::unique_ptr<ui::ActionZone> hand_zone;

  KeyPresserWidget(Widget* parent, Object& key_presser);

  Tock Tick(time::Timer& timer) override {
    shortcut_button->fg = key_selector ? kKeyGrabbingColor : KeyColor(false);
    if (auto key_presser = LockObject<KeyPresser>()) {
      shortcut_button->is_pressed = key_presser->key_pressed;
      shortcut_button->SetLabel(ToStr(key_presser->key), &timer);
    }
    shortcut_button->WakeAnimationAt(timer.last);
    return Tock::Draw;
  }

  void Draw(SkCanvas& canvas) const override {
    bool is_pressed = shortcut_button->is_pressed;

    BakeChildren(canvas);
    auto& img = is_pressed ? textures::PressingHandColor() : textures::PointingHandColor();
    canvas.save();
    canvas.translate(4.5_mm, -6.8_mm);
    canvas.rotate(15);
    img.draw(canvas);
    canvas.restore();
  }

  SkPath Shape() const override {
    auto button_shape = shortcut_button->Shape();
    auto hand_shape = GetHandShape();
    SkPath joined_shape;
    Op(button_shape, hand_shape, kUnion_SkPathOp, &joined_shape);
    return joined_shape;
  }

  bool CenteredAtZero() const override { return true; }

  void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override {
    auto button_shape = shortcut_button->Shape();
    SkRRect rrect;
    if (button_shape.isRRect(&rrect)) {
      out_positions.push_back(Vec2AndDir{.pos = Rect::TopCenter(rrect.rect()), .dir = -90_deg});
      out_positions.push_back(Vec2AndDir{.pos = Rect::LeftCenter(rrect.rect()), .dir = 0_deg});
      out_positions.push_back(Vec2AndDir{.pos = Rect::RightCenter(rrect.rect()), .dir = 180_deg});
    }
  }

  Interface FindOption(ui::Pointer&, ui::ActionTrigger) override;
  MiniMenuMode MenuMode() override { return MODE_6_DIR; }

  void KeyDown(ui::Caret&, ui::Key k) override {
    key_selector->Release();
    LockObject<KeyPresser>()->SetKey(k.physical);
    WakeAnimation();
    shortcut_button->WakeAnimation();
  }

  void ReleaseCaret(ui::Caret&) override {
    WakeAnimation();
    shortcut_button->WakeAnimation();
  }

  bool AllowChildPointerEvents(Widget& child) const override { return &child == hand_zone.get(); }
};

static std::unique_ptr<Action> DragKeyPresser(ui::Pointer& p, KeyPresserWidget& widget) {
  auto* lw = ui::Closest<LocationWidget>(widget);
  auto loc = lw ? lw->LockLocation() : nullptr;
  auto key_presser = widget.LockObject<KeyPresser>();
  if (!loc || !key_presser) return nullptr;
  return PickUp(p, *loc, *key_presser);
}

static std::unique_ptr<Action> PressKeyActivate(Interface self, ui::Pointer& pointer, Toy* toy) {
  auto* widget = dynamic_cast<KeyPresserWidget*>(toy);
  if (!widget) return nullptr;
  WeakPtr<Object> weak = self.object_ptr->AcquireWeakPtr();
  return std::make_unique<DragAndClickAction>(
      pointer, ui::PointerButton::Left, DragKeyPresser(pointer, *widget), [weak](ui::Pointer&) {
        if (auto object = weak.Lock()) Runnable(*object, KeyPresser::run_tbl).ScheduleRun();
      });
}

static std::unique_ptr<Action> SetKeyActivate(Interface, ui::Pointer& pointer, Toy* toy) {
  auto* widget = dynamic_cast<KeyPresserWidget*>(toy);
  if (!widget) return nullptr;
  MortalPtr<KeyPresserWidget> target = widget;
  return std::make_unique<DragAndClickAction>(
      pointer, ui::PointerButton::Left, DragKeyPresser(pointer, *widget), [target](ui::Pointer& p) {
        if (target) {
          if (target->key_selector) {
            target->key_selector->Release();
          } else if (p.keyboard) {
            auto* child = target->shortcut_button->child.get();
            Vec2 caret_position =
                TransformBetween(*child, *target).mapPoint(child->DrawBounds()->TopLeftCorner());
            target->key_selector = &p.keyboard->RequestCaret(*target, caret_position);
          }
          target->WakeAnimation();
          target->shortcut_button->WakeAnimation();
        }
      });
}

constinit Signal::Table kPressKey = [] {
  Signal::Table t("Press key");
  t.activate = &PressKeyActivate;
  return t;
}();

constinit Signal::Table kSetKey = [] {
  Signal::Table t("Set key");
  t.activate = &SetKeyActivate;
  return t;
}();

struct KeyPresserHandZone : ui::ActionZone {
  using ActionZone::ActionZone;
  SkPath Shape() const override { return GetHandShape(); }
  Interface FindOption(ui::Pointer&, ui::ActionTrigger trigger) override {
    if (trigger != ui::PointerButton::Left) return {};
    auto key_presser = static_cast<KeyPresserWidget&>(*parent).LockObject<KeyPresser>();
    return key_presser ? Interface(*key_presser, kPressKey) : Interface();
  }
};

KeyPresserWidget::KeyPresserWidget(Widget* parent, Object& key_presser)
    : ObjectToy(parent, key_presser),
      shortcut_button(new KeyPresserButton(this, "?", KeyColor(false), kBaseKeyWidth)),
      hand_zone(new KeyPresserHandZone(this)) {
  layers.OrderInside(shortcut_button.get());
}

Interface KeyPresserWidget::FindOption(ui::Pointer& pointer, ui::ActionTrigger trigger) {
  using enum ui::Dir;
  auto key_presser = LockObject<KeyPresser>();
  if (!key_presser) return {};
  if (trigger == ui::PointerButton::Left) return Interface(*key_presser, kSetKey);
  switch (static_cast<ui::Dir>(trigger)) {
    case NE:
      return Interface(*key_presser, KeyPresser::state_tbl);
    case NW:
      return Interface(*key_presser, KeyPresser::monitoring_tbl);
    default:
      return ObjectToy::FindOption(pointer, trigger);
  }
}

KeyPresser::KeyPresser(ui::AnsiKey key) : key(key) {}
string_view KeyPresser::Name() const { return "Key Presser"; }
Ptr<Object> KeyPresser::Clone() const { return MAKE_PTR(KeyPresser, key); }

std::unique_ptr<ObjectToy> KeyPresser::MakeToy(ui::Widget* parent) {
  auto w = std::make_unique<KeyPresserWidget>(parent, *this);
  w->shortcut_button->SetLabel(ToStr(key));
  return std::move(w);
}

void KeyPresser::SetKey(ui::AnsiKey k) {
  key = k;
  WakeToys();
}

void KeyPresser::SerializeState(ObjectSerializer& writer) const {
  writer.Key("key");
  auto key_name = ToStr(this->key);
  writer.String(key_name.data(), key_name.size());
  if (monitoring->IsOn()) {
    writer.Key("monitoring");
    writer.Bool(true);
  }
}
bool KeyPresser::DeserializeKey(ObjectDeserializer& d, StrView keyName) {
  if (keyName == "key") {
    Status status;
    Str value;
    d.Get(value, status);
    if (OK(status)) {
      SetKey(ui::AnsiKeyFromStr(value));
    } else {
      ReportError("Failed to deserialize KeyPresser. " + status.ToStr());
    }
    return true;
  } else if (keyName == "monitoring") {
    Status status;
    bool monitoring_on;
    d.Get(monitoring_on, status);
    if (OK(status) && monitoring_on) {
      monitoring->TurnOn();
    }
    return true;
  }
  return false;
}

KeyPresser::~KeyPresser() {
  if (keylogging) {
    keylogging->Release();
  }
}

constexpr time::Duration kKeyEventInhibitDuration = 50ms;

void KeyPresser::KeyloggerKeyDown(ui::Key key_down) {
  if (this->key != key_down.physical) return;
  if (key_pressed) return;
  if (time::SteadyNow() < last_pressed_time + kKeyEventInhibitDuration) {
    last_pressed_time = time::kZeroSteady;
    return;
  }
  key_pressed = true;
  state->NotifyTurnedOn();
  WakeToys();
}

void KeyPresser::KeyloggerKeyUp(ui::Key key_up) {
  if (this->key != key_up.physical) return;
  if (!key_pressed) return;
  if (time::SteadyNow() < last_released_time + kKeyEventInhibitDuration) {
    last_released_time = time::kZeroSteady;
    return;
  }
  key_pressed = false;
  state->NotifyTurnedOff();
  WakeToys();
}

void KeyPresser::KeyloggerOnRelease(const ui::Keylogging&) {}

}  // namespace automat::library
