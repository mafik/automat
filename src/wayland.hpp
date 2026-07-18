#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Public interface to Automat's Wayland compositor.

#include <include/core/SkImage.h>
#include <include/core/SkPath.h>
#include <include/core/SkPoint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSize.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "animation.hpp"
#include "base.hpp"
#include "int.hpp"
#include "launcher.hpp"
#include "mortal.hpp"
#include "ptr.hpp"
#include "status.hpp"
#include "str.hpp"
#include "vec.hpp"
#include "window_frame.hpp"

namespace automat::mux {
struct Epoll;
}  // namespace automat::mux

namespace automat::wayland {
struct Surface;  // compositor handle; the window object holds a MortalPtr to it

// Thread-aware part of wayland::Client
struct ClientObject : ReferenceCounted {
  // The cursor shape the client last requested over this window.
  // See: wp_cursor_shape_device_v1.
  std::atomic<uint32_t> cursor_shape{1};
};
}  // namespace automat::wayland

namespace automat::library {

// Kept alive by the wayland::Surface
struct WaylandSurface : Object {
  mutable std::mutex mutex;  // guards everything below

  WeakPtr<wayland::ClientObject> client_object;
  sk_sp<SkImage> image;
  SkRect src_crop = SkRect::MakeEmpty();  // wp_viewport crop
  SkISize dst_size = {};                  // pixels
  SkIRect geo = SkIRect::MakeEmpty();     // xdg window geometry (whole buffer when unset)
  // The surface's input region, in client pixels (a closed path; empty means the
  // surface takes no pointer input, e.g. a render-only content subsurface). The
  // toy maps it to its Shape, so Automat's own hit-testing routes input to the
  // right surface and the compositor does no hit-testing of its own.
  SkPath input_region;

  // Accessed only from the Wayland (epoll) thread.
  MortalPtr<wayland::Surface> surface_handle;

  // A child surface and its placement within this surface (client pixels). The
  // placement lives on the edge, not the child, so the toy reads it and the
  // stack as one consistent snapshot under this surface's mutex.
  //
  // Subsurfaces sit at `offset`. Popups carry their positioner instead: `offset`
  // is the unconstrained anchor position and `flipped` its mirror about the
  // anchor; the flags say which adjustments the client permits. The compositor
  // does not fit a popup to the window - the toy keeps it within the on-screen
  // viewport, which only the UI layer knows since the window is movable and
  // zoomable - so a popup may extend past the window edge.
  struct Child {
    Ptr<WaylandSurface> surface;
    SkIPoint offset = {};
    bool is_popup = false;
    SkIPoint flipped = {};
    bool flip_x = false, flip_y = false, slide_x = false, slide_y = false;
  };
  Vec<Child> stack;  // Back-to-front child surfaces
  // Splits the stack: [0, i) below this surface's own content, [i, size) above.
  int stack_self_i = 0;

  WaylandSurface() = default;
  WaylandSurface(const WaylandSurface&) = delete;

  StrView Name() const override { return "Wayland Surface"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(WaylandSurface); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

// A top-level window
//
// Lifetime controlled by the client:
// - unmapping/exiting removes the object,
// - deleting the object asks the client to close.
struct WaylandWindow : ClientWindow {
  Ptr<WaylandSurface> surface;  // Root content surface

  // Compositor-side identity; only the wayland thread dereferences it.
  std::atomic<void*> toplevel_handle{nullptr};

  std::atomic<bool> server_side_decorated{false};

  WaylandWindow() = default;
  WaylandWindow(const WaylandWindow& o) : ClientWindow(o) {}
  ~WaylandWindow() override;

  void DecorationPreferenceChanged() override;

  StrView Name() const override { return "Wayland Window"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library

namespace automat::wayland {

void Start(mux::Epoll&, Status&);  // pick a free WAYLAND_DISPLAY, flock its lock, serve on epoll
void Stop();       // controlled teardown (frame timer unregisters before mux::epoll dies)
Str SocketName();  // bound display name, empty if not started
void Tick();       // per-frame board reconcile, no-op if not started

}  // namespace automat::wayland
