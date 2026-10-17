#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <atomic>
#include <cstdint>

#include "base.hpp"
#include "path.hpp"
#include "status.hpp"
#include "str.hpp"
#include "vec.hpp"

#if defined(_WIN32)
struct IDataObject;
#endif

namespace automat::library {

struct DataOffer : Object {
  struct Entry {
    Str name;  // immutable once published; the only field the UI thread reads

    Path source_path;      // local file backing a uri copy/move, empty for octet
    Path dst;              // destination inside AutomatDir
    bool spawned = false;  // a File has been created for this entry

    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};  // 0 => indeterminate
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};

#if defined(__linux__)
    uint32_t type = 0;
    uint32_t property = 0;
    uint32_t time = 0;
    Str data;  // accumulating octet-stream bytes
    bool incremental = false;
#endif

    Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
  };

  static constexpr int kMaxEntries = 64;
  Entry entries[kMaxEntries];
  std::atomic<uint32_t> entry_count{0};
  std::atomic<bool> sealed{false};
  std::atomic<bool> cancelled{false};
  bool dropped = false;

#if defined(__linux__)
  uint32_t source = 0;  // drag source window (xcb thread only)
  uint32_t action = 0;  // requested XdndAction atom (xcb thread only)
  uint32_t target = 0;  // drop target window (xcb thread only)
  Vec<uint32_t> types;
  Str xds_name;
  bool started = false;
#endif
#if defined(_WIN32)
  std::atomic<unsigned long> source_cookie{0};
  bool async_started = false;
#endif

  StrView Name() const override { return "Data Offer"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(DataOffer); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  int EntryCount() const { return (int)entry_count.load(std::memory_order_acquire); }
  bool Done() const;
  bool Accepted() const;

  Entry* Add(StrView name);
  void AddPath(const Path& source);

  // Functions for managing the lifetime of the drag action.
  //
  // IMPORTANT: "lifetime of the mouse drag" != "lifetime of data transfer"
  //
  // These functions are for managing the former. Data transfer may have finished early (typically)
  // but may also continue even after DataOffer is dropped onto a Board.
  void BeginDragAction(ui::Pointer&);
  void CancelDragAction(ui::Pointer&);
  void EndDragAction(ui::Pointer&);

  // Called by the source responsible for writing some entry.
  //
  // Calls TryFinishDrop so you don't have to.
  void EntryWriteOver(Entry&, Status&);

  // Called when the conditions change that allow the data transfer to be finished.
  void TryFinishDrop();

#if defined(_WIN32)
  ~DataOffer() override;
  bool Receive(IDataObject&);
#endif
};

}  // namespace automat::library
