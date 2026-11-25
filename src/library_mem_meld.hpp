#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <thread>

#include "argument.hpp"
#include "base.hpp"
#include "crop_tool.hpp"
#include "library_process.hpp"
#include "mem_meld.hpp"
#include "once.hpp"

namespace automat::library {

struct MemMeld : Object {
  automat::MemMeld mem_meld;
  // This object was made so that its lifetime matches mem_meld.Attach()-Detach(). This feels wrong.
  // TODO: this object should probably be restartable (through On/Off interface)
  Once start_once;

  DEF_INTERFACE(MemMeld, ObjectArgument<Process>, process, "Process")
  static constexpr bool kVisibleWhenDisconnected = false;
  void OnConnect(Interface end);
  std::unique_ptr<ArgumentToy> OnMakeToy(ui::Widget* parent);
  void OnDestroyToy(std::unique_ptr<automat::Toy> toy);
  DEF_END(process);

  DEF_INTERFACE(MemMeld, Command, start, "Start")
  void OnRun() { obj->Start(); }
  DEF_END(start);

  struct CropParameters;

  DEF_INTERFACE(MemMeld, Croppable, croppable, "Crop")
  void CropViewSerialize(const CropView&, ObjectSerializer&);
  void CropViewDeserialize(CropView&, ObjectDeserializer&);
  std::unique_ptr<CropViewParameters> CropViewMakeParameters();
  void CropScissorsOpen(CropView&, automat::Toy& croppable_toy, CropScissors&, Vec2 point);
  void CropScissorsMove(CropView&, automat::Toy& croppable_toy, CropScissors&, Vec2 point);
  ui::Tock CropScissorsTick(CropView&, automat::Toy& croppable_toy, CropScissors&, time::Timer&);
  ui::Tock CropScissorsClose(CropScissors&, time::Timer&);
  std::unique_ptr<automat::Toy> CropMakeToy(CropView&, ui::Widget* parent);
  ui::Tock CropTickToy(CropView&, automat::Toy& cropped_toy, time::Timer&);
  DEF_END(croppable);

  INTERFACES(process_tbl, start_tbl, croppable_tbl);

  MemMeld() = default;
  MemMeld(const MemMeld&);
  ~MemMeld() override;
  string_view Name() const override { return "MemMeld"sv; }
  // TODO: allow for non-copy-able objects?
  Ptr<Object> Clone() const override { return MAKE_PTR(MemMeld, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  static std::unique_ptr<Toy> MakeDeadToy(ui::Widget* parent);

  void Start();

  // HELPER THREAD: MemMeld::Attach cannot use epoll because it's slower than raw wait().
  // Benchmarks show a +10% difference in speed.
  std::jthread attach_thread;
};

}  // namespace automat::library
