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

  // The latest committed frame. Raster-backed today (the compositor thread
  // copies each wl_shm buffer once, into a pooled allocation the image wraps
  // without further copies); a GPU-backed image from a dmabuf import can
  // land in the same field later. SkImage is the read-side type on purpose -
  // an SkSurface is a render target, and the client does the rendering.
  sk_sp<SkImage> content;
  int width = 0, height = 0;  // client-surface size, px
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
