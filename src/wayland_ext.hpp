#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPath.h>
#include <include/core/SkPoint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSize.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../build/generated/wayland_protocols_forward.hpp"
#include "colony.hpp"
#include "fd.hpp"
#include "int.hpp"
#include "mortal.hpp"
#include "mux_epoll.hpp"
#include "mux_timer.hpp"
#include "optional.hpp"
#include "path.hpp"
#include "ptr.hpp"
#include "str.hpp"
#include "vec.hpp"

namespace automat::library {
struct WaylandSurface;
struct WaylandWindow;
}  // namespace automat::library

namespace automat::wayland {

struct Client;

// Common header for all Wayland struct types.
//
// Useful for dispatching commands in a generic way.
struct Common {
  Kind kind;  // LLVM-style RTTI
  U32 id;
  Client& client;  // Saves one parameter from every method call
  MortalCoil mortal_coil;
  Common(Kind kind, U32 id, Client& client);
  ~Common();
  void GenericColonyDestroy();                                       // generated
  void GenericDispatch(U32 opcode, const char* p, const char* end);  // generated
};

template <U32 StartId = 0>
struct IdPool {
  Vec<U32> free_list{};
  U32 next_id{StartId};

  U32 Grab() {
    if (free_list.empty()) {
      return next_id++;
    } else {
      int id = free_list.back();
      free_list.pop_back();
      return id;
    }
  }

  void Return(U32 id) {
    // TODO: turn `free_list` into a heap & compact it on-line
    if (id == next_id - 1) {
      --next_id;
    } else {
      free_list.push_back(id);
    }
  }
};

struct Server : mux::Epoll::Listener {
  mux::Epoll& epoll;
  Path socket_path;  // full filesystem path; unlinked on clean shutdown
  FD lock_fd;  // exclusive flock on socket_path + ".lock"; released when the Server is destroyed

  // Compositor server-level state (touched on the mux::epoll thread unless noted).
  uint32_t serial = 1;  // monotonic serial source for protocol events
  std::unique_ptr<mux::Timer> frame_timer;
  bool frame_pending = false;
  std::mutex ui_mutex;                        // guards the two handoff vectors
  Vec<Ptr<ReferenceCounted>> ui_appeared;     // mapped windows awaiting board insert
  Vec<Ptr<ReferenceCounted>> ui_disappeared;  // unmapped windows awaiting board remove
  std::mutex adoption_mutex;
  Vec<std::pair<I64, WeakPtr<ReferenceCounted>>> adoptions;  // respawned clients, by pid

  // Input focus + keymap. Cleared in the surface destructor;
  // the keymap is the host X keymap, built once.
  MortalPtr<Surface> pointer_surface;
  MortalPtr<Surface> keyboard_surface;
  MortalPtr<XdgPopup> grabbing_popup;
  MortalPtr<DataSource> selection;  // current clipboard owner (cleared in its destructor)
  int keymap_fd = -1;
  U32 keymap_size = 0;
  U32 mod_shift = 0, mod_ctrl = 0, mod_alt = 0, mod_super = 0;

  // dmabuf version-4 feedback inputs, built lazily on the first feedback request.
  FD dmabuf_format_table_fd;
  U32 dmabuf_format_table_size = 0;
  dev_t dmabuf_main_device = 0;
  bool dmabuf_has_device = false;
  bool dmabuf_inited = false;

  Server(mux::Epoll& epoll) : epoll(epoll) {}
  ~Server();

  void NotifyRead(Status&) override;
  StrView Name() const override;

  void FlushAll();

