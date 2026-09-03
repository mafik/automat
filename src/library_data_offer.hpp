#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <atomic>
#include <cstdint>

#include "base.hpp"
#include "path.hpp"
#include "str.hpp"
#include "vec.hpp"

namespace automat::library {

struct DataOffer : Object {
  struct Entry {
    Str name;  // immutable once published; the only field the UI thread reads

    uint32_t source = 0;
    uint32_t type = 0;
    uint32_t property = 0;
    uint32_t time = 0;
    Str data;          // accumulating octet-stream bytes
    Path source_path;  // local file backing a uri copy/move, empty for octet
    Path dst;          // destination inside AutomatDir
    bool incremental = false;
    bool spawned = false;  // a File has been created for this entry

    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};  // 0 => indeterminate
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};

    Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
  };

  static constexpr int kMaxEntries = 64;
  Entry entries[kMaxEntries];
  std::atomic<uint32_t> entry_count{0};

  uint32_t source = 0;  // drag source window (xcb thread only)
  uint32_t action = 0;  // requested XdndAction atom (xcb thread only)
  Vec<uint32_t> types;
  Str xds_name;
  bool started = false;
  bool sealed = false;
  bool dropped = false;  // xcb thread only

  StrView Name() const override { return "Data Offer"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(DataOffer); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  int Count() const { return (int)entry_count.load(std::memory_order_acquire); }

  Entry* Add(StrView name);

  void Complete(Entry&, bool success);

  bool Done() const;
};

}  // namespace automat::library
