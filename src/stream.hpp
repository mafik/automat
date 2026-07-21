#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// Stream ports, as designed in docs/parrots/Pipeline Language.md.
//
// A stream connection carries a sequence of buffers between two objects. The
// connection itself is Automat-side topology only - the actual data transport
// is arranged by the owning objects (a kernel pipe wired at spawn, a GStreamer
// pad link, ...). What the generic layer provides:
//
// - StreamInput: an interface marking a stream input port on an Object.
// - StreamArgument: a stream output port; an InterfaceArgument that connects
//   downward to a StreamInput and renders as a pipe (Style::Stream).
// - Transfer totals for the connection's meters: either the State counters
//   (bumped by the producing object) or an OnStats override for objects that
//   meter through the kernel (/proc/pid/io, FIONREAD).

#include <atomic>
#include <mutex>

#include "argument.hpp"

namespace automat {

// Which side of a stream is blocked on it, when that is knowable (for a
// kernel pipe: /proc/pid/syscall names the blocked syscall and its fd).
enum class StreamBlocked : uint8_t {
  None,
  Producer,  // backpressured: blocked writing into a full stream
  Consumer,  // starved: blocked reading from an empty stream
};

// A snapshot of a stream's meters, read on every UI tick. `bytes` and
// `units` are cumulative totals since the stream started; rates are derived
// by the UI with RateEstimator. `units` counts library-specific quanta
// (buffers, frames, lines); zero means bytes are the only unit. `fill` and
// `capacity` describe the stream's buffer when it has one (a kernel pipe, a
// queue); capacity zero means unknown. They are byte counts unless
// `fill_unit` names the library's own quantum ("packets", "buffers").
struct StreamStats {
  uint64_t bytes = 0;
  uint64_t units = 0;
  uint64_t fill = 0;
  uint64_t capacity = 0;
  StrView fill_unit = {};
  StreamBlocked blocked = StreamBlocked::None;
};

struct StreamInput : Interface {
  struct State {
    mutable std::mutex mutex;
    WeakPtr<Object> producer;
    State() = default;
    State(const State&) {}
  };

  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kStreamInput; }

    // Called on the UI thread; must be cheap and thread-safe.
    Str (*format)(StreamInput) = [](StreamInput) { return Str(); };

    constexpr Table(StrView name) : Interface::Table(Interface::kStreamInput, name) {}

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      if constexpr (requires(ImplT& i) {
                      { i.OnFormat() } -> std::convertible_to<Str>;
                    })
        format = [](StreamInput self) -> Str { return static_cast<ImplT&>(self).OnFormat(); };
    }
  };

  INTERFACE_BOUND(StreamInput, Interface)

  Str Format() const { return table->format(*this); }

  Ptr<Object> Producer() const {
    auto lock = std::lock_guard(state->mutex);
    return state->producer.Lock();
  }
  void SetProducer(WeakPtr<Object> producer) const {
    auto lock = std::lock_guard(state->mutex);
    state->producer = std::move(producer);
  }

  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = StreamInput;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

struct StreamArgument : InterfaceArgument<StreamInput, Interface::kStreamArg> {
  using BaseArg = InterfaceArgument<StreamInput, Interface::kStreamArg>;

  struct State : BaseArg::State {
    // Producer-side counters; any thread may bump them (Record).
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> units{0};
    State() = default;
    State(const State& o) : BaseArg::State(o), bytes(0), units(0) {}
  };

