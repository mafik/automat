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
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

struct MouseButtonEvent : Object {
  ui::PointerButton button;
  bool down;

  struct Run : Runnable {
    using Parent = MouseButtonEvent;
    static constexpr StrView kName = "Run"sv;
    static constexpr int Offset() { return offsetof(MouseButtonEvent, run); }

    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<Run> run;

  struct Next : NextArg {
    using Parent = MouseButtonEvent;
    static constexpr StrView kName = "Next"sv;
    static constexpr int Offset() { return offsetof(MouseButtonEvent, next); }
  };
  NextArg::Def<Next> next;

  MouseButtonEvent(ui::PointerButton button, bool down) : button(button), down(down) {}
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  INTERFACES(run, next)
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
    using Parent = MouseButtonPresser;
    static constexpr StrView kName = "Click"sv;
    static constexpr int Offset() { return offsetof(MouseButtonPresser, click); }

    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<Click> click;

  struct State : OnOff {
    using Parent = MouseButtonPresser;
    static constexpr StrView kName = "State"sv;
    static constexpr int Offset() { return offsetof(MouseButtonPresser, state); }

    bool IsOn() const { return object().down; }
    void OnTurnOn();
    void OnTurnOff();
  };
  OnOff::Def<State> state;

  struct NextImpl : NextArg {
    using Parent = MouseButtonPresser;
    static constexpr StrView kName = "Next"sv;
    static constexpr int Offset() { return offsetof(MouseButtonPresser, next); }
  };
  NextArg::Def<NextImpl> next;

  MouseButtonPresser(ui::PointerButton button);
  MouseButtonPresser();
  ~MouseButtonPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  INTERFACES(next, click, state)
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
