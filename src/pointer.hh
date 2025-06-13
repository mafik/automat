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
struct RootWidget;

struct PointerMoveCallback {
  Vec<Pointer*> pointers;
  virtual ~PointerMoveCallback();
  virtual void PointerMove(Pointer&, Vec2 position) = 0;
  void StartWatching(Pointer& p);
  void StopWatching(Pointer& p);
};

struct PointerGrab;

// Base class for objects that can grab pointer input.
struct PointerGrabber {
  virtual ~PointerGrabber() = default;

  // Called by the Pointer infrastructure to make the PointerGrabber release all resources related
  // to this grab.
  virtual void ReleaseGrab(PointerGrab&) = 0;

  virtual void PointerGrabberMove(PointerGrab&, Vec2) {}
  virtual void PointerGrabberButtonDown(PointerGrab&, PointerButton) {}
  virtual void PointerGrabberButtonUp(PointerGrab&, PointerButton) {}
  virtual void PointerGrabberWheel(PointerGrab&, float) {}
};

// Represents a pointer grab. Can be used to manipulate it.
struct PointerGrab {
  Pointer& pointer;
  PointerGrabber& grabber;
  PointerGrab(Pointer& pointer, PointerGrabber& grabber) : pointer(pointer), grabber(grabber) {}
  virtual ~PointerGrab() = default;

  // This will also call `ReleaseGrab` of its PointerGrabber.
  // This means that the pointers in the PointerGrabber will be invalid (nullptr) after this call!
  virtual void Release();
};

struct PointerWidget;

struct Pointer {
  Pointer(RootWidget&, Vec2 position);
  ~Pointer();
  void Move(Vec2 position);
  void Wheel(float delta);
  void ButtonDown(PointerButton);
  void ButtonUp(PointerButton);

  Widget* GetWidget();

  enum IconType {
    kIconArrow,
    kIconHand,
    kIconIBeam,
    kIconAllScroll,
    kIconResizeHorizontal,
    kIconCrosshair,
  };

  IconType Icon() const;
  virtual void PushIcon(IconType);
  virtual void PopIcon();

  void UpdatePath();

  Vec2 PositionWithin(const Widget&) const;
  Vec2 PositionWithinRootMachine() const;

  Str ToStr() const;

  // Called by a PointerGrabber that wants to grab all pointer events, even when Automat is in the
  // background.
  //
  // Should be overridden by platform-specific implementations to actually grab the pointer.
  virtual PointerGrab& RequestGlobalGrab(PointerGrabber&);

  void EndAllActions();
  void ReplaceAction(Action& old_action, std::unique_ptr<Action>&& new_action);

  std::unique_ptr<PointerGrab> grab;

  RootWidget& root_widget;
  // The main keyboard associated with this pointer device. May be null!
  Keyboard* keyboard;

  Vec2 pointer_position;
  std::vector<Pointer::IconType> icons;

  Vec2 button_down_position[static_cast<int>(PointerButton::Count)];
  time::SystemPoint button_down_time[static_cast<int>(PointerButton::Count)];

  Vec<PointerMoveCallback*> move_callbacks;

  std::unique_ptr<Action> actions[static_cast<int>(PointerButton::Count)];
  Ptr<Widget> hover;

  Vec<WeakPtr<Widget>> path;

  Ptr<PointerWidget> pointer_widget;
};

struct PointerWidget : Widget {
  Pointer& pointer;
  PointerWidget(Pointer& pointer) : pointer(pointer) {}
  SkPath Shape() const override { return SkPath(); }
  void FillChildren(Vec<Ptr<Widget>>& children) override {
    for (auto& action : pointer.actions) {
      if (action == nullptr) {
        continue;
      }
      if (auto widget = action->Widget()) {
        children.push_back(widget->AcquirePtr());
      }
    }
  }
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
};

}  // namespace automat::gui
