#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <atomic>
#include <mutex>

#include "base.hpp"
#include "str.hpp"

namespace automat::library {

// A proxy for one node in the PipeWire daemon's graph, as designed in
// docs/parrots/Pipeline Language.md: the node belongs to the daemon and is
// already live; the board object mirrors it. The face shows the node's name,
// media.class, state (in PipeWire's own words), and a peak meter fed by a
// small capture stream attached to the node.
struct PipeWireNode : Object {
  mutable std::mutex mutex;  // guards node_name and the mirrored state below

  Str node_name;  // recipe data: the node.name this object proxies

  // Mirrored from the daemon, guarded by `mutex`:
  Str media_class;
  Str state_word = "absent";  // suspended / idle / running; absent = not in the graph
  bool daemon = false;        // whether a daemon connection exists at all

  // Peak of the capture stream, 0..1, written on the PipeWire loop thread.
  std::atomic<float> vu{0};

  // The VU capture stream (a VuStream*), managed on the PipeWire loop.
  void* stream = nullptr;

  PipeWireNode() = default;
  PipeWireNode(const PipeWireNode& o) : Object(o), node_name(o.node_name) {}
  ~PipeWireNode() override;

  StrView Name() const override { return "pipewire:node"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(PipeWireNode, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  // Points the proxy at a daemon node and re-attaches the capture stream.
  void SetNodeName(StrView name);
  // Refreshes the mirrored fields from the daemon's registry. Called on
  // every toy tick; cheap (one map lookup under the mirror mutex).
  void RefreshFromMirror();
};

}  // namespace automat::library
