#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>
#include <include/core/SkPath.h>
#include <include/core/SkPoint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSize.h>

#include <atomic>
#include <mutex>

#include "base.hpp"
#include "str.hpp"
#include "vec.hpp"

namespace automat::library {

// Board-metric size of a window object showing a width x height client
// surface (content + chrome). Used to seat new windows next to their Command.
Vec2 WindowBoardSize(int width, int height);

struct WaylandSurface : Object {
  mutable std::mutex mutex;  // guards everything below

  sk_sp<SkImage> image;
  SkRect src_crop = SkRect::MakeEmpty();  // wp_viewport crop
  SkISize dst_size = {};                  // pixels
  // The surface's input region, in client pixels (a closed path; empty means the
  // surface takes no pointer input, e.g. a render-only content subsurface). The
  // toy maps it to its Shape, so Automat's own hit-testing routes input to the
  // right surface and the compositor does no hit-testing of its own.
  SkPath input_region;
  // The protocol wl_surface this object mirrors (a wl_resource*); the wayland
  // thread resolves it to route input to this surface's client. Null until mapped.
  std::atomic<void*> surface_handle{nullptr};

  // A child surface and its placement within this surface (client pixels). The
  // placement lives on the edge, not the child, so the toy reads it and the
  // child list as one consistent snapshot under this surface's mutex.
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
  // Child surfaces owned by this one, back-to-front, split by whether they
  // composite below or above this surface's own content. `above` also carries
  // this surface's popups, which always draw on top.
  Vec<Child> below;
  Vec<Child> above;

  WaylandSurface() = default;
  WaylandSurface(const WaylandSurface&) = delete;

  StrView Name() const override { return "Wayland Surface"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(WaylandSurface); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

// Adds window's identity, chrome and persistence on top of WaylandSurface.
//
// Lifetime controlled by the client:
// - unmapping/exiting removes the object,
// - deleting the object asks the client to close.
struct WaylandWindow : WaylandSurface {
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

  // The cursor shape the client last requested over this window.
  // See: wp_cursor_shape_device_v1.
  // The wayland thread writes it.
  std::atomic<uint32_t> cursor_shape{1};

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
  WaylandWindow(const WaylandWindow& o) : WaylandSurface(), launcher(o.launcher) {
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
