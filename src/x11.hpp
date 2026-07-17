#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Public interface to Automat's X11 server. The design mirrors wayland.hpp: an X11
// window a client maps becomes a board object, and a small set of lifecycle calls run
// the server on the shared mux::Epoll thread.

#include <include/core/SkImage.h>
#include <include/core/SkPath.h>
#include <include/core/SkRect.h>
#include <include/core/SkSize.h>

#include <atomic>
#include <mutex>

#include "base.hpp"
#include "int.hpp"
#include "mortal.hpp"
#include "ptr.hpp"
#include "status.hpp"
#include "str.hpp"
#include "vec.hpp"
#include "window_frame.hpp"

namespace automat::mux {
struct Epoll;
}  // namespace automat::mux

namespace automat::x11 {
struct Window;  // server-side window resource; the board object holds a MortalPtr to it
}  // namespace automat::x11

namespace automat::library {

// A top-level X11 window shown on the board.
//
// Lifetime controlled by the client:
// - unmapping/destroying removes the object,
// - deleting the object asks the client to close (WM_DELETE_WINDOW, then a kill).
struct X11Window : Object, DecoratedWindow {
  mutable std::mutex mutex;  // guards the non-atomic fields below

  Str title;
  Str app_id;  // WM_CLASS instance/class
  bool client_gone = false;
  bool override_redirect = false;  // menus/tooltips: drawn without chrome
  bool client_decorated = false;   // the client draws its own frame (Motif hints / GTK CSD)

  // The composited snapshot of this window's tree and its size in client pixels.
  sk_sp<SkImage> image;
  SkISize content_size = {};
  SkPath input_region;  // client pixels (the whole window; shaped windows narrow it)

  // Recipe-level persistence: the argv whose child mapped this window. Serialized;
  // deserializing (or cloning) re-runs it and the new client is adopted.
  Vec<Str> recipe;
  I64 client_pid = 0;
  bool pending_respawn = false;

  // Server-side identity; only the x11 thread dereferences it.
  std::atomic<void*> window_handle{nullptr};

  DEF_INTERFACE(X11Window, ObjectArgument<Object>, launcher, "Launcher")
  static constexpr auto kStyle = Argument::Style::Cable;
  static constexpr float kAutoconnectRadius = 0.f;
  DEF_END(launcher);

  INTERFACES(launcher);

  X11Window() = default;
  X11Window(const X11Window& o) : launcher(o.launcher) {
    auto lock = std::lock_guard(o.mutex);
    recipe = o.recipe;
    title = o.title;
    app_id = o.app_id;
    client_gone = true;
    pending_respawn = !recipe.empty();
    decoration_preference.store(o.decoration_preference.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
  }
  ~X11Window() override;

  void DecorationPreferenceChanged() override;

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  StrView Name() const override { return "X11 Window"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(X11Window, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library

namespace automat::x11 {

void Start(mux::Epoll&, Status&);  // pick a free DISPLAY, listen on epoll
void Stop();                       // controlled teardown
Str SocketName();                  // bound display name (e.g. ":3"), empty if not started
void UIFrame();                    // per-frame board reconcile, no-op if not started

}  // namespace automat::x11
