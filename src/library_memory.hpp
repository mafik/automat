#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "mem_meld.hpp"

namespace automat::library {

struct MemMeld : Object {
  automat::MemMeld mem_meld;

  DEF_INTERFACE(MemMeld, Command, start, "Start")
  void OnRun() { obj->Start(); }
  DEF_END(start);

  INTERFACES(start_tbl);

  MemMeld();
  string_view Name() const override { return "Memory Viewer"sv; }
  Ptr<Object> Clone() const override { return MAKE_PTR(MemMeld); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void Start();
};

}  // namespace automat::library