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
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override;
};

struct MouseButtonEvent : Object, Runnable {
  ui::PointerButton button;
  bool down;
  MouseButtonEvent(ui::PointerButton button, bool down) : button(button), down(down) {}
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location&, RunTask&) override;
  audio::Sound& NextSound() override;
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override;

  void SerializeState(Serializer& writer, const char* key = "value") const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

struct MouseMove : Object {
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override;
  void OnMouseMove(Vec2);
};

struct MouseButtonPresser : Object, Runnable, LongRunning {
  ui::PointerButton button;

  MouseButtonPresser(ui::PointerButton button);
  MouseButtonPresser();
  ~MouseButtonPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override;

  void OnRun(Location& here, RunTask& run_task) override;
  void OnCancel() override;
  LongRunning* AsLongRunning() override { return this; }

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library