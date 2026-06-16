#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>
#include <include/core/SkPoint.h>

#include <atomic>

#include "base.hpp"
#include "str.hpp"
#include "vec.hpp"

namespace automat::library {

// Board-metric size of a window object showing a width x height client
// surface (content + chrome). Used to seat new windows next to their Command.
Vec2 WindowBoardSize(int width, int height);

// One surface in a window's composited tree: the toplevel (layer 0) plus any
// subsurfaces. Layers are drawn back-to-front; `image` is sampled through
// `source` (the wp_viewport crop, or the whole buffer) into a `size`-pixel
// rectangle placed at `position` within the window, all in client pixels.
struct WindowLayer {
  sk_sp<SkImage> image;
  SkRect source;
  SkIPoint position;
  SkISize size;
};

// One mapped Wayland toplevel, shown as an object on the board. The window's
// life is bound to the client: the client unmapping/exiting removes the
// object, and deleting the object asks the client to close.
struct WaylandWindow : Object {
  mutable std::mutex mutex;  // guards everything below

  // The committed frame as a back-to-front list of layers (toplevel surface plus
  // any subsurfaces); the client does the rendering, so the read side is just
  // images. Each surface's shm buffer is row-copied into a pooled raster image
  // and each dmabuf imported to a GPU texture, on the compositor thread at commit
  // time. Empty until the first frame.
  Vec<WindowLayer> layers;
  // Window extent in client pixels (the toplevel surface size). Drives the
  // window's board size and the surface-local mapping of pointer input. Empty
  // until the first frame.
  SkISize viewport_destination_px = {};
  uint64_t content_serial = 0;
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

  // The Command whose child mapped this window. Connected at adoption, drawn
  // as a cable, serialized as a link; restore-time respawn goes through it so
  // the Command keeps STOP control over the new child.
  DEF_INTERFACE(WaylandWindow, ObjectArgument<Object>, launcher, "Launcher")
  static constexpr auto kStyle = Argument::Style::Cable;
  static constexpr float kAutoconnectRadius = 0.f;
  DEF_END(launcher);

  INTERFACES(launcher);

  WaylandWindow() = default;
  WaylandWindow(const WaylandWindow& o) : Object(), launcher(o.launcher) {
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
