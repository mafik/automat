#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

// This file contains shared code for mouse-related objects.

#include <include/effects/SkRuntimeEffect.h>

#include "base.hpp"

namespace automat::library::mouse {

SkRuntimeEffect& GetPixelGridRuntimeEffect();

}  // namespace automat::library::mouse

namespace automat::library {

extern constinit Interface::Table kMouseDownMenu;
extern constinit Interface::Table kMouseUpMenu;
extern constinit Interface::Table kMousePresserMenu;
extern constinit Interface::Table kMouseScrollMenu;

// Mouse is an object that really does nothing, except offering access into other mouse-related
// objects.
struct Mouse : Object {
  string_view Name() const override { return "Mouse"; }
  Ptr<Object> Clone() const override;
  INTERFACES(kMouseDownMenu, kMouseUpMenu, kMousePresserMenu, kMouseScrollMenu);
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

struct MouseButtonEvent : Object {
  ui::PointerButton button;
  bool down;

  DEF_INTERFACE(MouseButtonEvent, Command, run, "Run")
  void OnRun() {
    obj->Run();
    Done();
  }
  DEF_END(run);

  DEF_INTERFACE(MouseButtonEvent, NextArg, next, "Next")
  DEF_END(next);

  MouseButtonEvent(ui::PointerButton button, bool down) : button(button), down(down) {}
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  INTERFACES(run_tbl, next_tbl);
  audio::Sound& NextSound() override;
  void Run();
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

  DEF_INTERFACE(MouseButtonPresser, Command, click, "Click")
  void OnRun() {
    obj->Click();
    Done();
  }
  DEF_END(click);

  DEF_INTERFACE(MouseButtonPresser, OnOff, state, "State")
  bool IsOn() const { return obj->down; }
  void OnTurnOn() { obj->Press(); }
  void OnTurnOff() { obj->Release(); }
  DEF_END(state);

  DEF_INTERFACE(MouseButtonPresser, NextArg, next, "Next")
  DEF_END(next);

  MouseButtonPresser(ui::PointerButton button);
  MouseButtonPresser();
  ~MouseButtonPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  INTERFACES(next_tbl, state_tbl, click_tbl);
  void Click();
  void Press();
  void Release();
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
