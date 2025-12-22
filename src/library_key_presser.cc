// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_key_presser.hh"

#include <include/core/SkSamplingOptions.h>
#include <include/pathops/SkPathOps.h>

#include <memory>
#include <tracy/Tracy.hpp>

#include "../build/generated/embedded.hh"
#include "key_button.hh"
#include "keyboard.hh"
#include "menu.hh"
#include "root_widget.hh"
#include "sincos.hh"
#include "status.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"

using namespace std;
using namespace automat;

namespace automat::library {

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
    path.transform(matrix);
    return path;
  }();
  return path;
}

struct DragAndClickAction : Action {
  ui::PointerButton btn;
  std::unique_ptr<Action> drag_action;
  std::unique_ptr<Option> click_option;
  time::SystemPoint press_time;
  DragAndClickAction(ui::Pointer& pointer, ui::PointerButton btn,
                     std::unique_ptr<Action>&& drag_action, std::unique_ptr<Option>&& click_option)
      : Action(pointer),
        btn(btn),
        drag_action(std::move(drag_action)),
        click_option(std::move(click_option)) {
    press_time = pointer.button_down_time[static_cast<int>(btn)];
  }
  ~DragAndClickAction() override {
    if (click_option && (time::SystemNow() - press_time < 0.2s)) {
      auto click_action = click_option->Activate(pointer);
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
};

struct RunAction : Action {
  Location& location;
  RunAction(ui::Pointer& pointer, Location& location) : Action(pointer), location(location) {
    if (auto long_running = location.object->AsLongRunning(); long_running->IsRunning()) {
      long_running->Cancel();
    } else {
      location.ScheduleRun();
    }
  }
  void Update() override {}
};

struct RunOption : TextOption {
  ui::Widget* widget;
  RunOption(ui::Widget* widget) : TextOption("Run"), widget(widget) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<RunOption>(widget); }
  std::unique_ptr<Action> Activate(ui::Pointer& p) const override {
    return std::make_unique<RunAction>(p, *Closest<Location>(*widget));
  }
};

struct UseObjectOption : TextOption {
  ui::Widget* widget;

  UseObjectOption(ui::Widget* widget) : TextOption("Use"), widget(widget) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<UseObjectOption>(widget);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& p) const override {
    return widget->FindAction(p, ui::PointerButton::Left);
  }
};

struct KeyPresserButton : KeyButton {
  KeyPresserButton(Widget* parent, StrView label, SkColor color, float width)
      : KeyButton(parent, label, color, width) {}
  using KeyButton::KeyButton;
  float PressRatio() const override;
};

struct KeyPresserWidget : Object::WidgetBase, ui::CaretOwner {
  mutable std::unique_ptr<KeyPresserButton> shortcut_button;

  // This is used to select the pressed key
  ui::Caret* key_selector = nullptr;

  KeyPresserWidget(Widget* parent)
      : WidgetBase(parent),
        shortcut_button(new KeyPresserButton(this, "?", KeyColor(false), kBaseKeyWidth)) {
    shortcut_button->activate = [this](ui::Pointer& pointer) {
      if (key_selector) {
        key_selector->Release();
      } else if (pointer.keyboard) {
        Vec2 caret_position = shortcut_button->child->TextureBounds()->TopLeftCorner();
        key_selector =
            &pointer.keyboard->RequestCaret(*this, shortcut_button->child.get(), caret_position);
      }
      WakeAnimation();
      shortcut_button->WakeAnimation();
    };
  }

  animation::Phase Tick(time::Timer&) override {
    shortcut_button->fg = key_selector ? kKeyGrabbingColor : KeyColor(false);
    return animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    bool is_running = false;
    {
      auto key_presser = LockObject<KeyPresser>();
      is_running = key_presser->IsRunning();
    }

    DrawChildren(canvas);
    auto& img = is_running ? textures::PressingHandColor() : textures::PointingHandColor();
    canvas.save();
    canvas.translate(4.5_mm, -6.8_mm);
    canvas.rotate(15);
    img.draw(canvas);
    canvas.restore();
  }

