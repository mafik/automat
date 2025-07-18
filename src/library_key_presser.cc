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
  static SkPath path = []() {
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

float KeyPresserButton::PressRatio() const {
  if (key_presser->key_selector || key_presser->key_pressed) {
    return 1;
  }
  return 0;
}

KeyPresser::KeyPresser(gui::AnsiKey key)
    : key(key),
      shortcut_button(MakePtr<KeyPresserButton>(this, MakeKeyLabelWidget(ToStr(key)),
                                                KeyColor(false), kBaseKeyWidth)) {
  shortcut_button->activate = [this](gui::Pointer& pointer) {
    if (key_selector) {
      key_selector->Release();
    } else if (pointer.keyboard) {
      Vec2 caret_position = shortcut_button->child->TextureBounds()->TopLeftCorner();
      key_selector = &pointer.keyboard->RequestCaret(*this, shortcut_button->child, caret_position);
    }
    WakeAnimation();
    shortcut_button->WakeAnimation();
  };
}
KeyPresser::KeyPresser() : KeyPresser(gui::AnsiKey::F) {}
string_view KeyPresser::Name() const { return "Key Presser"; }
Ptr<Object> KeyPresser::Clone() const { return MakePtr<KeyPresser>(key); }
animation::Phase KeyPresser::Tick(time::Timer&) {
  shortcut_button->fg = key_selector ? kKeyGrabbingColor : KeyColor(false);
  return animation::Finished;
}
void KeyPresser::Draw(SkCanvas& canvas) const {
  DrawChildren(canvas);

  static auto pointing_hand_color =
      PersistentImage::MakeFromAsset(embedded::assets_pointing_hand_color_webp, {.height = 8.8_mm});
  static auto pressing_hand_color =
      PersistentImage::MakeFromAsset(embedded::assets_pressing_hand_color_webp, {.height = 8.8_mm});

  auto& img = key_pressed ? pressing_hand_color : pointing_hand_color;
  canvas.save();
  canvas.translate(4.5_mm, -6.8_mm);
  canvas.rotate(15);
  img.draw(canvas);
  canvas.restore();
}
SkPath KeyPresser::Shape() const {
  static SkPath shape = [this]() {
    auto button_shape = shortcut_button->Shape();
    auto hand_shape = GetHandShape();
    SkPath joined_shape;
    Op(button_shape, hand_shape, kUnion_SkPathOp, &joined_shape);
    return joined_shape;
  }();
  return shape;
}
void KeyPresser::ConnectionPositions(Vec<Vec2AndDir>& out_positions) const {
  auto button_shape = shortcut_button->Shape();
  SkRRect rrect;
  if (button_shape.isRRect(&rrect)) {
    out_positions.push_back(Vec2AndDir{.pos = Rect::TopCenter(rrect.rect()), .dir = -90_deg});
    out_positions.push_back(Vec2AndDir{.pos = Rect::LeftCenter(rrect.rect()), .dir = 0_deg});
    out_positions.push_back(Vec2AndDir{.pos = Rect::RightCenter(rrect.rect()), .dir = 180_deg});
  }
}

void KeyPresser::SetKey(gui::AnsiKey k) {
  key = k;
  shortcut_button->SetLabel(ToStr(k));
}

void KeyPresser::ReleaseCaret(gui::Caret&) {
  key_selector = nullptr;
  WakeAnimation();
  shortcut_button->WakeAnimation();
}
void KeyPresser::KeyDown(gui::Caret&, gui::Key k) {
  key_selector->Release();
  SetKey(k.physical);
  WakeAnimation();
  shortcut_button->WakeAnimation();
}

void KeyPresser::FillChildren(Vec<Ptr<Widget>>& children) { children.push_back(shortcut_button); }

struct DragAndClickAction : Action {
  gui::PointerButton btn;
  std::unique_ptr<Action> drag_action;
  std::unique_ptr<Option> click_option;
  time::SystemPoint press_time;
  DragAndClickAction(gui::Pointer& pointer, gui::PointerButton btn,
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
  gui::Widget* Widget() override {
    if (drag_action) {
      return drag_action->Widget();
    }
    return nullptr;
  }
};

struct RunAction : Action {
  Location& location;
  RunAction(gui::Pointer& pointer, Location& location) : Action(pointer), location(location) {
    if (auto long_running = location.object->AsLongRunning(); long_running->IsRunning()) {
      long_running->Cancel();
    } else {
      location.ScheduleRun();
    }
  }
  void Update() override {}
};

struct RunOption : Option {
  Ptr<gui::Widget> widget;
  RunOption(Ptr<gui::Widget> widget) : Option("Run"), widget(widget) {}
  std::unique_ptr<Option> Clone() const override { return std::make_unique<RunOption>(widget); }
  std::unique_ptr<Action> Activate(gui::Pointer& p) const override {
    return std::make_unique<RunAction>(p, *Closest<Location>(*widget));
  }
};

struct UseObjectOption : Option {
  Ptr<gui::Widget> widget;

  UseObjectOption(Ptr<gui::Widget> widget) : Option("Use"), widget(widget) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<UseObjectOption>(widget);
  }
  std::unique_ptr<Action> Activate(gui::Pointer& p) const override {
    return widget->FindAction(p, gui::PointerButton::Left);
  }
};

std::unique_ptr<Action> KeyPresser::FindAction(gui::Pointer& p, gui::ActionTrigger btn) {
  if (btn != gui::PointerButton::Left) return nullptr;
  auto hand_shape = GetHandShape();
  auto local_pos = p.PositionWithin(*this);
  if (hand_shape.contains(local_pos.x, local_pos.y)) {
    return std::make_unique<DragAndClickAction>(p, btn, Object::FallbackWidget::FindAction(p, btn),
                                                std::make_unique<RunOption>(AcquirePtr()));
  } else {
    return std::make_unique<DragAndClickAction>(p, btn, Object::FallbackWidget::FindAction(p, btn),
                                                std::make_unique<UseObjectOption>(shortcut_button));
  }
}

void KeyPresser::OnRun(Location& here, RunTask& run_task) {
  ZoneScopedN("KeyPresser");
  audio::Play(embedded::assets_SFX_key_down_wav);
  SendKeyEvent(key, true);
  key_pressed = true;
  WakeAnimation();
  BeginLongRunning(here, run_task);
}

void KeyPresser::OnCancel() {
  audio::Play(embedded::assets_SFX_key_up_wav);
  SendKeyEvent(key, false);
  key_pressed = false;
  WakeAnimation();
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
        SetKey(gui::AnsiKeyFromStr(value));
      }
    }
  }
  if (!OK(status)) {
    l.ReportError("Failed to deserialize KeyPresser. " + status.ToStr());
  }
}

KeyPresser::~KeyPresser() {
  if (key_pressed) {
    Cancel();
  }
}
}  // namespace automat::library
