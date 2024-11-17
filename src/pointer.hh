// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <vector>

#include "action.hh"
#include "math.hh"
#include "str.hh"
#include "time.hh"
#include "vec.hh"
#include "widget.hh"

namespace automat::animation {
struct Display;
}  // namespace automat::animation

namespace automat::gui {

struct Keyboard;
struct PointerImpl;
struct Widget;
struct Window;
struct DrawContext;

struct PointerMoveCallback {
  maf::Vec<Pointer*> pointers;
  virtual ~PointerMoveCallback();
  virtual void PointerMove(Pointer&, Vec2 position) = 0;
  void StartWatching(Pointer& p);
  void StopWatching(Pointer& p);
};

struct Pointer final {
  Pointer(Window&, Vec2 position);
  ~Pointer();
  void Move(Vec2 position);
  void Wheel(float delta);
  void ButtonDown(PointerButton);
  void ButtonUp(PointerButton);

  Widget* GetWidget() {
    if (action) {
      return action->Widget();
    }
    return nullptr;
  }

  enum IconType { kIconArrow, kIconHand, kIconIBeam };

  IconType Icon() const;
  void PushIcon(IconType);
  void PopIcon();

  void UpdatePath();

  Vec2 PositionWithin(const Widget&) const;
  Vec2 PositionWithinRootMachine() const;

  maf::Str ToStr() const;

  Window& window;
  // The main keyboard associated with this pointer device. May be null!
  Keyboard* keyboard;

  Vec2 pointer_position;
  std::vector<Pointer::IconType> icons;

  Vec2 button_down_position[static_cast<int>(PointerButton::Count)];
  time::SystemPoint button_down_time[static_cast<int>(PointerButton::Count)];

  maf::Vec<PointerMoveCallback*> move_callbacks;

  std::unique_ptr<Action> action;
  std::shared_ptr<Widget> hover;

  maf::Vec<std::weak_ptr<Widget>> path;
};

}  // namespace automat::gui
