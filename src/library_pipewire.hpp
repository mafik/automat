#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <atomic>
#include <mutex>

#include "base.hpp"
#include "str.hpp"
#include "stream.hpp"

namespace automat::library {

// A proxy for one node in the PipeWire daemon's graph, as designed in
// docs/parrots/Pipeline Language.md: the node belongs to the daemon and is
// already live; the board object mirrors it. The face shows the node's name,
// media.class, state, the negotiated format, a peak
// meter fed by a small capture stream, and volume/mute instruments backed by
// SPA Props. Stream ports mirror the node's port directions; connecting two
// proxies creates daemon links (object.linger, so they outlive Automat) and
// links made by others (WirePlumber policy) appear as board connections.
struct PipeWireNode : Object {
  mutable std::mutex mutex;  // guards node_name and the mirrored state below

  Str node_name;  // recipe data: the node.name this object proxies

  // Mirrored from the daemon, guarded by `mutex`:
  Str media_class;
  Str state_word = "absent";  // suspended / idle / running; absent = not in the graph
  Str format;                 // negotiated format, in PipeWire's notation
  bool daemon = false;        // whether a daemon connection exists at all
  bool has_in_ports = false;
  bool has_out_ports = false;
  bool has_volume = false;
  bool has_mute = false;
  float volume = 0;  // cubic-scaled, as wpctl prints it; linear in the daemon
  bool mute = false;

  // Whether the out connection has been realized as daemon links. Until it
  // is, the sweep keeps trying to create them (the peer may not be in the
  // graph yet); once it is, their disappearance means another tool destroyed
  // them and the board connection follows.
  bool out_links_realized = false;

  // Peak of the capture stream, 0..1, written on the PipeWire loop thread.
  std::atomic<float> vu{0};

  // The VU capture stream (a VuStream*), managed on the PipeWire loop. It
  // exists only while the proxy sits on a board (RefreshFromMirror keeps it
  // in step with `here`): shelf entries and toolbar prototypes must not open
  // daemon streams - a capture stream is itself a node, so a shelf that
  // attached one per entry would list its own streams and grow without end.
  void* stream = nullptr;

  DEF_INTERFACE(PipeWireNode, StreamInput, in_stream, "input")
  Str OnFormat() { return obj->FormatLabel(); }
  DEF_END(in_stream);

  DEF_INTERFACE(PipeWireNode, StreamArgument, out_stream, "output")
  Str OnFormat() { return obj->FormatLabel(); }
  void OnCanConnect(Interface end, Status& status) { obj->CanFeed(*this, end, status); }
  void OnConnect(Interface end) { obj->OnOutConnect(*this, end); }
  DEF_END(out_stream);

  INTERFACES(in_stream, out_stream);

  PipeWireNode() = default;
  PipeWireNode(const PipeWireNode& o)
      : Object(o), node_name(o.node_name), in_stream(o.in_stream), out_stream(o.out_stream) {}
  ~PipeWireNode() override;

  StrView Name() const override { return "pipewire:node"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(PipeWireNode, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  // Points the proxy at a daemon node; the capture stream follows on the
  // next tick.
  void SetNodeName(StrView name);
  Str NodeName() const;
  // Refreshes the mirrored fields from the daemon's registry. Called on
  // every toy tick; cheap (a few short scans under the mirror mutex).
  void RefreshFromMirror();

  // Writes SPA Props on the daemon node. `v` is the cubic-scaled value the
  // slider shows; the daemon stores channelVolumes linear (v cubed).
  void SetVolume(float v);
  void SetMute(bool m);

  Str FormatLabel() const;

  // The out port's link check: only PipeWire nodes can be linked, and both
  // nodes must be in the daemon's graph with ports in the right directions.
  void CanFeed(StreamArgument self, Interface end, Status& status);
  // The out port's connect handler: stores the target, then makes the board
  // change real - daemon links to the old target are destroyed, links to the
  // new target are created.
  void OnOutConnect(StreamArgument self, Interface end);
  // Mirrors the daemon's links onto the board connection, both ways: a
  // connection whose daemon links were destroyed disconnects, and a daemon
  // link whose peer has a proxy on the same board appears as a connection.
  // Called on every toy tick (UI thread).
  void SyncBoardLinks();
};

// The PipeWire shelf lists the live nodes of the running system, grouped by
// media.class and shown as their proxies' own faces (so each entry carries
// its current state). PipeWire objects are not prototypes to instantiate but
// existing things to use: dragging an entry out places that node's proxy on
// the board. The shelf follows the daemon - nodes appearing and disappearing
// rebuild it.
struct PipeWireShelf : Object {
  StrView Name() const override { return "PipeWire"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(PipeWireShelf); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

// Disconnects from the daemon and joins the mirror thread.
void StopPipeWire();

}  // namespace automat::library