  // Compositor public API (formerly wayland_compositor.hpp). Callers reach it through the
  // `wayland::server` global; all of them run on the UI/render thread except where noted.
  bool Running();
  void UIFrame();
  void NotifyWindowDestroyed(void* toplevel_handle);
  void SendPointerEnter(library::WaylandSurface&, float lx, float ly);
  void SendPointerMotion(library::WaylandSurface&, float lx, float ly);
  void SendPointerButton(library::WaylandSurface&, uint32_t button, bool pressed);
  void SendPointerAxis(library::WaylandSurface&, float notches_up);
  void SendPointerLeave(library::WaylandSurface&);
  void SendKeyboardEnter(library::WaylandWindow&);
  void SendKeyboardLeave(library::WaylandWindow&);
  void SendKey(library::WaylandWindow&, uint32_t evdev_keycode, bool pressed, bool ctrl, bool alt,
               bool shift, bool super);
  void SendDecorationPreference(library::WaylandWindow&);
};

struct Client : mux::Epoll::Listener {
  Server& server;
  std::string in;
  std::string out;
  std::deque<FD> recv_fds;
  Vec<FD> out_fds;
  Vec<Common*> client_ids;
  Vec<Common*> server_ids;
  IdPool<0xff000000> server_id_pool;
  U32 next_id = 2;
  bool errored = false;
  static Colony<Client> colony;

  Client(Server& s) : server(s) {}
  ~Client() {
    for (Common* o : client_ids)
      if (o) o->GenericColonyDestroy();
    for (Common* o : server_ids)  // e.g. data_offers we allocated on the client's behalf
      if (o) o->GenericColonyDestroy();
  }

  void NotifyRead(Status&) override;
  StrView Name() const override;

