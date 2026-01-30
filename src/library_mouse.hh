#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

// This file contains shared code for mouse-related objects.

#include <include/effects/SkRuntimeEffect.h>

#include "base.hh"

namespace automat::library::mouse {

SkRuntimeEffect& GetPixelGridRuntimeEffect();

}  // namespace automat::library::mouse

namespace automat::library {

// Mouse is an object that really does nothing, except offering access into other mouse-related
// objects.
struct Mouse : Object {
  string_view Name() const override { return "Mouse"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, Object&) override;
};

struct MouseButtonEvent : Object, Runnable {
  ui::PointerButton button;
  bool down;
  MouseButtonEvent(ui::PointerButton button, bool down) : button(button), down(down) {}
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void Parts(const std::function<void(Part&)>& cb) override;
  void OnRun(std::unique_ptr<RunTask>&) override;
  audio::Sound& NextSound() override;
  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, Object&) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

struct MouseMove : Object {
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, Object&) override;
  void OnMouseMove(Vec2);
};

// Interface for objects that act as sinks for relative float64 values.
struct SinkRelativeFloat64 {
  virtual void OnRelativeFloat64(double value) = 0;
};

struct MouseScrollY : Object, SinkRelativeFloat64 {
  SinCos rotation;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, Object&) override;
  void OnRelativeFloat64(double) override;
};

struct MouseScrollX : Object, SinkRelativeFloat64 {
  SinCos rotation;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, Object&) override;
  void OnRelativeFloat64(double) override;
};

struct MouseButtonPresser : Object, Runnable, OnOff {
  ui::PointerButton button;
  bool down = false;

  MouseButtonPresser(ui::PointerButton button);
  MouseButtonPresser();
  ~MouseButtonPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void Parts(const std::function<void(Part&)>& cb) override;
  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, Object&) override;

  void OnRun(std::unique_ptr<RunTask>& run_task) override;

  bool IsOn() const override { return down; }
  void OnTurnOn() override;
  void OnTurnOff() override;

  operator OnOff*() override { return this; }

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
