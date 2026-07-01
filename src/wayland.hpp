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
#include "mortal.hpp"
#include "ptr.hpp"
#include "status.hpp"
#include "str.hpp"
#include "vec.hpp"

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

namespace automat::ui {
struct PointerObject;
}  // namespace automat::ui

namespace automat::library {

// Kept alive by the wayland::Surface
struct WaylandSurface : Object {
  mutable std::mutex mutex;  // guards everything below

  WeakPtr<wayland::ClientObject> client_object;
  sk_sp<SkImage> image;
  SkRect src_crop = SkRect::MakeEmpty();  // wp_viewport crop
  SkISize dst_size = {};                  // pixels
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
    Optional<animation::SpringV2<Vec2>> offset_animated = std::nullopt;
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
struct WaylandWindow : Object {
  mutable std::mutex mutex;  // guards non-atomics below

  Ptr<WaylandSurface> surface;  // Root content surface

  Str title;
  Str app_id;
  bool client_gone = false;

  // Recipe-level persistence: the argv whose child mapped this window.
  // Serialized; deserializing (or cloning) re-runs it and the new client is
  // adopted into this object.
  Vec<Str> recipe;
  I64 client_pid = 0;
  bool pending_respawn = false;

  // Compositor-side identity; only the wayland thread dereferences it.
  std::atomic<void*> toplevel_handle{nullptr};

  enum class DecorationPreference { Auto = 0, ServerSide = 1, ClientSide = 2 };
  std::atomic<DecorationPreference> decoration_preference{DecorationPreference::Auto};

  std::atomic<bool> server_side_decorated{false};

  // The Command whose child mapped this window. Connected at adoption, drawn
  // as a cable, serialized as a link; restore-time respawn goes through it so
  // the Command keeps STOP control over the new child.
  DEF_INTERFACE(WaylandWindow, ObjectArgument<Object>, launcher, "Launcher")
  static constexpr auto kStyle = Argument::Style::Cable;
  static constexpr float kAutoconnectRadius = 0.f;
  DEF_END(launcher);

  INTERFACES(launcher);

  WaylandWindow() = default;
  WaylandWindow(const WaylandWindow& o) : launcher(o.launcher) {
    auto lock = std::lock_guard(o.mutex);
    recipe = o.recipe;
    title = o.title;
    app_id = o.app_id;
    client_gone = true;
    pending_respawn = !recipe.empty();
  }
  ~WaylandWindow() override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  StrView Name() const override { return "Wayland Window"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(WaylandWindow, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library

namespace automat::wayland {

void Start(mux::Epoll&, Status&);  // pick a free WAYLAND_DISPLAY, flock its lock, serve on epoll
void Stop();       // controlled teardown (frame timer unregisters before mux::epoll dies)
Str SocketName();  // bound display name, empty if not started
void UIFrame();    // per-frame board reconcile, no-op if not started

void RegisterPointer(ui::PointerObject&);

}  // namespace automat::wayland
