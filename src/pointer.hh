// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <list>

#include "action.hh"
#include "math.hh"
#include "object.hh"
#include "ptr.hh"
#include "str.hh"
#include "time.hh"
#include "vec.hh"
#include "widget.hh"

namespace automat::animation {
struct Display;
}  // namespace automat::animation

namespace automat::ui {

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
  virtual ~PointerGrab();

  // This will also call `ReleaseGrab` of its PointerGrabber.
  void Release();
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
    kIconResizeVertical,
    kIconCrosshair,
  };

  // RAII wrapper for icon lifetime management
  struct IconOverride {
    IconOverride(Pointer&, IconType);
    ~IconOverride();

    // Non-copyable and non-movable
    IconOverride(const IconOverride&) = delete;
    IconOverride(IconOverride&&) = delete;
    IconOverride& operator=(const IconOverride&) = delete;
    IconOverride& operator=(IconOverride&&) = delete;

    Pointer& pointer;
    std::list<IconType>::iterator it;
  };

  IconType Icon() const;

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
  std::list<Pointer::IconType> icons;

  Vec2 button_down_position[static_cast<int>(PointerButton::Count)];
  time::SystemPoint button_down_time[static_cast<int>(PointerButton::Count)];

  Vec<PointerMoveCallback*> move_callbacks;

  std::unique_ptr<Action> actions[static_cast<int>(PointerButton::Count)];
  TrackedPtr<Widget> hover;

  Vec<TrackedPtr<Widget>> path;

  unique_ptr<PointerWidget> pointer_widget;

  // Called when icon state changes (for platform-specific cursor updates)
  virtual void OnIconChanged(IconType old_icon, IconType new_icon) {}

  struct Logging;

  struct Logger {
    virtual ~Logger() = default;
    virtual void PointerLoggerButtonDown(Logging&, PointerButton) {}
    virtual void PointerLoggerButtonUp(Logging&, PointerButton) {}

    // The value is ±1 when scroll wheel moves one notch.
    // Typical mouse wheel contains 24 notches in one rotation.
    // OS-es may quantize this value to multiples of 1/120.
    // Value is positive when the finger moves up.
    virtual void PointerLoggerScrollY(Logging&, float) {}
    virtual void PointerLoggerScrollX(Logging&, float) {}

    // `relative_px` uses OS coords (so Y axis is increasing downwards).
    virtual void PointerLoggerMove(Logging&, Vec2 relative_px) {}
  };

  struct Logging {
    Pointer& pointer;
    Logger& logger;
    Logging(Pointer& pointer, Logger& logger) : pointer(pointer), logger(logger) {}
    void Release();
  };

  Vec<std::unique_ptr<Logging>> loggings;
};

struct PointerWidget : Widget {
  Pointer& pointer;
  PointerWidget(Widget* parent, Pointer& pointer) : Widget(parent), pointer(pointer) {}
  SkPath Shape() const override { return SkPath(); }

  struct HighlightState {
    Toy* widget;
    Part* part;
    float highlight;
  };

  std::vector<HighlightState> highlight_current;
  double time_seconds = 0;  // used to animate dashed lines

  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas& canvas) const override;

  void FillChildren(Vec<Widget*>& children) override {
    for (auto& action : pointer.actions) {
      if (action == nullptr) {
        continue;
      }
      if (auto widget = action->Widget()) {
        children.push_back(widget);
      }
    }
  }
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
};

}  // namespace automat::ui
