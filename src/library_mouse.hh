#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

// This file contains shared code for mouse-related objects.

#include <include/effects/SkRuntimeEffect.h>

#include "base.hh"
#include "parent_ref.hh"

namespace automat::library::mouse {

SkRuntimeEffect& GetPixelGridRuntimeEffect();

}  // namespace automat::library::mouse

namespace automat::library {

// Mouse is an object that really does nothing, except offering access into other mouse-related
// objects.
struct Mouse : Object {
  string_view Name() const override { return "Mouse"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

struct MouseButtonEvent : Object {
  ui::PointerButton button;
  bool down;
  struct MyRunnable : Runnable {
    void OnRun(std::unique_ptr<RunTask>&) override;
    PARENT_REF(MouseButtonEvent, runnable)
  } runnable;
  MouseButtonEvent(ui::PointerButton button, bool down) : button(button), down(down) {}
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void Atoms(const std::function<LoopControl(Atom&)>& cb) override;
  Runnable* AsRunnable() override { return &runnable; }
  SignalNext* AsSignalNext() override { return &runnable; }
  audio::Sound& NextSound() override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

struct MouseMove : Object {
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
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
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void OnRelativeFloat64(double) override;
};

struct MouseScrollX : Object, SinkRelativeFloat64 {
  SinCos rotation;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void OnRelativeFloat64(double) override;
};

struct MouseButtonPresser : Object {
  ui::PointerButton button;
  bool down = false;

  struct Click : Runnable {
    StrView Name() const override { return "Click"sv; }

    void OnRun(std::unique_ptr<RunTask>&) override;

    PARENT_REF(MouseButtonPresser, click)
  } click;

  struct State : OnOff {
    StrView Name() const override { return "State"sv; }
    bool IsOn() const override { return MouseButtonPresser().down; }
    void OnTurnOn() override;
    void OnTurnOff() override;

    PARENT_REF(MouseButtonPresser, state)
  } state;

  MouseButtonPresser(ui::PointerButton button);
  MouseButtonPresser();
  ~MouseButtonPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void Atoms(const std::function<LoopControl(Atom&)>& cb) override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  operator OnOff*() override { return &state; }

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
