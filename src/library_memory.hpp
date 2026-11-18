#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "mem_meld.hpp"

namespace automat::library {

struct Memory : Object {
  MemMeld mem_meld;
  MemMeld::Region last_region;

  DEF_INTERFACE(Memory, Command, start, "Start")
  void OnRun() { obj->Start(); }
  DEF_END(start);

  INTERFACES(start_tbl);

  Memory();
  string_view Name() const override { return "Memory Viewer"sv; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Memory); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void Start();
};

}  // namespace automat::library