  void SetId(U32 id, Common* o) {
    bool server = id >= 0xff000000;
    auto& table = server ? server_ids : client_ids;
    U32 index = server ? id - 0xff000000 : id - 1;
    if (index >= table.size()) table.resize(index + 1, nullptr);
    table[index] = o;
  }
  Common* GetId(U32 id) {
    bool server = id >= 0xff000000;
    auto& table = server ? server_ids : client_ids;
    U32 index = server ? id - 0xff000000 : id - 1;
    return index < table.size() ? table[index] : nullptr;
  }
  bool CheckId(U32 id) {
    if (id == 0 || id > next_id) return false;
    if (id == next_id) {
      ++next_id;
      return true;
    }
    return GetId(id) == nullptr;
  }
  void ProtocolError(U32 object_id, U32 code, StrView message);
  void Disconnect();
};

// A section of an `image`, possibly stretched to `dst_size` (image pixels).
struct CutoutGeometry {
  SkRect src_crop = SkRect::MakeEmpty();
  SkISize dst_size = {};
};
struct SurfaceCutout : CutoutGeometry {
  sk_sp<SkImage> image;
};

struct InputRegion {
  bool infinite = true;  // by default the whole surface is reactive
  SkPath path;
};

// Base class for (injecting data into specific) auto-generated Wayland types.
template <class>
struct Base : Common {
  using Common::Common;
};

template <>
struct Base<Surface> : Common {
  using Common::Common;
  MortalPtr<Buffer> pending_buffer;  // wl_buffer attached since the last commit
  bool buffer_attached = false;      // an attach happened (a null buffer means unmap)
  Vec<U32> frame_callbacks;          // wl_callback ids from wl_surface.frame
  MortalPtr<XdgSurface> xdg;
  Subsurface* as_subsurface = nullptr;
  MortalPtr<Viewport> viewport;
  MortalPtr<Buffer> held_dmabuf;  // dmabuf wl_buffer held for zero-copy display
  SurfaceCutout content;
  WeakPtr<ReferenceCounted> object;  // a library::WaylandSurface mirroring this
  InputRegion input_region;
  Optional<InputRegion> pending_input_region;
  int buffer_scale = 1;
  int32_t buffer_transform = 0;
  Vec<sk_sp<SkData>> frame_pool;  // recycled shm-copy allocations
  Vec<Surface*> stack;            // back-to-front (this + subsurfaces)
  Vec<Surface*> pending_stack;    // accumulates until commit
  ~Base();
};

template <>
struct Base<Region> : Common {
  using Common::Common;
  SkPath path;
};

template <>
struct Base<XdgWmBase> : Common {
  using Common::Common;
  U32 version = 1;
};

template <>
struct Base<XdgSurface> : Common {
  using Common::Common;
  MortalPtr<Surface> surface;
  MortalPtr<XdgToplevel> toplevel;
  MortalPtr<XdgPopup> popup;
  Vec<XdgPopup*> child_popups;  // oldest first
  SkIRect geo = SkIRect::MakeEmpty();
  U32 last_configure_serial = 0;
  U32 version = 1;  // inherited from the xdg_wm_base, propagated on to the toplevel
  bool initial_configure_sent = false;
};

template <>
struct Base<XdgToplevel> : Common {
  using Common::Common;
  MortalPtr<XdgSurface> xdg;
  Str title;
  Str app_id;
  bool mapped = false;
  U32 version = 1;  // inherited from the xdg_wm_base; gates wm_capabilities
  I64 pid = 0;
  WeakPtr<ReferenceCounted> window;  // a library::WaylandWindow
  MortalPtr<ZxdgToplevelDecorationV1> decoration;
  std::unique_ptr<mux::Timer> sigterm_timer;
  ~Base();
};

template <>
struct Base<ZxdgToplevelDecorationV1> : Common {
  using Common::Common;
  MortalPtr<XdgToplevel> toplevel;
  U32 client_mode = 0;
};

template <>
struct Base<Subsurface> : Common {
  using Common::Common;
  Surface* surface = nullptr;  // the child surface (holds the subsurface role)
  MortalPtr<Surface> parent;
  SkIPoint pos = {}, pending_pos = {};
  bool position_dirty = false;
  bool sync = true;
  Optional<SurfaceCutout> cache;
  ~Base();
};

template <>
struct Base<XdgPopup> : Common {
  using Common::Common;
  MortalPtr<XdgSurface> xdg;
  MortalPtr<XdgSurface> parent;
  SkIRect geo = SkIRect::MakeEmpty();
  SkIPoint flipped = {};
  bool flip_x = false, flip_y = false, slide_x = false, slide_y = false;
  ~Base();
};

template <>
struct Base<XdgPositioner> : Common {
  using Common::Common;
  SkISize size = {};
  SkIRect anchor_rect = SkIRect::MakeEmpty();
  U32 anchor = 0;
  U32 gravity = 0;
  SkIPoint offset = {};
  U32 constraint_adjustment = 0;
};

template <>
struct Base<Viewport> : Common {
  using Common::Common;
  MortalPtr<Surface> surface;
  float src_x = -1, src_y = -1, src_w = -1, src_h = -1;
  SkISize dst_size = {-1, -1};
};

template <>
struct Base<ShmPool> : Common {
  using Common::Common;
  FD fd;
  size_t size = 0;
};

struct DmabufPlane {
  FD fd;
  U32 offset = 0;
  U32 stride = 0;
};

template <>
struct Base<Buffer> : Common {
  using Common::Common;
  void* data = nullptr;  // shm: read-only mmap of the pool
  size_t map_size = 0;
  int32_t offset = 0, width = 0, height = 0, stride = 0;
  U32 format = 0;  // shm: wl_shm format
  DmabufPlane dmabuf_planes[4];
  int dmabuf_plane_count = 0;
  U32 drm_format = 0;  // dmabuf: DRM fourcc
  U64 dmabuf_modifier = 0;
  bool y_invert = false;
  ~Base();
};

template <>
struct Base<LinuxBufferParamsV1> : Common {
  using Common::Common;
  DmabufPlane planes[4];
  int plane_count = 0;
  U64 modifier = 0;
  bool used = false;
};

template <>
struct Base<Seat> : Common {
  using Common::Common;
  U32 version = 1;
};
template <>
struct Base<Pointer> : Common {
  using Common::Common;
  U32 version = 1;
};
template <>
struct Base<Keyboard> : Common {
  using Common::Common;
  U32 version = 1;
};

template <>
struct Base<DataSource> : Common {
  using Common::Common;
  Vec<Str> mimes;
};

template <>
struct Base<DataOffer> : Common {
  using Common::Common;
  MortalPtr<DataSource> source;
};

void AdvertiseDmabufOnBind(LinuxDmabufV1&, U32 version);

}  // namespace automat::wayland
