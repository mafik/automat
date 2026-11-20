#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <atomic>

#include "base.hpp"
#include "int.hpp"
#include "object_source.hpp"
#include "str.hpp"

namespace automat::library {

struct Process : Object {
  struct Watch;

  I64 pid = 0;  // TODO: track with pidfd instead
  Str start_time;
  std::atomic<bool> exited = false;
  Watch* watch = nullptr;
  WeakPtr<Object> mem_meld;

  Process() = default;
  Process(I64 pid);
  Process(const Process&);
  ~Process() override;

  StrView Name() const override { return "Process"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Process, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
  void OnUnsuspend() override;
};

extern ObjectSource::Table kMakeMemMeld;

}  // namespace automat::library