  struct Table : BaseArg::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kStreamArg; }

    // Stores the target and maintains the input's producer back-pointer.
    // Impls that override OnConnect should call this first.
    static void StreamOnConnect(Argument self, Interface end) {
      auto stream_self = cast<StreamArgument>(self);
      if (auto old = stream_self.state->target.Lock()) {
        StreamInput old_input(old.Owner<Object>(), old.Get());
        if (old_input && old_input.Producer().get() == self.object_ptr) {
          old_input.SetProducer({});
        }
      }
      stream_self.state->target = dyn_cast_if_present<StreamInput>(end);
      if (auto input = dyn_cast_if_present<StreamInput>(end)) {
        input.SetProducer(self.object_ptr->AcquireWeakPtr());
      }
    }

    // Format of the flowing data in the library's notation. Empty until
    // negotiated. Called on the UI thread; must be cheap and thread-safe.
    Str (*format)(StreamArgument) = [](StreamArgument) { return Str(); };

    StreamStats (*stats)(StreamArgument) = [](StreamArgument self) {
      return StreamStats{
          .bytes = self.state->bytes.load(std::memory_order_relaxed),
          .units = self.state->units.load(std::memory_order_relaxed),
      };
    };

    constexpr Table(StrView name) : BaseArg::Table(name) {
      style = Style::Stream;
      on_connect = &StreamOnConnect;
      autoconnect_radius = 6_cm;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Argument::Table::FillFrom<ImplT>();
      if constexpr (requires(ImplT& i) {
                      { i.OnFormat() } -> std::convertible_to<Str>;
                    })
        format = [](StreamArgument self) -> Str { return static_cast<ImplT&>(self).OnFormat(); };
      if constexpr (requires(ImplT& i) {
                      { i.OnStats() } -> std::convertible_to<StreamStats>;
                    })
        stats = [](StreamArgument self) -> StreamStats {
          return static_cast<ImplT&>(self).OnStats();
        };
    }
  };

  INTERFACE_BOUND(StreamArgument, BaseArg)

  Str Format() const { return table->format(*this); }
  StreamStats Stats() const { return table->stats(*this); }

  // Producer-side accounting for objects that pass buffers through Automat.
  void Record(uint64_t bytes, uint64_t units = 1) {
    state->bytes.fetch_add(bytes, std::memory_order_relaxed);
    state->units.fetch_add(units, std::memory_order_relaxed);
    state->last_activity.fetch_add(1, std::memory_order_relaxed);
  }

  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = StreamArgument;

    bool OnIsConnected() { return !this->target.IsExpired(); }

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.template FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

struct StreamOutSlot {
  StreamArgument::Table table{""};
  StreamArgument::State state;
  Str name;

  void Init(Str port_name, int state_off) {
    name = std::move(port_name);
    table = StreamArgument::Table(name);
    table.state_off = state_off;
  }
};

struct StreamInSlot {
  StreamInput::Table table{""};
  StreamInput::State state;
  Str name;

  void Init(Str port_name, int state_off) {
    name = std::move(port_name);
    table = StreamInput::Table(name);
    table.state_off = state_off;
  }
};

struct RateEstimator {
  constexpr static int kSamples = 8;
  constexpr static double kMinSampleInterval = 0.125;  // seconds

  struct Sample {
    double time = 0;
    uint64_t total = 0;
  };
  Sample ring[kSamples] = {};
  int count = 0;  // samples stored so far (saturates at kSamples)
  int next = 0;   // ring write position

  // Returns the current per-second rate.
  double Update(double now, uint64_t total) {
    if (count > 0) {
      const Sample& last = ring[(next + kSamples - 1) % kSamples];
      if (total < last.total) {  // counter reset (process restarted)
        count = 0;
        next = 0;
      } else if (now - last.time < kMinSampleInterval) {
        return Rate(now, total);
      }
    }
    ring[next] = {now, total};
    next = (next + 1) % kSamples;
    if (count < kSamples) ++count;
    return Rate(now, total);
  }

  double Rate(double now, uint64_t total) const {
    if (count == 0) return 0;
    const Sample& oldest = ring[count < kSamples ? 0 : next];
    double dt = now - oldest.time;
    if (dt <= 0) return 0;
    return (double)(total - oldest.total) / dt;
  }
};

// Formats a bytes-per-second rate as a short human-readable label ("1.2 MB/s").
Str FormatBytesPerSecond(double bytes_per_second);

// Formats a byte count as a short binary-unit label ("64 KiB", "1.2 MiB").
Str FormatBytes(uint64_t bytes);

}  // namespace automat
