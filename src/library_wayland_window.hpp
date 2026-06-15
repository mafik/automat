#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>

#include <atomic>

#include "base.hpp"
#include "str.hpp"
#include "vec.hpp"

namespace automat::library {

// Board-metric size of a window object showing a width x height client
// surface (content + chrome). Used to seat new windows next to their Command.
Vec2 WindowBoardSize(int width, int height);

// One mapped Wayland toplevel, shown as an object on the board. The window's
// life is bound to the client: the client unmapping/exiting removes the
// object, and deleting the object asks the client to close.
struct WaylandWindow : Object {
  mutable std::mutex mutex;  // guards everything below

  // The latest committed frame as an SkImage; the client does the rendering, so
  // the read side is just an image. An shm buffer is row-copied into a pooled
  // raster image and a dmabuf is imported to a GPU texture, both on the
  // compositor thread at commit time.
  sk_sp<SkImage> content;
  // Surface size in client pixels. Drives the window's board size and the
  // surface-local mapping of pointer input. Empty until the first frame.
  SkISize viewport_destination_px = {};
  // Rectangle of `content` to display, in buffer pixels - the whole buffer
  // unless a wp_viewport crops it.
  SkRect viewport_source_px = {};
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