  SkPath Shape() const override {
    static SkPath shape = [this]() {
      auto button_shape = shortcut_button->Shape();
      auto hand_shape = GetHandShape();
      SkPath joined_shape;
      Op(button_shape, hand_shape, kUnion_SkPathOp, &joined_shape);
      return joined_shape;
    }();
    return shape;
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

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn != ui::PointerButton::Left) return WidgetBase::FindAction(p, btn);
    auto hand_shape = GetHandShape();
    auto local_pos = p.PositionWithin(*this);
    if (hand_shape.contains(local_pos.x, local_pos.y)) {
      return std::make_unique<DragAndClickAction>(p, btn, Object::WidgetBase::FindAction(p, btn),
                                                  std::make_unique<RunOption>(this));
    } else {
      return std::make_unique<DragAndClickAction>(
          p, btn, Object::WidgetBase::FindAction(p, btn),
          std::make_unique<UseObjectOption>(shortcut_button.get()));
    }
  }

  void KeyDown(ui::Caret&, ui::Key k) override {
    key_selector->Release();
    LockObject<KeyPresser>()->SetKey(k.physical);
    WakeAnimation();
    shortcut_button->WakeAnimation();
  }

  void ReleaseCaret(ui::Caret&) override {
    key_selector = nullptr;
    WakeAnimation();
    shortcut_button->WakeAnimation();
  }

  void FillChildren(Vec<Widget*>& children) override { children.push_back(shortcut_button.get()); }

  bool AllowChildPointerEvents(Widget& child) const override { return false; }
};

float KeyPresserButton::PressRatio() const {
  auto key_presser_widget = static_cast<KeyPresserWidget*>(this->parent.Get());
  auto key_presser = key_presser_widget->LockObject<KeyPresser>();
  if (key_presser_widget->key_selector || key_presser->IsRunning()) {
    return 1;
  }
  return 0;
}

KeyPresser::KeyPresser(ui::AnsiKey key) : key(key) {}
string_view KeyPresser::Name() const { return "Key Presser"; }
Ptr<Object> KeyPresser::Clone() const { return MAKE_PTR(KeyPresser, key); }

std::unique_ptr<Object::WidgetInterface> KeyPresser::MakeWidget(ui::Widget* parent) {
  auto w = std::make_unique<KeyPresserWidget>(parent);
  w->object = AcquireWeakPtr();
  w->shortcut_button->SetLabel(ToStr(key));
  return std::move(w);
}

void KeyPresser::SetKey(ui::AnsiKey k) {
  key = k;
  ForEachWidget([k](ui::RootWidget&, ui::Widget& widget) {
    static_cast<KeyPresserWidget&>(widget).shortcut_button->SetLabel(ToStr(k));
  });
}

void KeyPresser::Args(std::function<void(Argument&)> cb) {
  if (keylogging) {
    cb(next_arg);
  }
}

void KeyPresser::OnRun(Location& here, std::unique_ptr<RunTask>& run_task) {
  ZoneScopedN("KeyPresser");
  audio::Play(embedded::assets_SFX_key_down_wav);
  SendKeyEvent(key, true);
  WakeWidgetsAnimation();
  BeginLongRunning(std::move(run_task));
}

void KeyPresser::OnCancel() {
  audio::Play(embedded::assets_SFX_key_up_wav);
  SendKeyEvent(key, false);
  WakeWidgetsAnimation();
}

void KeyPresser::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("key");
  auto key_name = ToStr(this->key);
  writer.String(key_name.data(), key_name.size());
  writer.EndObject();
}
void KeyPresser::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto& key : ObjectView(d, status)) {
    if (key == "key") {
      Str value;
      d.Get(value, status);
      if (OK(status)) {
        SetKey(ui::AnsiKeyFromStr(value));
      }
    }
  }
  if (!OK(status)) {
    ReportError("Failed to deserialize KeyPresser. " + status.ToStr());
  }
}

KeyPresser::~KeyPresser() {
  if (keylogging) {
    keylogging->Release();
  }
  OnLongRunningDestruct();
}

void KeyPresser::KeyloggerKeyDown(ui::Key key_down) {
  if (this->key != key_down.physical) return;
  if (auto h = here.lock()) {
    ScheduleNext(*h);
  }
}

void KeyPresser::KeyloggerKeyUp(ui::Key key_up) {
  if (this->key != key_up.physical) return;
}

void KeyPresser::KeyloggerOnRelease(const ui::Keylogging&) { keylogging = nullptr; }

bool KeyPresser::Monitoring::IsOn() const { return GetKeyPresser().keylogging != nullptr; };

void KeyPresser::Monitoring::OnTurnOn() {
  auto& key_presser = GetKeyPresser();
  if (key_presser.keylogging == nullptr) {
    ui::root_widget->window->BeginLogging(&key_presser, &key_presser.keylogging, nullptr, nullptr);
  }
}

void KeyPresser::Monitoring::OnTurnOff() {
  auto& key_presser = GetKeyPresser();
  if (key_presser.keylogging) {
    key_presser.keylogging->Release();
  }
}

}  // namespace automat::library
