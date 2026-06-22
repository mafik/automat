// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "wayland_compositor.hpp"

#if defined(__linux__)

#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPath.h>
#include <include/core/SkSize.h>
#include <include/pathops/SkPathOps.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstring>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

#include "automat.hpp"
#include "colony.hpp"
#include "dmabuf.hpp"
#include "format.hpp"
#include "library_command.hpp"
#include "library_wayland_window.hpp"
#include "log.hpp"
#include "mux_epoll.hpp"
#include "mux_timer.hpp"
#include "thread_name.hpp"
#include "time.hpp"
#include "vk.hpp"
#include "xcb.hpp"

// The generated headers #define F std::function; they must come after all
// Automat headers.
#include "../build/generated/cursor-shape-v1.hpp"
#include "../build/generated/linux-dmabuf-v1.hpp"
#include "../build/generated/viewporter.hpp"
#include "../build/generated/wayland.hpp"
#include "../build/generated/xdg-decoration-unstable-v1.hpp"
#include "../build/generated/xdg-shell.hpp"

// The generated cursor-shape-v1 bindings reference this interface in the argument
// table of get_tablet_tool_v2. Automat advertises no tablet, so a client can
// never create a zwp_tablet_tool_v2 and that request is unreachable; a minimal
// stub satisfies the linker without generating the whole tablet protocol. (If
// tablet support is ever added, its generated definition replaces this.)
extern const wl_interface zwp_tablet_tool_v2_interface = {
    "zwp_tablet_tool_v2", 1, 0, nullptr, 0, nullptr};

namespace automat::wayland {

using library::WaylandSurface;
using library::WaylandWindow;
using Impl = Server::Impl;

namespace {

// Protocol-object model. One struct per protocol object the compositor has
// to remember between requests; each owns its generated resource wrapper and
// is destroyed from the wrapper's onDestroy (so client disconnects, which
// destroy every resource, tear all of this down automatically). The Wayland
// data model behind it is well explained at https://wayland-book.com:
// surfaces are double-buffered (requests accumulate "pending" state that
// becomes "current" at commit), and a bare wl_surface only becomes a window
// when an xdg-shell role chain is built on top of it.

struct Toplevel;
struct Viewport;
struct Subsurface;
struct Popup;
struct XdgSurf;

// Describes a section of an image (not included) possibly stretched to some `dst_size`.
struct CutoutGeometry {
  SkRect src_crop = SkRect::MakeEmpty();  // in local `image` pixels
  SkISize dst_size = {};                  // in local `image` pixels
};

// Describes a section of an image (image included) possibly stretched to some `dst_size`.
struct SurfaceCutout : CutoutGeometry {
  sk_sp<SkImage> image;
};

// A region rectangle, added or subtracted. Ops stay in request order: the last
// one covering a point decides membership.
struct RegionOp {
  SkIRect rect;
  bool add;
};
struct InputRegion {
  bool infinite = true;
  Vec<RegionOp> ops;
};

// A wl_region: snapshotted into a surface's InputRegion at adopt time, so the
// client may free it immediately.
struct Region {
  CWlRegion res;
  Vec<RegionOp> ops;
  Region(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Region(const Region&) = delete;
};

// A wl_surface: the client's pixel container. We track only what commit
// needs - the buffer attached since the last commit, the frame callbacks the
// client registered ("tell me when it's worth drawing the next frame"), and
// the toplevel role, if any, that turns this surface into a window. Surfaces
// without a toplevel (cursors, icons, subsurfaces) get their buffers
// released and are otherwise ignored.
struct Surface {
  CWlSurface res;  // address-pinned: the wl_resource and its destroy listener point back at it
  wl_resource* pending_buffer = nullptr;
  bool buffer_attached = false;  // attach happened since the last commit (may be null = unmap)
  Colony<CWlCallback> frame_cbs;
  Toplevel* toplevel = nullptr;
  XdgSurf* xdg = nullptr;
  wl_resource* held_dmabuf = nullptr;  // dmabuf buffer held for zero-copy display
  Viewport* viewport = nullptr;        // wp_viewport crop/scale state, if any
  SurfaceCutout content;
  // The scene-graph node (an Automat object) mirroring this surface; the parent
  // node's child list owns it, so it dies when the surface leaves the tree.
  WeakPtr<WaylandSurface> object;
  InputRegion input_region;                    // current; pointer hit-testing honors it
  Optional<InputRegion> pending_input_region;  // set_input_region, applied at commit
  // The image is not rotated, so only the normal transform renders correctly.
  int buffer_scale = 1;
  int32_t buffer_transform = 0;
  // Recycled frame allocations for this surface's shm copies; an entry is
  // reclaimed once nothing else (the window's SkImage, the renderer) references it.
  std::vector<sk_sp<SkData>> frame_pool;

  Subsurface* as_subsurface = nullptr;
  std::vector<Surface*> stack;          // back-to-front order (this + subsurfaces)
  std::vector<Surface*> pending_stack;  // accumulates updates until 'commit'

  Surface(wl_client* client, int version, uint32_t id) : res(client, version, id) {
    stack.push_back(this);  // own content; subsurfaces are inserted around it
    pending_stack.push_back(this);
  }
  Surface(const Surface&) = delete;
};

// A wl_subsurface: positions one child surface relative to a parent and stacks
// it in the parent's surface tree. The position is applied at the PARENT
// surface's commit; in sync mode (the default) the child's own commits are
// cached and applied at the parent's commit too, while in desync mode they apply
// immediately. `surface`/`parent` go null when either wl_surface is destroyed.
struct Subsurface {
  CWlSubsurface res;
  Surface* surface = nullptr;  // the child surface (holds the subsurface role)
  Surface* parent = nullptr;
  SkIPoint pos = {};          // applied position, in the parent's surface px
  SkIPoint pending_pos = {};  // set_position, applied at the parent's commit
  bool position_dirty = false;
  bool sync = true;  // set_desync clears this
  // Sync-mode cache: the child's committed frame, held until the parent commits.
  Optional<SurfaceCutout> cache;

  Subsurface(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Subsurface(const Subsurface&) = delete;
};

// A wp_viewport: the crop and scale state a client attaches to one wl_surface.
// The state lives on this object, not on the Surface, so destroying the viewport
// removes the crop and scale at the surface's next commit, which is exactly what
// the protocol requires. `surface` becomes null once the wl_surface is gone; any
// later request on the orphaned viewport then raises no_surface.
struct Viewport {
  CWpViewport res;
  Surface* surface = nullptr;
  // Pending source rectangle (buffer pixels; Automat applies no buffer transform
  // or scale) and destination size, read at the surface's next commit. A
  // negative width marks the rectangle or the destination as unset, which falls
  // back to the buffer's own geometry.
  double src_x = -1, src_y = -1, src_w = -1, src_h = -1;
  SkISize dst_size = {-1, -1};

  Viewport(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Viewport(const Viewport&) = delete;
};

// An xdg_surface: the middle of the role chain wl_surface -> xdg_surface ->
// xdg_toplevel. Its job is the configure handshake: after the client commits
// the unmapped surface, the compositor sends one configure bundle (toplevel
// configure first, then xdg_surface.configure with a serial) and nothing may
// send xdg_surface.configure out of band before that - GLFW clients
// disconnect if it happens. The client acks and only then attaches pixels.
struct XdgSurf {
  CXdgSurface res;
  CXdgWmBase* wm_base = nullptr;  // for posting protocol errors
  Surface* surface = nullptr;
  Toplevel* toplevel = nullptr;
  Popup* popup = nullptr;
  Vec<Popup*> child_popups;            // oldest first
  SkIRect geo = SkIRect::MakeEmpty();  // set_window_geometry, surface px
  uint32_t last_configure_serial = 0;  // ack_configure must not exceed it
  bool initial_configure_sent = false;

  XdgSurf(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  XdgSurf(const XdgSurf&) = delete;
};

// An xdg_toplevel: an actual window. "Mapped" means the first buffer was
// committed - that is the moment a WaylandWindow object is created (or a
// respawned recipe's existing object is adopted) and handed to the UI thread
// for insertion onto the board. The window object is OWNED by its board
// Location; the compositor keeps only a WeakPtr because a board drag
// temporarily extracts the Location, and treating that as deletion would
// kill the client mid-drag. ~WaylandWindow is the real deletion signal.
struct Toplevel {
  CXdgToplevel res;
  XdgSurf* xdg = nullptr;
  Str title;
  Str app_id;
  bool mapped = false;
  pid_t pid = 0;                      // client process, for close escalation
  mux::Timer* close_timer = nullptr;  // armed when a close was requested
  WeakPtr<WaylandWindow> window_weak;

  Toplevel(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Toplevel(const Toplevel&) = delete;

  Ptr<WaylandWindow> LockWindow() const { return window_weak.Lock(); }
};

struct Positioner {
  CXdgPositioner res;
  SkISize size = {};                           // popup size (set_size)
  SkIRect anchor_rect = SkIRect::MakeEmpty();  // set_anchor_rect
  uint32_t anchor = 0;                         // xdgPositionerAnchor (set_anchor)
  uint32_t gravity = 0;                        // xdgPositionerGravity (set_gravity)
  SkIPoint offset = {};                        // set_offset
  uint32_t constraint_adjustment = 0;          // flip/slide to keep the popup on-screen

  Positioner(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Positioner(const Positioner&) = delete;
};

struct Popup {
  CXdgPopup res;
  XdgSurf* xdg = nullptr;
  XdgSurf* parent = nullptr;
  SkIRect geo = SkIRect::MakeEmpty();  // anchor geometry in the parent's window-geometry space
  SkIPoint flipped = {};               // `geo` mirrored about the anchor
  bool flip_x = false, flip_y = false, slide_x = false, slide_y = false;

  Popup(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Popup(const Popup&) = delete;
};

struct DataSource {
  CWlDataSource res;
  Vec<Str> mimes;
  DataSource(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  DataSource(const DataSource&) = delete;
};

struct DataOffer {
  CWlDataOffer res;
  DataSource* source = nullptr;
  DataOffer(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  DataOffer(const DataOffer&) = delete;
};

// One plane of a dmabuf-backed buffer. The fd stays open for the buffer's whole
// lifetime so the compositor can re-import on every commit.
struct DmabufPlane {
  int fd = -1;
  uint32_t offset = 0;
  uint32_t stride = 0;
};

// A zwp_linux_buffer_params_v1: planes accumulate via `add`, then `create` or
// `create_immed` turns them into a wl_buffer. Single-use; the client destroys it
// once the buffer (or `failed`) comes back.
struct DmabufParams {
  CZwpLinuxBufferParamsV1 res;
  DmabufPlane planes[4];
  int plane_count = 0;
  uint64_t modifier = DRM_FORMAT_MOD_INVALID;
  bool used = false;

  DmabufParams(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  DmabufParams(const DmabufParams&) = delete;
  ~DmabufParams() {
    for (auto& p : planes)
      if (p.fd >= 0) close(p.fd);
  }
};

// A dmabuf-backed wl_buffer. The compositor owns the plane fds until the client
// destroys the buffer; every commit re-imports them into a GPU texture (see the
// dmabuf branch in Commit).
struct DmabufBuffer {
  CWlBuffer res;
  SkISize size = {};
  uint32_t drm_format = 0;
  uint64_t modifier = DRM_FORMAT_MOD_INVALID;
  bool y_invert = false;
  DmabufPlane planes[4];
  int plane_count = 0;

  DmabufBuffer(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  DmabufBuffer(const DmabufBuffer&) = delete;
  ~DmabufBuffer() {
    for (auto& p : planes)
      if (p.fd >= 0) close(p.fd);
  }
};

// Drives libwayland from our epoll loop: when the wl_display's event loop fd
// signals, dispatch its ready events once (non-blocking) and flush replies.
struct WaylandListener : mux::Epoll::Listener {
  Impl* impl = nullptr;
  StrView Name() const override { return "Wayland"sv; }
  void NotifyRead(Status&) override;
};

}  // namespace

struct Server::Impl {
  wl_display* display = nullptr;
  wl_event_loop* loop = nullptr;
  Str socket_name;
  std::thread thread;
  std::atomic<bool> ready = false;
  WaylandListener wayland_listener;
  mux::Epoll epoll;
  uint32_t serial = 1;

  // Protocol objects; wayland thread only.
  Colony<CWlCompositor> compositor_resources;
  Colony<CWlSubcompositor> subcompositors;
  Colony<Subsurface> subsurfaces;
  Colony<Region> regions;
  Colony<CWlDataDeviceManager> data_device_managers;
  Colony<CWlDataDevice> data_devices;
  Colony<DataSource> data_sources;
  Colony<DataOffer> data_offers;
  DataSource* selection = nullptr;  // the current clipboard selection source, or null
  Colony<CWlOutput> outputs;
  Colony<CWlSeat> seats;
  Colony<CWlPointer> pointers;
  Colony<CWlKeyboard> keyboards;
  // The surfaces in a window tree that currently hold pointer / keyboard focus,
  // so events route to the subsurface under the cursor (cleared on destroy).
  Surface* pointer_surface = nullptr;
  Surface* keyboard_surface = nullptr;
  Popup* grabbing_popup = nullptr;  // topmost popup holding a grab, or null
  Colony<CXdgWmBase> wm_bases;
  Colony<Positioner> positioners;
  Colony<Popup> popups;
  Colony<CZxdgDecorationManagerV1> decoration_managers;
  Colony<CZxdgToplevelDecorationV1> decorations;
  Colony<CWpViewporter> viewporters;
  Colony<Viewport> viewports;
  Colony<CWpCursorShapeManagerV1> cursor_shape_managers;
  Colony<CWpCursorShapeDeviceV1> cursor_shape_devices;
  Colony<Surface> surfaces;
  Colony<XdgSurf> xdg_surfaces;
  Colony<Toplevel> toplevels;
  std::unordered_map<wl_resource*, Surface*> surface_by_res;

  // GPU buffer passing (zwp_linux_dmabuf_v1). Buffers outlive their params and
  // are looked up by resource at commit time.
  Colony<CZwpLinuxDmabufV1> dmabuf_globals;
  Colony<DmabufParams> dmabuf_params;
  Colony<DmabufBuffer> dmabuf_buffers;
  std::unordered_map<wl_resource*, DmabufBuffer*> dmabuf_by_res;

  // Version 4 feedback inputs, built once in InitDmabuf: the render node clients
  // must allocate on (without it Mesa can't pick a device and falls back to shm)
  // and a sealed table the feedback events index into.
  Colony<CZwpLinuxDmabufFeedbackV1> dmabuf_feedbacks;
  int dmabuf_format_table_fd = -1;
  uint32_t dmabuf_format_table_size = 0;
  dev_t dmabuf_main_device = 0;
  bool dmabuf_has_device = false;

  Colony<mux::Timer> close_timers;    // SIGTERMs for naughty children, plus the frame timer
  mux::Timer* frame_timer = nullptr;  // paces wl_surface.frame callbacks to ~60 Hz
  bool frame_pending = false;         // a frame-callback completion is already scheduled

  // The xkb keymap advertised to clients (matches the compositor host's
  // default layout); built once at startup.
  int keymap_fd = -1;
  uint32_t keymap_size = 0;
  uint32_t mod_shift = 0, mod_ctrl = 0, mod_alt = 0, mod_super = 0;

  // Handoff to the UI thread.
  std::mutex ui_mutex;
  std::vector<Ptr<Object>> ui_appeared;
  std::vector<Ptr<Object>> ui_disappeared;

  // Respawned clients waiting to be adopted into their board objects.
  std::mutex adoption_mutex;
  std::vector<std::pair<pid_t, WeakPtr<WaylandWindow>>> adoptions;
};

namespace {

template <typename T>
void EraseOwned(Colony<T>& v, T* p) {
  v.erase(v.get_iterator(p));
}

template <typename T>
void DestroyOnRequest(T& w) {
  w.setDestroy([](T* r) { wl_resource_destroy(r->resource()); });
}
template <typename T>
void ReleaseOnRequest(T& w) {
  w.setRelease([](T* r) { wl_resource_destroy(r->resource()); });
}

// The Surface backing a wl_surface resource, or null if it is not one of ours
// (or was already destroyed).
Surface* SurfaceOrNull(Impl& s, wl_resource* res) {
  auto it = s.surface_by_res.find(res);
  return it == s.surface_by_res.end() ? nullptr : it->second;
}

// The Region backing a wl_region resource, or null. Regions are few, so a linear
// scan beats carrying another lookup table.
Region* RegionOrNull(Impl& s, wl_resource* res) {
  for (auto& r : s.regions)
    if (r.res.resource() == res) return &r;
  return nullptr;
}

XdgSurf* XdgSurfOrNull(Impl& s, wl_resource* res) {
  if (!res) return nullptr;
  for (auto& x : s.xdg_surfaces)
    if (x.res.resource() == res) return &x;
  return nullptr;
}

Positioner* PositionerOrNull(Impl& s, wl_resource* res) {
  if (!res) return nullptr;
  for (auto& p : s.positioners)
    if (p.res.resource() == res) return &p;
  return nullptr;
}

// Places the popup by the positioner's anchor, gravity and offset, and records
// the flipped alternative and the client's flip/slide permissions.
void ResolvePopup(const Positioner& p, Popup& popup) {
  auto is_left = [](uint32_t e) {
    return e == XDG_POSITIONER_ANCHOR_LEFT || e == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           e == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
  };
  auto is_right = [](uint32_t e) {
    return e == XDG_POSITIONER_ANCHOR_RIGHT || e == XDG_POSITIONER_ANCHOR_TOP_RIGHT ||
           e == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
  };
  auto is_top = [](uint32_t e) {
    return e == XDG_POSITIONER_ANCHOR_TOP || e == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           e == XDG_POSITIONER_ANCHOR_TOP_RIGHT;
  };
  auto is_bottom = [](uint32_t e) {
    return e == XDG_POSITIONER_ANCHOR_BOTTOM || e == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT ||
           e == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
  };
  // The anchor and gravity enums share values, so one mirror serves both axes.
  auto mirror_x = [](uint32_t e) -> uint32_t {
    switch (e) {
      case XDG_POSITIONER_ANCHOR_LEFT:
        return XDG_POSITIONER_ANCHOR_RIGHT;
      case XDG_POSITIONER_ANCHOR_RIGHT:
        return XDG_POSITIONER_ANCHOR_LEFT;
      case XDG_POSITIONER_ANCHOR_TOP_LEFT:
        return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
      case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
        return XDG_POSITIONER_ANCHOR_TOP_LEFT;
      case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
        return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
      case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
        return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
      default:
        return e;
    }
  };
  auto mirror_y = [](uint32_t e) -> uint32_t {
    switch (e) {
      case XDG_POSITIONER_ANCHOR_TOP:
        return XDG_POSITIONER_ANCHOR_BOTTOM;
      case XDG_POSITIONER_ANCHOR_BOTTOM:
        return XDG_POSITIONER_ANCHOR_TOP;
      case XDG_POSITIONER_ANCHOR_TOP_LEFT:
        return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
      case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
        return XDG_POSITIONER_ANCHOR_TOP_LEFT;
      case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
        return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
      case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
        return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
      default:
        return e;
    }
  };
  // Gravity is the direction the popup extends from the anchor point.
  auto place_x = [&](uint32_t anchor, uint32_t gravity) {
    const SkIRect& ar = p.anchor_rect;
    int w = p.size.width();
    int a = is_left(anchor) ? ar.x() : is_right(anchor) ? ar.right() : ar.x() + ar.width() / 2;
    int x = is_left(gravity) ? a - w : is_right(gravity) ? a : a - w / 2;
    return x + p.offset.x();
  };
  auto place_y = [&](uint32_t anchor, uint32_t gravity) {
    const SkIRect& ar = p.anchor_rect;
    int h = p.size.height();
    int a = is_top(anchor) ? ar.y() : is_bottom(anchor) ? ar.bottom() : ar.y() + ar.height() / 2;
    int y = is_top(gravity) ? a - h : is_bottom(gravity) ? a : a - h / 2;
    return y + p.offset.y();
  };

  uint32_t ca = p.constraint_adjustment;
  popup.geo = SkIRect::MakeXYWH(place_x(p.anchor, p.gravity), place_y(p.anchor, p.gravity),
                                p.size.width(), p.size.height());
  popup.flipped = {place_x(mirror_x(p.anchor), mirror_x(p.gravity)),
                   place_y(mirror_y(p.anchor), mirror_y(p.gravity))};
  popup.flip_x = ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X;
  popup.flip_y = ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
  popup.slide_x = ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X;
  popup.slide_y = ca & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
}

void WaylandListener::NotifyRead(Status&) {
  wl_event_loop_dispatch(impl->loop, 0);
  wl_display_flush_clients(impl->display);
}

uint32_t NowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             time::SteadyNow().time_since_epoch())
      .count();
}

// Completes every pending wl_surface.frame callback; the frame timer fires this
// at ~60 Hz to pace animating clients. Flushing matters because this runs off
// the timer fd, not the client-fd dispatch path that flushes on its own.
void CompletePendingFrames(Impl& s) {
  uint32_t now = NowMs();
  for (auto& surf : s.surfaces) {
    for (auto& cb : surf.frame_cbs) cb.sendDone(now);
    surf.frame_cbs.clear();
  }
  s.frame_pending = false;
  wl_display_flush_clients(s.display);
}

// ----------------------------------------------------------------- surfaces --

void UnmapToplevel(Impl& s, Toplevel& t) {
  if (!t.mapped) return;
  t.mapped = false;
  if (auto win = t.LockWindow()) {
    win->toplevel_handle.store(nullptr);
    {
      auto lock = std::lock_guard(win->mutex);
      win->client_gone = true;
    }
    win->WakeToys();
    {
      auto lock = std::lock_guard(s.ui_mutex);
      s.ui_disappeared.push_back(std::move(win));
    }
    vm.WakeToys();  // wakes RootWidget::Tick (via PackFrame) so UIFrame drains
  }
  t.window_weak = {};
}

// Releases the dmabuf buffer a surface holds for zero-copy display, if any.
void ReleaseHeldDmabuf(Impl& s, Surface& surf) {
  if (surf.held_dmabuf) {
    wl_buffer_send_release(surf.held_dmabuf);
    surf.held_dmabuf = nullptr;
  }
}

// Returns the window object for this toplevel's frame, creating it (or adopting
// a respawned recipe's existing object) on the first buffer. Sets `adopted` when
// an existing board object was reattached.
Ptr<Object> AcquireWindow(Impl& s, Toplevel& t, bool& adopted) {
  adopted = false;
  auto win = t.LockWindow();
  if (win || t.mapped) return win;
  {  // A respawned recipe adopts its existing board object.
    auto lock = std::lock_guard(s.adoption_mutex);
    for (auto it = s.adoptions.begin(); it != s.adoptions.end(); ++it) {
      if (it->first == t.pid) {
        win = it->second.Lock();
        s.adoptions.erase(it);
        break;
      }
    }
  }
  if (win) {
    adopted = true;
    win->toplevel_handle.store(&t);
    {
      auto lock = std::lock_guard(win->mutex);
      win->client_gone = false;
      win->client_pid = t.pid;
      if (!t.title.empty()) win->title = t.title;
      win->app_id = t.app_id;
    }
    t.window_weak = win->AcquireWeakPtr();
  } else {
    auto created = MAKE_PTR(WaylandWindow);
    created->toplevel_handle.store(&t);
    {
      auto lock = std::lock_guard(created->mutex);
      created->title = t.title;
      created->app_id = t.app_id;
      created->client_pid = t.pid;
    }
    t.window_weak = created->AcquireWeakPtr();
    win = std::move(created);
  }
  return win;
}

// Wakes the toy/UI for a new frame and, on the first frame, seats the window on
// the board (adopted windows are already there).
void PublishFrame(Impl& s, Toplevel& t, Ptr<Object> win, bool adopted) {
  win->WakeToys();
  vm.WakeToys();
  if (!t.mapped) {
    t.mapped = true;
    if (!adopted) {
      auto lock = std::lock_guard(s.ui_mutex);
      s.ui_appeared.push_back(std::move(win));
    }
  }
}

// Fills `out` from the surface's viewport and the buffer size. Returns false,
// having posted the protocol error, on the two violations the protocol defers to
// commit time: a non-integer crop with no destination size, and a source
// rectangle reaching outside the buffer. Source values are exact dyadic
// rationals (wl_fixed/256) and buffer sizes are integers, so the comparisons
// below carry no floating-point slack.
bool ResolveGeometry(Surface& surf, SkISize buf, CutoutGeometry& out) {
  Viewport* vp = surf.viewport;
  bool src_set = vp && vp->src_w >= 0;
  bool dst_set = vp && vp->dst_size.width() >= 0;
  double sx = src_set ? vp->src_x : 0;
  double sy = src_set ? vp->src_y : 0;
  double sw = src_set ? vp->src_w : buf.width();
  double sh = src_set ? vp->src_h : buf.height();
  if (src_set) {
    if (!dst_set && (sw != std::floor(sw) || sh != std::floor(sh))) {
      vp->res.error(WP_VIEWPORT_ERROR_BAD_SIZE, "non-integer crop without a destination size");
      return false;
    }
    if (sx + sw > buf.width() || sy + sh > buf.height() || sx < 0 || sy < 0) {
      vp->res.error(WP_VIEWPORT_ERROR_OUT_OF_BUFFER, "source rectangle reaches outside the buffer");
      return false;
    }
  }
  out.src_crop.setXYWH(sx, sy, sw, sh);
  if (dst_set) {
    out.dst_size = vp->dst_size;
  } else {
    // No viewport destination: the surface size is the (cropped) buffer divided
    // by the buffer scale, with width and height swapped for a 90/270 transform.
    int dw = (int)sw / surf.buffer_scale, dh = (int)sh / surf.buffer_scale;
    bool swap = surf.buffer_transform == WL_OUTPUT_TRANSFORM_90 ||
                surf.buffer_transform == WL_OUTPUT_TRANSFORM_270 ||
                surf.buffer_transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                surf.buffer_transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
    out.dst_size = {swap ? dh : dw, swap ? dw : dh};
  }
  static bool first = true;
  if (first && (src_set || dst_set)) {
    first = false;
    LOG << f("Wayland: first viewport applied, buffer {}x{} -> surface {}x{} source {}x{}+{}+{}",
             buf.width(), buf.height(), out.dst_size.width(), out.dst_size.height(), (int)sw,
             (int)sh, (int)sx, (int)sy);
  }
  return true;
}

// Imports a just-committed buffer into `out_*` (the image plus its wp_viewport
// source crop and displayed surface size). Handles shm (row-copied into
// surf.frame_pool) and dmabuf (GPU import); releases the shm buffer immediately
// or holds the dmabuf until the next frame replaces it. Returns false (with the
// buffer already released or held) when the buffer is unusable or a protocol
// error was posted.
bool ImportBuffer(Impl& s, Surface& surf, wl_resource* buf, SurfaceCutout& out) {
  if (wl_shm_buffer* shm = wl_shm_buffer_get(buf)) {
    int w = wl_shm_buffer_get_width(shm);
    int h = wl_shm_buffer_get_height(shm);
    int stride = wl_shm_buffer_get_stride(shm);
    uint32_t format = wl_shm_buffer_get_format(shm);
    CutoutGeometry geom;
    bool ok = w > 0 && h > 0 && ResolveGeometry(surf, {w, h}, geom);
    if (ok) {
      // The one unavoidable copy (we release the client's buffer right after
      // commit): shm rows land in a pooled allocation, and the SkImage wraps
      // that allocation without copying again.
      size_t need = (size_t)w * 4 * h;
      sk_sp<SkData> data;
      for (auto it = surf.frame_pool.begin(); it != surf.frame_pool.end(); ++it) {
        if ((*it)->unique() && (*it)->size() == need) {
          data = std::move(*it);
          surf.frame_pool.erase(it);
          break;
        }
      }
      if (!data) data = SkData::MakeUninitialized(need);
      wl_shm_buffer_begin_access(shm);
      auto* src = (const uint8_t*)wl_shm_buffer_get_data(shm);
      auto* dst = (uint8_t*)data->writable_data();
      for (int row = 0; row < h; ++row) {
        memcpy(dst + (size_t)row * w * 4, src + (size_t)row * stride, (size_t)w * 4);
      }
      wl_shm_buffer_end_access(shm);
      auto info = SkImageInfo::Make(
          w, h, kBGRA_8888_SkColorType,
          format == WL_SHM_FORMAT_XRGB8888 ? kOpaque_SkAlphaType : kPremul_SkAlphaType);
      out.image = SkImages::RasterFromData(info, data, (size_t)w * 4);
      out.src_crop = geom.src_crop;
      out.dst_size = geom.dst_size;
      surf.frame_pool.push_back(std::move(data));
      if (surf.frame_pool.size() > 4) surf.frame_pool.erase(surf.frame_pool.begin());
    }
    // Copy-release: the client may reuse the buffer immediately.
    wl_buffer_send_release(buf);
    ReleaseHeldDmabuf(s, surf);
    return ok;
  }
  if (auto it = s.dmabuf_by_res.find(buf); it != s.dmabuf_by_res.end()) {
    DmabufBuffer* db = it->second;
    static bool first_dmabuf = true;
    if (first_dmabuf) {
      first_dmabuf = false;
      LOG << f("Wayland: first dmabuf frame {}x{} drm_format=0x{:08x} modifier=0x{:016x}",
               db->size.width(), db->size.height(), db->drm_format, db->modifier);
    }
    CutoutGeometry geom;
    bool ok = !db->size.isEmpty() && ResolveGeometry(surf, db->size, geom);
    if (ok) {
      // Import the planes to an SkImage here, on the compositor thread, so a
      // dmabuf becomes a layer exactly like an shm frame (vk::ImportDmabuf owns
      // the Graphite recorder its zero-copy path needs). The fds are dup'd
      // because the buffer outlives this commit and may be re-imported next frame.
      DmabufImage desc;
      desc.width = db->size.width();
      desc.height = db->size.height();
      desc.drm_format = db->drm_format;
      desc.modifier = db->modifier;
      desc.opaque = db->drm_format == DRM_FORMAT_XRGB8888;
      desc.y_invert = db->y_invert;
      desc.plane_count = db->plane_count;
      bool dup_ok = true;
      for (int i = 0; i < db->plane_count; ++i) {
        desc.fds[i] = fcntl(db->planes[i].fd, F_DUPFD_CLOEXEC, 0);
        desc.offsets[i] = db->planes[i].offset;
        desc.strides[i] = db->planes[i].stride;
        if (desc.fds[i] < 0) dup_ok = false;
      }
      // ImportDmabuf takes desc by value and closes its fds; on a dup failure we
      // skip it and desc closes them itself at scope exit.
      out.image = dup_ok ? vk::ImportDmabuf(std::move(desc)) : nullptr;
      out.src_crop = geom.src_crop;
      out.dst_size = geom.dst_size;
      ok = (bool)out.image;
    }
    // Zero-copy: hold this buffer until the next frame replaces it, then let the
    // client reuse the previous one.
    if (surf.held_dmabuf && surf.held_dmabuf != buf) {
      wl_buffer_send_release(surf.held_dmabuf);
    }
    surf.held_dmabuf = buf;
    return ok;
  }
  // Unknown buffer type; release it so clients don't stall.
  wl_buffer_send_release(buf);
  return false;
}

// Walks up to the Toplevel that owns `surf`'s window, following subsurface parent
// links and popup parent links, or null if the surface isn't part of a toplevel
// tree. Popups hang off the toplevel through their parent xdg_surface, so a popup
// commit recomposes the same window.
Toplevel* OwningToplevel(Surface& surf) {
  for (Surface* p = &surf; p;) {
    if (p->toplevel) return p->toplevel;
    if (p->xdg && p->xdg->popup && p->xdg->popup->parent) {
      p = p->xdg->popup->parent->surface;
      continue;
    }
    p = p->as_subsurface ? p->as_subsurface->parent : nullptr;
  }
  return nullptr;
}

Ptr<WaylandSurface> GetOrCreateObject(Surface& surf) {
  if (auto p = surf.object.Lock()) return p;
  auto p = MAKE_PTR(WaylandSurface);
  surf.object = p->AcquireWeakPtr();
  return p;
}

SkPath InputRegionPath(const InputRegion& ir, SkISize size) {
  if (ir.infinite) return SkPath::Rect(SkRect::MakeIWH(size.width(), size.height()));
  SkPath path;
  for (const RegionOp& op : ir.ops) {
    SkPath rect = SkPath::Rect(SkRect::Make(op.rect));
    SkPath out;
    if (Op(path, rect, op.add ? kUnion_SkPathOp : kDifference_SkPathOp, &out))
      path = std::move(out);
  }
  return path;
}

// Copies `surf`'s committed texture and child surfaces into its `object`,
// recursing into subsurfaces and popups to mirror the Wayland surface tree.
void UpdateSurfaceNode(WaylandSurface& object, Surface& surf) {
  Vec<WaylandSurface::Child> below, above;
  bool seen_self = false;
  for (Surface* entry : surf.stack) {
    if (entry == &surf) {
      seen_self = true;
      continue;
    }
    Ptr<WaylandSurface> child = GetOrCreateObject(*entry);
    UpdateSurfaceNode(*child, *entry);
    (seen_self ? above : below).push_back({std::move(child), entry->as_subsurface->pos});
  }
  if (surf.xdg) {
    for (Popup* pp : surf.xdg->child_popups) {
      if (!pp->xdg || !pp->xdg->surface) continue;
      Ptr<WaylandSurface> child = GetOrCreateObject(*pp->xdg->surface);
      UpdateSurfaceNode(*child, *pp->xdg->surface);
      SkIPoint base = surf.xdg->geo.topLeft();
      above.push_back({std::move(child), base + pp->geo.topLeft(), true, base + pp->flipped,
                       pp->flip_x, pp->flip_y, pp->slide_x, pp->slide_y});
    }
  }
  {
    auto lock = std::lock_guard(object.mutex);
    object.image = surf.content.image;
    object.src_crop = surf.content.src_crop;
    object.dst_size = surf.content.dst_size;
    object.input_region = InputRegionPath(surf.input_region, surf.content.dst_size);
    object.below = std::move(below);
    object.above = std::move(above);
    object.WakeToys();
  }
  // The toy hands this back to route input; resolved via surface_by_res.
  object.surface_handle.store(surf.res.resource());
}

// Rebuilds the window's surface-object tree from the toplevel surface tree plus
// any popups (the toplevel surface IS the window object), so Automat's widget
// compositing lays the tree out, each surface drawing its own texture.
void SyncWindowTree(WaylandWindow& win, Toplevel& t) {
  if (t.xdg && t.xdg->surface) {
    UpdateSurfaceNode(win, *t.xdg->surface);
  } else {
    auto lock = std::lock_guard(win.mutex);
    win.image = nullptr;
    win.dst_size = {};
    win.below.clear();
    win.above.clear();
    win.WakeToys();
  }
}

// At a parent surface's commit, applies its subsurfaces' pending positions and,
// in sync mode, their cached committed state - recursing into sync children.
// Returns whether anything visible changed.
bool ApplyChildren(Surface& parent) {
  bool changed = false;
  if (parent.stack != parent.pending_stack) {
    parent.stack = parent.pending_stack;
    changed = true;
  }
  for (Surface* entry : parent.stack) {
    if (entry == &parent) continue;
    Subsurface* sub = entry->as_subsurface;
    if (sub->position_dirty) {
      sub->pos = sub->pending_pos;
      sub->position_dirty = false;
      changed = true;
    }
    if (sub->sync && sub->cache) {
      entry->content = std::move(*sub->cache);
      sub->cache.reset();
      changed = true;
      ApplyChildren(*entry);
    }
  }
  return changed;
}

// Recomposes and publishes the window that owns `surf` after its applied state
// changed. The toplevel surface's first frame creates/adopts and maps the window
// object; subsurface updates refresh an already-mapped window.
void ApplyAndPublish(Impl& s, Surface& surf) {
  Toplevel* owner = OwningToplevel(surf);
  if (!owner) return;
  bool adopted = false;
  Ptr<Object> win_obj =
      (owner == surf.toplevel) ? AcquireWindow(s, *owner, adopted) : owner->LockWindow();
  if (!win_obj) return;
  SyncWindowTree(static_cast<WaylandWindow&>(*win_obj), *owner);
  PublishFrame(s, *owner, std::move(win_obj), adopted);
}

void SendXdgConfigure(Impl& s, XdgSurf& xp) {
  xp.last_configure_serial = s.serial++;
  xp.res.sendConfigure(xp.last_configure_serial);
}

void Commit(Impl& s, Surface& surf) {
  Toplevel* t = surf.toplevel;

  // Initial configure handshake, sent on the first commit before the client
  // attaches pixels. A toplevel gets its configure bundle (toplevel first, then
  // xdg_surface); a popup gets its resolved geometry, then xdg_surface. Nothing
  // else may send an xdg_surface.configure before this.
  if (XdgSurf* xp = surf.xdg; xp && !xp->initial_configure_sent) {
    if (xp->toplevel) {
      // wm_capabilities (v5+) must precede the first xdg_surface.configure; an
      // empty set means none of maximize/minimize/fullscreen/window-menu.
      if (xp->toplevel->res.version() >= 5) {
        wl_array caps;
        wl_array_init(&caps);
        xp->toplevel->res.sendWmCapabilities(&caps);
        wl_array_release(&caps);
      }
      wl_array states;
      wl_array_init(&states);
      xp->toplevel->res.sendConfigure(0, 0, &states);
      wl_array_release(&states);
      SendXdgConfigure(s, *xp);
      xp->initial_configure_sent = true;
    } else if (xp->popup) {
      Popup& pp = *xp->popup;
      pp.res.sendConfigure(pp.geo.x(), pp.geo.y(), pp.geo.width(), pp.geo.height());
      SendXdgConfigure(s, *xp);
      xp->initial_configure_sent = true;
    }
  }

  if (surf.pending_input_region) {
    surf.input_region = std::move(*surf.pending_input_region);
    surf.pending_input_region.reset();
  }

  bool dirty = false;
  if (surf.buffer_attached) {
    surf.buffer_attached = false;
    wl_resource* buf = surf.pending_buffer;
    surf.pending_buffer = nullptr;
    if (!buf) {
      // Null buffer = unmap: a toplevel removes its window, a subsurface drops
      // out of its window's layers.
      surf.content.image = nullptr;
      if (t) {
        UnmapToplevel(s, *t);
      } else {
        dirty = true;
      }
      ReleaseHeldDmabuf(s, surf);
    } else {
      SurfaceCutout content;
      if (ImportBuffer(s, surf, buf, content)) {
        if (surf.as_subsurface && surf.as_subsurface->sync) {
          // Sync subsurface: cache the committed frame until the parent commits.
          surf.as_subsurface->cache = std::move(content);
        } else {
          surf.content = std::move(content);
          dirty = true;
        }
      }
    }
  }

  // This surface's commit applies its subsurfaces' positions and sync caches.
  if (ApplyChildren(surf)) dirty = true;
  if (dirty) ApplyAndPublish(s, surf);

  // Pace frame callbacks at ~60 Hz rather than completing them at commit, or an
  // animating client (a video) re-renders in a tight loop. Armed on demand.
  if (!surf.frame_cbs.empty() && !s.frame_pending && s.frame_timer) {
    s.frame_pending = true;
    s.frame_timer->Arm(1.0 / 60);
  }
}

// place_above/place_below reorder a subsurface relative to a sibling, which may
// be another subsurface of the same parent or the parent surface itself. The
// reorder is double-buffered: it edits the parent's pending stack and only
// becomes visible at the parent's next commit, so nothing is published here.
void RestackSubsurface(Impl& s, Subsurface& sub, wl_resource* sibling_res, bool above) {
  Surface* parent = sub.parent;
  Surface* child = sub.surface;
  if (!parent || !child) return;
  auto& stack = parent->pending_stack;
  std::erase(stack, child);
  Surface* ref = SurfaceOrNull(s, sibling_res);  // a sibling's surface, or the parent itself
  auto it = std::find(stack.begin(), stack.end(), ref);
  if (it == stack.end())
    stack.push_back(child);  // unknown reference: leave on top
  else
    stack.insert(it + (above ? 1 : 0), child);
}

// Removes a subsurface from its parent's tree and orphans it. Idempotent: safe to
// call again from the wl_surface's own destroy.
void DetachSubsurface(Impl& s, Subsurface& sub) {
  Surface* parent = sub.parent;
  if (parent && sub.surface) {  // immediate: drop from both applied and pending stacks
    std::erase(parent->stack, sub.surface);
    std::erase(parent->pending_stack, sub.surface);
  }
  if (sub.surface) sub.surface->as_subsurface = nullptr;
  sub.surface = nullptr;
  sub.parent = nullptr;
  if (parent) ApplyAndPublish(s, *parent);
}

void NewSurface(Impl& s, wl_client* client, int version, uint32_t id) {
  Surface* sp = &*s.surfaces.emplace(client, version, id);
  auto* res = &sp->res;
  s.surface_by_res[res->resource()] = sp;

  res->setAttach([&s, sp](CWlSurface*, wl_resource* buffer, int32_t, int32_t) {
    sp->pending_buffer = buffer;
    sp->buffer_attached = true;
  });
  res->setFrame(
      [&s, sp](CWlSurface* r, uint32_t cb_id) { sp->frame_cbs.emplace(r->client(), 1, cb_id); });
  res->setSetInputRegion([&s, sp](CWlSurface*, wl_resource* region_res) {
    InputRegion ir;
    if (region_res) {
      ir.infinite = false;
      if (Region* rg = RegionOrNull(s, region_res)) ir.ops = rg->ops;
    }
    sp->pending_input_region = std::move(ir);
  });
  res->setSetBufferScale([sp](CWlSurface* r, int32_t scale) {
    if (scale <= 0) {
      r->error(WL_SURFACE_ERROR_INVALID_SCALE, "buffer scale must be positive");
      return;
    }
    sp->buffer_scale = scale;
  });
  res->setSetBufferTransform([sp](CWlSurface* r, int32_t transform) {
    if (transform < WL_OUTPUT_TRANSFORM_NORMAL || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
      r->error(WL_SURFACE_ERROR_INVALID_TRANSFORM, "unknown buffer transform");
      return;
    }
    sp->buffer_transform = transform;
  });
  res->setCommit([&s, sp](CWlSurface*) { Commit(s, *sp); });
  res->setOnDestroy([&s, sp](CWlSurface* r) {
    ReleaseHeldDmabuf(s, *sp);
    if (sp->viewport) sp->viewport->surface = nullptr;
    if (s.pointer_surface == sp) s.pointer_surface = nullptr;
    if (s.keyboard_surface == sp) s.keyboard_surface = nullptr;
    // Subsurface role: detach from the parent tree (recomposes the window).
    if (sp->as_subsurface) DetachSubsurface(s, *sp->as_subsurface);
    // Parent of subsurfaces: orphan them so they stop compositing. The pending
    // stack is the authoritative child set (it includes any not yet committed).
    for (Surface* entry : sp->pending_stack)
      if (entry != sp) entry->as_subsurface->parent = nullptr;
    if (sp->xdg) sp->xdg->surface = nullptr;
    s.surface_by_res.erase(r->resource());
    EraseOwned(s.surfaces, sp);
  });
  DestroyOnRequest(*res);
}

// ---------------------------------------------------------------- xdg-shell --

// Moves wl_keyboard focus to `target` (or clears it when null): a leave to the
// old surface, an enter to the new one. Used both for the toplevel (click to
// focus) and for popup grabs (a menu takes the keyboard so arrow keys work).
void KeyboardFocusTo(Impl& s, Surface* target) {
  if (s.keyboard_surface == target) return;
  if (s.keyboard_surface) {
    wl_client* c = s.keyboard_surface->res.client();
    for (auto& k : s.keyboards)
      if (k.client() == c) k.sendLeaveRaw(s.serial++, s.keyboard_surface->res.resource());
  }
  s.keyboard_surface = target;
  if (target) {
    wl_client* c = target->res.client();
    wl_array keys;
    wl_array_init(&keys);
    for (auto& k : s.keyboards) {
      if (k.client() != c) continue;
      // The spec requires the enter event first, then the modifiers snapshot.
      k.sendEnterRaw(s.serial++, target->res.resource(), &keys);
      k.sendModifiers(s.serial++, 0, 0, 0, 0);
    }
    wl_array_release(&keys);
  }
}

void FocusKeyboardOnPopup(Impl& s, Popup& pp) {
  if (pp.xdg && pp.xdg->surface) KeyboardFocusTo(s, pp.xdg->surface);
}

// On popup dismissal, hand the keyboard back to the owning toplevel so typing in
// the page keeps working.
void RestoreKeyboardAfterPopup(Impl& s, XdgSurf* parent) {
  Surface* target = nullptr;
  if (parent && parent->surface)
    if (Toplevel* t = OwningToplevel(*parent->surface))
      if (t->xdg) target = t->xdg->surface;
  KeyboardFocusTo(s, target);
}

void NewToplevel(Impl& s, XdgSurf& xs, uint32_t id) {
  Toplevel* tp = &*s.toplevels.emplace(xs.res.client(), xs.res.version(), id);
  tp->xdg = &xs;
  xs.toplevel = tp;
  if (xs.surface) xs.surface->toplevel = tp;
  uid_t uid;
  gid_t gid;
  wl_client_get_credentials(xs.res.client(), &tp->pid, &uid, &gid);

  auto* res = &tp->res;
  res->setSetTitle([&s, tp](CXdgToplevel*, const char* title) {
    tp->title = title;
    if (auto win_obj = tp->LockWindow()) {
      auto& win = *win_obj;
      {
        auto lock = std::lock_guard(win.mutex);
        win.title = title;
      }
      win.WakeToys();
    }
  });
  res->setSetAppId([&s, tp](CXdgToplevel*, const char* app_id) {
    tp->app_id = app_id;
    if (auto win_obj = tp->LockWindow()) {
      auto& win = *win_obj;
      auto lock = std::lock_guard(win.mutex);
      win.app_id = app_id;
    }
  });
  res->setOnDestroy([&s, tp](CXdgToplevel*) {
    UnmapToplevel(s, *tp);
    if (tp->close_timer) {
      EraseOwned(s.close_timers, tp->close_timer);
      tp->close_timer = nullptr;
    }
    if (tp->xdg) tp->xdg->toplevel = nullptr;
    if (tp->xdg && tp->xdg->surface) tp->xdg->surface->toplevel = nullptr;
    EraseOwned(s.toplevels, tp);
  });
  DestroyOnRequest(*res);
}

void NewPopup(Impl& s, XdgSurf& xs, uint32_t id, XdgSurf* parent, Positioner* pos) {
  Popup* pp = &*s.popups.emplace(xs.res.client(), xs.res.version(), id);
  pp->xdg = &xs;
  pp->parent = parent;
  xs.popup = pp;
  if (pos) ResolvePopup(*pos, *pp);
  if (parent) parent->child_popups.push_back(pp);

  auto* res = &pp->res;
  // The grab makes this popup the input focus: keyboard goes to it and a press
  // outside the popup chain dismisses it (see RoutePointer / SendPointerButton).
  res->setGrab([&s, pp](CXdgPopup*, wl_resource*, uint32_t) {
    s.grabbing_popup = pp;
    FocusKeyboardOnPopup(s, *pp);
  });
  res->setReposition([&s, pp](CXdgPopup*, wl_resource* positioner_res, uint32_t token) {
    if (Positioner* np = PositionerOrNull(s, positioner_res)) ResolvePopup(*np, *pp);
    pp->res.sendRepositioned(token);
    pp->res.sendConfigure(pp->geo.x(), pp->geo.y(), pp->geo.width(), pp->geo.height());
    if (pp->xdg) SendXdgConfigure(s, *pp->xdg);
    if (pp->parent && pp->parent->surface) ApplyAndPublish(s, *pp->parent->surface);
  });
  res->setOnDestroy([&s, pp](CXdgPopup*) {
    XdgSurf* parent = pp->parent;
    if (parent) std::erase(parent->child_popups, pp);
    if (pp->xdg) pp->xdg->popup = nullptr;
    if (s.grabbing_popup == pp) {
      s.grabbing_popup = nullptr;
      RestoreKeyboardAfterPopup(s, parent);
    }
    if (parent && parent->surface) ApplyAndPublish(s, *parent->surface);
    EraseOwned(s.popups, pp);
  });
  // Destroying a popup that still has child popups is the not_the_topmost_popup
  // error (a popup chain must be torn down from the innermost outwards).
  res->setDestroy([pp](CXdgPopup* r) {
    if (pp->xdg && !pp->xdg->child_popups.empty() && pp->xdg->wm_base)
      pp->xdg->wm_base->error(XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
                              "destroyed a popup that is not the topmost");
    else
      wl_resource_destroy(r->resource());
  });
}

void NewXdgSurface(Impl& s, CXdgWmBase* wm_base, uint32_t id, wl_resource* surface_res) {
  XdgSurf* xp = &*s.xdg_surfaces.emplace(wm_base->client(), wm_base->version(), id);
  xp->wm_base = wm_base;
  xp->surface = SurfaceOrNull(s, surface_res);
  if (xp->surface) xp->surface->xdg = xp;

  auto* res = &xp->res;
  res->setGetToplevel([&s, xp](CXdgSurface*, uint32_t id) { NewToplevel(s, *xp, id); });
  res->setGetPopup(
      [&s, xp](CXdgSurface*, uint32_t id, wl_resource* parent_res, wl_resource* positioner_res) {
        NewPopup(s, *xp, id, XdgSurfOrNull(s, parent_res), PositionerOrNull(s, positioner_res));
      });
  res->setSetWindowGeometry([xp](CXdgSurface*, int32_t x, int32_t y, int32_t w, int32_t h) {
    xp->geo = SkIRect::MakeXYWH(x, y, w, h);
  });
  res->setAckConfigure([xp](CXdgSurface* r, uint32_t serial) {
    if (serial > xp->last_configure_serial)
      r->error(XDG_SURFACE_ERROR_INVALID_SERIAL, "ack of a configure that was never sent");
  });
  res->setOnDestroy([&s, xp](CXdgSurface*) {
    if (xp->toplevel) xp->toplevel->xdg = nullptr;
    if (xp->popup) xp->popup->xdg = nullptr;
    if (xp->surface) xp->surface->xdg = nullptr;
    EraseOwned(s.xdg_surfaces, xp);
  });
  // Destroying an xdg_surface while its toplevel/popup role object still lives is
  // the defunct_role_object error; otherwise tear the surface down normally.
  res->setDestroy([xp](CXdgSurface* r) {
    if (xp->toplevel || xp->popup)
      r->error(XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
               "xdg_surface destroyed before its role object");
    else
      wl_resource_destroy(r->resource());
  });
}

// ------------------------------------------------------------------ globals --

void BindCompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CWlCompositor* rp = &*s.compositor_resources.emplace(client, version, id);
  rp->setCreateSurface(
      [&s](CWlCompositor* r, uint32_t id) { NewSurface(s, r->client(), r->version(), id); });
  rp->setCreateRegion([&s](CWlCompositor* r, uint32_t id) {
    Region* gp = &*s.regions.emplace(r->client(), r->version(), id);
    gp->res.setAdd([gp](CWlRegion*, int32_t x, int32_t y, int32_t w, int32_t h) {
      gp->ops.push_back({SkIRect::MakeXYWH(x, y, w, h), true});
    });
    gp->res.setSubtract([gp](CWlRegion*, int32_t x, int32_t y, int32_t w, int32_t h) {
      gp->ops.push_back({SkIRect::MakeXYWH(x, y, w, h), false});
    });
    gp->res.setOnDestroy([&s, gp](CWlRegion*) { EraseOwned(s.regions, gp); });
    DestroyOnRequest(gp->res);
  });
  rp->setOnDestroy([&s, rp](CWlCompositor*) { EraseOwned(s.compositor_resources, rp); });
}

void BindSubcompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CWlSubcompositor* rp = &*s.subcompositors.emplace(client, version, id);
  rp->setGetSubsurface([&s](CWlSubcompositor* r, uint32_t id, wl_resource* surface_res,
                            wl_resource* parent_res) {
    Surface* child = SurfaceOrNull(s, surface_res);
    Surface* parent = SurfaceOrNull(s, parent_res);
    // The child must be role-less; an existing subsurface or xdg role is the
    // protocol's bad_surface error.
    if (child && (child->as_subsurface || child->xdg)) {
      r->error(WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "the surface already has a role");
      return;
    }
    // The parent must not be the child or one of its descendants; such a loop
    // is the bad_parent error (and keeps the tree walks finite).
    for (Surface* a = parent; a; a = a->as_subsurface ? a->as_subsurface->parent : nullptr) {
      if (a == child) {
        r->error(WL_SUBCOMPOSITOR_ERROR_BAD_PARENT, "the parent is the surface or a descendant");
        return;
      }
    }
    Subsurface* sub = &*s.subsurfaces.emplace(r->client(), r->version(), id);
    sub->surface = child;
    sub->parent = parent;
    if (child) child->as_subsurface = sub;
    if (parent && child) parent->pending_stack.push_back(child);  // newest on top
    auto* sr = &sub->res;
    sr->setSetPosition([sub](CWlSubsurface*, int32_t x, int32_t y) {
      sub->pending_pos = {x, y};
      sub->position_dirty = true;
    });
    sr->setSetSync([sub](CWlSubsurface*) { sub->sync = true; });
    sr->setSetDesync([sub](CWlSubsurface*) { sub->sync = false; });
    sr->setPlaceAbove([&s, sub](CWlSubsurface*, wl_resource* sibling) {
      RestackSubsurface(s, *sub, sibling, true);
    });
    sr->setPlaceBelow([&s, sub](CWlSubsurface*, wl_resource* sibling) {
      RestackSubsurface(s, *sub, sibling, false);
    });
    sr->setOnDestroy([&s, sub](CWlSubsurface*) {
      DetachSubsurface(s, *sub);
      EraseOwned(s.subsurfaces, sub);
    });
    DestroyOnRequest(*sr);
  });
  rp->setOnDestroy([&s, rp](CWlSubcompositor*) { EraseOwned(s.subcompositors, rp); });
  DestroyOnRequest(*rp);
}

DataSource* DataSourceOrNull(Impl& s, wl_resource* res) {
  if (!res) return nullptr;
  for (auto& src : s.data_sources)
    if (src.res.resource() == res) return &src;
  return nullptr;
}

void OfferSelectionTo(Impl& s, CWlDataDevice& dev) {
  if (!s.selection) {
    dev.sendSelection(nullptr);
    return;
  }
  DataOffer* offer = &*s.data_offers.emplace(dev.client(), dev.version(), 0);
  offer->source = s.selection;
  offer->res.setReceive([offer](CWlDataOffer*, const char* mime, int32_t fd) {
    // Broker the paste: hand the destination's fd to the source to fill, flush so
    // the fd is on the wire before we drop our copy. A gone source just closes the
    // fd, unblocking the reader.
    if (offer->source) {
      offer->source->res.sendSend(mime, fd);
      wl_client_flush(offer->source->res.client());
    }
    close(fd);
  });
  offer->res.setAccept([](CWlDataOffer*, uint32_t, const char*) {});
  offer->res.setSetActions([](CWlDataOffer*, uint32_t, uint32_t) {});
  offer->res.setFinish([](CWlDataOffer*) {});
  offer->res.setOnDestroy([&s, offer](CWlDataOffer*) { EraseOwned(s.data_offers, offer); });
  DestroyOnRequest(offer->res);
  dev.sendDataOffer(&offer->res);
  for (auto& mime : s.selection->mimes) offer->res.sendOffer(mime.c_str());
  dev.sendSelection(&offer->res);
}

void SetSelection(Impl& s, wl_resource* source_res) {
  DataSource* src = DataSourceOrNull(s, source_res);
  if (s.selection && s.selection != src) s.selection->res.sendCancelled();
  s.selection = src;
  for (auto& dev : s.data_devices) OfferSelectionTo(s, dev);
}

void BindDataDeviceManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CWlDataDeviceManager* rp = &*s.data_device_managers.emplace(client, version, id);
  rp->setCreateDataSource([&s](CWlDataDeviceManager* r, uint32_t id) {
    DataSource* src = &*s.data_sources.emplace(r->client(), r->version(), id);
    src->res.setOffer([src](CWlDataSource*, const char* mime) { src->mimes.push_back(mime); });
    src->res.setSetActions([](CWlDataSource*, uint32_t) {});
    src->res.setOnDestroy([&s, src](CWlDataSource*) {
      if (s.selection == src) s.selection = nullptr;
      for (auto& o : s.data_offers)
        if (o.source == src) o.source = nullptr;
      EraseOwned(s.data_sources, src);
    });
    DestroyOnRequest(src->res);
  });
  rp->setGetDataDevice([&s](CWlDataDeviceManager* r, uint32_t id, wl_resource*) {
    CWlDataDevice* dev = &*s.data_devices.emplace(r->client(), r->version(), id);
    dev->setSetSelection(
        [&s](CWlDataDevice*, wl_resource* source_res, uint32_t) { SetSelection(s, source_res); });
    dev->setStartDrag([](CWlDataDevice*, wl_resource*, wl_resource*, wl_resource*, uint32_t) {});
    dev->setOnDestroy([&s, dev](CWlDataDevice*) { EraseOwned(s.data_devices, dev); });
    ReleaseOnRequest(*dev);
    OfferSelectionTo(s, *dev);
  });
  rp->setOnDestroy([&s, rp](CWlDataDeviceManager*) { EraseOwned(s.data_device_managers, rp); });
}

void BindOutput(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CWlOutput* rp = &*s.outputs.emplace(client, version, id);
  rp->setOnDestroy([&s, rp](CWlOutput*) { EraseOwned(s.outputs, rp); });
  ReleaseOnRequest(*rp);
  rp->sendGeometry(0, 0, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Automat", "Board",
                   WL_OUTPUT_TRANSFORM_NORMAL);
  rp->sendMode((wl_output_mode)(WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED), 1920, 1080,
               60000);
  if (version >= 4) rp->sendName("AUTOMAT-1");
  if (version >= 2) {
    rp->sendScale(1);
    rp->sendDone();
  }
}

// Builds the xkb keymap once, serialized into a memfd that every
// wl_keyboard.keymap event shares. Keycodes pass through Automat verbatim
// (AnsiKey round-trips through the fixed x11 tables), so clients must see the
// HOST's real keymap - the X server's - or every non-qwerty layout garbles.
void InitKeymap(Impl& s) {
  xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  xkb_keymap* keymap = nullptr;
  if (xcb::connection) {
    xkb_x11_setup_xkb_extension(xcb::connection, XKB_X11_MIN_MAJOR_XKB_VERSION,
                                XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                nullptr, nullptr, nullptr, nullptr);
    int32_t device = xkb_x11_get_core_keyboard_device_id(xcb::connection);
    if (device >= 0) {
      keymap =
          xkb_x11_keymap_new_from_device(ctx, xcb::connection, device, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
  }
  if (!keymap) {
    keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
  }
  if (!keymap) {
    ERROR << "Wayland: couldn't build an xkb keymap; keyboard input disabled.";
    xkb_context_unref(ctx);
    return;
  }
  char* str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  s.keymap_size = strlen(str) + 1;
  s.keymap_fd = memfd_create("automat-keymap", MFD_CLOEXEC);
  if (s.keymap_fd >= 0) {
    (void)!write(s.keymap_fd, str, s.keymap_size);
  }
  auto Mask = [&](const char* name) -> uint32_t {
    xkb_mod_index_t i = xkb_keymap_mod_get_index(keymap, name);
    return i == XKB_MOD_INVALID ? 0 : (1u << i);
  };
  s.mod_shift = Mask(XKB_MOD_NAME_SHIFT);
  s.mod_ctrl = Mask(XKB_MOD_NAME_CTRL);
  s.mod_alt = Mask(XKB_MOD_NAME_ALT);
  s.mod_super = Mask(XKB_MOD_NAME_LOGO);
  free(str);
  xkb_keymap_unref(keymap);
  xkb_context_unref(ctx);
}

void BindSeat(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CWlSeat* rp = &*s.seats.emplace(client, version, id);
  rp->setGetPointer([&s](CWlSeat* r, uint32_t id) {
    CWlPointer* pp = &*s.pointers.emplace(r->client(), r->version(), id);
    pp->setOnDestroy([&s, pp](CWlPointer*) { EraseOwned(s.pointers, pp); });
    ReleaseOnRequest(*pp);
  });
  rp->setGetKeyboard([&s](CWlSeat* r, uint32_t id) {
    CWlKeyboard* kp = &*s.keyboards.emplace(r->client(), r->version(), id);
    kp->setOnDestroy([&s, kp](CWlKeyboard*) { EraseOwned(s.keyboards, kp); });
    ReleaseOnRequest(*kp);
    if (s.keymap_fd >= 0) {
      kp->sendKeymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, s.keymap_fd, s.keymap_size);
    }
    // Automat forwards the host's key auto-repeat (one wl_keyboard.key per X
    // repeat event), so clients must not also repeat on their own — otherwise a
    // held key repeats twice. Advertise no client-side repeat.
    if (kp->version() >= 4) kp->sendRepeatInfo(0, 0);
  });
  rp->setOnDestroy([&s, rp](CWlSeat*) { EraseOwned(s.seats, rp); });
  ReleaseOnRequest(*rp);
  rp->sendCapabilities(
      (wl_seat_capability)(WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD));
  if (version >= 2) rp->sendName("automat");
}

void BindDecorationManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CZxdgDecorationManagerV1* rp = &*s.decoration_managers.emplace(client, version, id);
  rp->setGetToplevelDecoration(
      [&s](CZxdgDecorationManagerV1* r, uint32_t id, wl_resource* toplevel_res) {
        CZxdgToplevelDecorationV1* dp = &*s.decorations.emplace(r->client(), r->version(), id);
        // Automat always draws the chrome; every mode request gets server-side.
        // (No xdg_surface.configure here: the initial commit handshake sends
        // the only legal pre-map configure bundle.)
        auto confirm = [dp](auto*, auto...) {
          dp->sendConfigure(ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        };
        dp->setSetMode(confirm);
        dp->setUnsetMode(confirm);
        dp->sendConfigure(ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        dp->setOnDestroy([&s, dp](CZxdgToplevelDecorationV1*) { EraseOwned(s.decorations, dp); });
        DestroyOnRequest(*dp);
      });
  rp->setOnDestroy([&s, rp](CZxdgDecorationManagerV1*) { EraseOwned(s.decoration_managers, rp); });
  DestroyOnRequest(*rp);
}

void BindXdgWmBase(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CXdgWmBase* rp = &*s.wm_bases.emplace(client, version, id);
  rp->setGetXdgSurface(
      [&s](CXdgWmBase* r, uint32_t id, wl_resource* surface) { NewXdgSurface(s, r, id, surface); });
  rp->setCreatePositioner([&s](CXdgWmBase* r, uint32_t id) {
    Positioner* pp = &*s.positioners.emplace(r->client(), r->version(), id);
    auto* pr = &pp->res;
    pr->setSetSize([pp](CXdgPositioner*, int32_t w, int32_t h) { pp->size = {w, h}; });
    pr->setSetAnchorRect([pp](CXdgPositioner*, int32_t x, int32_t y, int32_t w, int32_t h) {
      pp->anchor_rect = SkIRect::MakeXYWH(x, y, w, h);
    });
    pr->setSetAnchor([pp](CXdgPositioner*, xdgPositionerAnchor a) { pp->anchor = a; });
    pr->setSetGravity([pp](CXdgPositioner*, xdgPositionerGravity g) { pp->gravity = g; });
    pr->setSetOffset([pp](CXdgPositioner*, int32_t x, int32_t y) { pp->offset = {x, y}; });
    pr->setSetConstraintAdjustment([pp](CXdgPositioner*, xdgPositionerConstraintAdjustment ca) {
      pp->constraint_adjustment = ca;
    });
    pr->setOnDestroy([&s, pp](CXdgPositioner*) { EraseOwned(s.positioners, pp); });
    DestroyOnRequest(pp->res);
  });
  rp->setPong([](CXdgWmBase*, uint32_t) {});
  rp->setOnDestroy([&s, rp](CXdgWmBase*) { EraseOwned(s.wm_bases, rp); });
  DestroyOnRequest(*rp);
}

// -------------------------------------------------------------- viewporter --

// wp_viewporter lets a client crop and scale a surface independently of its
// buffer size: a source rectangle selects part of the buffer and a destination
// size sets the surface size it is scaled to. Both pieces are double-buffered;
// they are read at commit (ResolveGeometry) and the surface's content rectangle
// and board size follow. Used by clients that render at a fractional scale or
// hand over an oversized buffer and crop it.
void BindViewporter(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CWpViewporter* rp = &*s.viewporters.emplace(client, version, id);
  rp->setGetViewport([&s](CWpViewporter* r, uint32_t id, wl_resource* surface_res) {
    Surface* surf = SurfaceOrNull(s, surface_res);
    if (surf && surf->viewport) {
      r->error(WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, "surface already has a viewport");
      return;
    }
    Viewport* vp = &*s.viewports.emplace(r->client(), r->version(), id);
    vp->surface = surf;
    if (surf) surf->viewport = vp;
    auto* vr = &vp->res;
    vr->setSetSource([vp](CWpViewport* r, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h) {
      if (!vp->surface) {
        r->error(WP_VIEWPORT_ERROR_NO_SURFACE, "the wl_surface was destroyed");
        return;
      }
      wl_fixed_t unset = wl_fixed_from_int(-1);
      if (x == unset && y == unset && w == unset && h == unset) {
        vp->src_x = vp->src_y = vp->src_w = vp->src_h = -1;
        return;
      }
      if (x < 0 || y < 0 || w <= 0 || h <= 0) {
        r->error(WP_VIEWPORT_ERROR_BAD_VALUE, "negative origin or non-positive source size");
        return;
      }
      vp->src_x = wl_fixed_to_double(x);
      vp->src_y = wl_fixed_to_double(y);
      vp->src_w = wl_fixed_to_double(w);
      vp->src_h = wl_fixed_to_double(h);
    });
    vr->setSetDestination([vp](CWpViewport* r, int32_t w, int32_t h) {
      if (!vp->surface) {
        r->error(WP_VIEWPORT_ERROR_NO_SURFACE, "the wl_surface was destroyed");
        return;
      }
      if (w == -1 && h == -1) {
        vp->dst_size = {-1, -1};
        return;
      }
      if (w <= 0 || h <= 0) {
        r->error(WP_VIEWPORT_ERROR_BAD_VALUE, "non-positive destination size");
        return;
      }
      vp->dst_size = {w, h};
    });
    vr->setOnDestroy([&s, vp](CWpViewport*) {
      if (vp->surface) vp->surface->viewport = nullptr;
      EraseOwned(s.viewports, vp);
    });
    DestroyOnRequest(vp->res);
  });
  rp->setOnDestroy([&s, rp](CWpViewporter*) { EraseOwned(s.viewporters, rp); });
  DestroyOnRequest(*rp);
}

// ---------------------------------------------------------------- cursor shape --

// Advertise named cursors so GTK clients use them instead of wl_pointer.set_cursor,
// whose cursor buffer we don't render.
void BindCursorShapeManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CWpCursorShapeManagerV1* rp = &*s.cursor_shape_managers.emplace(client, version, id);
  auto make_device = [&s](wl_client* c, uint32_t ver, uint32_t id) {
    CWpCursorShapeDeviceV1* dp = &*s.cursor_shape_devices.emplace(c, ver, id);
    dp->setSetShape([&s](CWpCursorShapeDeviceV1*, uint32_t, wpCursorShapeDeviceV1Shape shape) {
      if (!s.pointer_surface) return;
      if (Toplevel* t = OwningToplevel(*s.pointer_surface))
        if (auto win = t->LockWindow()) {
          win->cursor_shape.store(shape, std::memory_order_relaxed);
          win->WakeToys();
        }
    });
    dp->setOnDestroy([&s, dp](CWpCursorShapeDeviceV1*) { EraseOwned(s.cursor_shape_devices, dp); });
    DestroyOnRequest(*dp);
  };
  rp->setGetPointer([make_device](CWpCursorShapeManagerV1* r, uint32_t id, wl_resource*) {
    make_device(r->client(), r->version(), id);
  });
  rp->setGetTabletToolV2([make_device](CWpCursorShapeManagerV1* r, uint32_t id, wl_resource*) {
    make_device(r->client(), r->version(), id);
  });
  rp->setOnDestroy([&s, rp](CWpCursorShapeManagerV1*) { EraseOwned(s.cursor_shape_managers, rp); });
  DestroyOnRequest(*rp);
}

// ------------------------------------------------------------------ dmabuf --

// The format/modifier pairs the compositor accepts. AR24/XR24 are the 32-bit
// BGRA formats every toolkit produces; both import as VK_FORMAT_B8G8R8A8_UNORM.
// LINEAR is what the all-software Mesa stack allocates here; INVALID lets a
// client send an implicitly-allocated buffer.
struct DmabufFormatModifier {
  uint32_t format;
  uint64_t modifier;
};
constexpr DmabufFormatModifier kDmabufFormats[] = {
    {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR},
    {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR},
    {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID},
    {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID},
};
constexpr int kDmabufFormatCount = (int)(sizeof(kDmabufFormats) / sizeof(kDmabufFormats[0]));

// Builds the version 4 feedback inputs once: the render node clients allocate on
// (its dev_t becomes main_device) and a sealed format table the feedback events
// reference by index.
void InitDmabuf(Impl& s) {
  struct stat st;
  if (stat("/dev/dri/renderD128", &st) == 0) {
    s.dmabuf_main_device = st.st_rdev;
    s.dmabuf_has_device = true;
  } else {
    ERROR << "Wayland dmabuf: can't stat /dev/dri/renderD128; clients fall back to shm.";
  }
  struct TableEntry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
  };
  static_assert(sizeof(TableEntry) == 16, "dmabuf format table entry must be 16 bytes");
  TableEntry table[kDmabufFormatCount];
  for (int i = 0; i < kDmabufFormatCount; ++i) {
    table[i] = {kDmabufFormats[i].format, 0, kDmabufFormats[i].modifier};
  }
  int fd = memfd_create("automat-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    ERROR << "Wayland dmabuf: memfd_create failed; feedback disabled.";
    return;
  }
  if (write(fd, table, sizeof(table)) != (ssize_t)sizeof(table)) {
    close(fd);
    return;
  }
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
  s.dmabuf_format_table_fd = fd;
  s.dmabuf_format_table_size = (uint32_t)sizeof(table);
}

// Sends a full feedback bundle: the format table, the main device, then one
// tranche (same device, every advertised format) closed by done. This is the
// only way Mesa learns which DRM device to allocate on; without it Mesa clients
// silently fall back to wl_shm.
void SendDmabufFeedback(Impl& s, CZwpLinuxDmabufFeedbackV1* fb) {
  if (s.dmabuf_format_table_fd >= 0) {
    fb->sendFormatTable(s.dmabuf_format_table_fd, s.dmabuf_format_table_size);
  }
  wl_array device;
  wl_array_init(&device);
  if (s.dmabuf_has_device) {
    memcpy(wl_array_add(&device, sizeof(dev_t)), &s.dmabuf_main_device, sizeof(dev_t));
  }
  fb->sendMainDevice(&device);
  fb->sendTrancheTargetDevice(&device);
  fb->sendTrancheFlags((zwpLinuxDmabufFeedbackV1TrancheFlags)0);
  wl_array indices;
  wl_array_init(&indices);
  for (uint16_t i = 0; i < kDmabufFormatCount; ++i) {
    *(uint16_t*)wl_array_add(&indices, sizeof(uint16_t)) = i;
  }
  fb->sendTrancheFormats(&indices);
  fb->sendTrancheDone();
  fb->sendDone();
  wl_array_release(&indices);
  wl_array_release(&device);
}

// Legacy version 1-3 advertisement; version 4 clients use SendDmabufFeedback and
// must not receive these events.
void SendDmabufFormats(CZwpLinuxDmabufV1* r) {
  for (auto& fm : kDmabufFormats) {
    r->sendFormat(fm.format);
    if (r->version() >= 3) {
      r->sendModifier(fm.format, (uint32_t)(fm.modifier >> 32),
                      (uint32_t)(fm.modifier & 0xffffffffu));
    }
  }
}

// Turns finished params into a wl_buffer. `buffer_id` is the client-supplied id
// for create_immed, or 0 to let the server allocate one for create. Plane fd
// ownership moves from the params into the buffer.
DmabufBuffer* MakeDmabufBuffer(Impl& s, DmabufParams& p, uint32_t buffer_id, SkISize size,
                               uint32_t format, bool y_invert) {
  DmabufBuffer* b = &*s.dmabuf_buffers.emplace(p.res.client(), 1, buffer_id);
  b->size = size;
  b->drm_format = format;
  b->modifier = p.modifier;
  b->y_invert = y_invert;
  b->plane_count = p.plane_count;
  for (int i = 0; i < p.plane_count; ++i) {
    b->planes[i] = p.planes[i];
    p.planes[i].fd = -1;  // ownership moves to the buffer
  }
  s.dmabuf_by_res[b->res.resource()] = b;
  b->res.setOnDestroy([&s, b, res = b->res.resource()](CWlBuffer* r) {
    // Drop any dangling references before the buffer's storage goes away.
    for (auto& surf : s.surfaces) {
      if (surf.held_dmabuf == res) surf.held_dmabuf = nullptr;
      if (surf.pending_buffer == res) {
        surf.pending_buffer = nullptr;
        surf.buffer_attached = false;
      }
    }
    s.dmabuf_by_res.erase(res);
    EraseOwned(s.dmabuf_buffers, b);
  });
  DestroyOnRequest(b->res);
  return b;
}

void BindLinuxDmabuf(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Impl*)data;
  CZwpLinuxDmabufV1* rp = &*s.dmabuf_globals.emplace(client, version, id);
  rp->setCreateParams([&s](CZwpLinuxDmabufV1* r, uint32_t id) {
    DmabufParams* pp = &*s.dmabuf_params.emplace(r->client(), r->version(), id);
    auto* pr = &pp->res;
    pr->setAdd([pp](CZwpLinuxBufferParamsV1* r, int32_t fd, uint32_t plane_idx, uint32_t offset,
                    uint32_t stride, uint32_t mod_hi, uint32_t mod_lo) {
      if (plane_idx >= 4) {
        r->error(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane index out of bounds");
        close(fd);
        return;
      }
      if (pp->planes[plane_idx].fd >= 0) {
        r->error(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane already set");
        close(fd);
        return;
      }
      pp->planes[plane_idx] = {fd, offset, stride};
      if ((int)plane_idx + 1 > pp->plane_count) pp->plane_count = plane_idx + 1;
      pp->modifier = ((uint64_t)mod_hi << 32) | mod_lo;
    });
    pr->setCreate([&s, pp](CZwpLinuxBufferParamsV1* r, int32_t width, int32_t height,
                           uint32_t format, zwpLinuxBufferParamsV1Flags flags) {
      if (pp->used) {
        r->error(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
        return;
      }
      pp->used = true;
      if (width <= 0 || height <= 0 || pp->plane_count == 0) {
        r->sendFailed();
        return;
      }
      DmabufBuffer* b = MakeDmabufBuffer(s, *pp, 0, {width, height}, format,
                                         flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT);
      r->sendCreated(b->res.resource());
    });
    pr->setCreateImmed([&s, pp](CZwpLinuxBufferParamsV1* r, uint32_t buffer_id, int32_t width,
                                int32_t height, uint32_t format,
                                zwpLinuxBufferParamsV1Flags flags) {
      if (pp->used) {
        r->error(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used");
        return;
      }
      pp->used = true;
      if (width <= 0 || height <= 0 || pp->plane_count == 0) {
        r->error(ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "missing planes or bad dimensions");
        return;
      }
      MakeDmabufBuffer(s, *pp, buffer_id, {width, height}, format,
                       flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT);
    });
    pr->setOnDestroy([&s, pp](CZwpLinuxBufferParamsV1*) { EraseOwned(s.dmabuf_params, pp); });
    DestroyOnRequest(*pr);
  });
  auto make_feedback = [&s](CZwpLinuxDmabufV1* r, uint32_t id) {
    CZwpLinuxDmabufFeedbackV1* fb = &*s.dmabuf_feedbacks.emplace(r->client(), r->version(), id);
    fb->setOnDestroy([&s, fb](CZwpLinuxDmabufFeedbackV1*) { EraseOwned(s.dmabuf_feedbacks, fb); });
    DestroyOnRequest(*fb);
    SendDmabufFeedback(s, fb);
  };
  rp->setGetDefaultFeedback(make_feedback);
  rp->setGetSurfaceFeedback(
      [make_feedback](CZwpLinuxDmabufV1* r, uint32_t id, wl_resource*) { make_feedback(r, id); });
  rp->setOnDestroy([&s, rp](CZwpLinuxDmabufV1*) { EraseOwned(s.dmabuf_globals, rp); });
  DestroyOnRequest(*rp);
  // Version 4 clients use the feedback above; only version 1-3 get the legacy
  // per-format/modifier events (the protocol forbids mixing the two).
  if (version < 4) SendDmabufFormats(rp);
}

void WaylandThread(Impl* s, std::stop_token stop) {
  SetThreadName("Wayland");
  s->display = wl_display_create();
  if (!s->display) {
    ERROR << "Wayland compositor disabled: wl_display_create failed.";
    return;
  }
  s->loop = wl_display_get_event_loop(s->display);
  // The socket is optional: without it clients can't connect, but the thread
  // still runs so the Server stays alive until shutdown.
  if (getenv("XDG_RUNTIME_DIR")) {
    if (const char* socket = wl_display_add_socket_auto(s->display)) {
      s->socket_name = socket;
    } else {
      ERROR << "Wayland: couldn't open a socket; clients can't connect.";
    }
  } else {
    ERROR << "Wayland: XDG_RUNTIME_DIR unset; clients can't connect.";
  }
  wl_display_init_shm(s->display);
  wl_global_create(s->display, &wl_compositor_interface, 6, s, BindCompositor);
  wl_global_create(s->display, &wl_subcompositor_interface, 1, s, BindSubcompositor);
  wl_global_create(s->display, &wl_output_interface, 4, s, BindOutput);
  wl_global_create(s->display, &wl_seat_interface, 5, s, BindSeat);
  wl_global_create(s->display, &wl_data_device_manager_interface, 3, s, BindDataDeviceManager);
  wl_global_create(s->display, &xdg_wm_base_interface, 6, s, BindXdgWmBase);
  wl_global_create(s->display, &zxdg_decoration_manager_v1_interface, 1, s, BindDecorationManager);
  wl_global_create(s->display, &zwp_linux_dmabuf_v1_interface, 4, s, BindLinuxDmabuf);
  wl_global_create(s->display, &wp_viewporter_interface, 1, s, BindViewporter);
  wl_global_create(s->display, &wp_cursor_shape_manager_v1_interface, 2, s, BindCursorShapeManager);
  InitKeymap(*s);
  InitDmabuf(*s);

  Status status;
  s->epoll.Init(status);
  s->wayland_listener.impl = s;
  s->wayland_listener.fd = wl_event_loop_get_fd(s->loop);
  s->epoll.Add(&s->wayland_listener, status);

  // The frame timer paces wl_surface.frame callbacks (armed on demand in Commit).
  s->frame_timer = &*s->close_timers.emplace(s->epoll);
  s->frame_timer->handler = [s] { CompletePendingFrames(*s); };

  s->ready = true;
  vm.WakeToys();
  if (!s->socket_name.empty()) LOG << "Wayland compositor listening on " << s->socket_name;

  if (OK(status)) s->epoll.Loop(status, false, stop);
  if (!OK(status)) ERROR << "Wayland epoll loop stopped: " << status.ToStr();
  s->ready = false;
  wl_display_flush_clients(s->display);
  wl_display_destroy_clients(s->display);
  s->wayland_listener.fd.fd = -1;
  wl_display_destroy(s->display);
  if (s->keymap_fd >= 0) close(s->keymap_fd);
}

Toplevel* FindToplevel(Impl& s, void* handle) {
  for (auto& t : s.toplevels) {
    if (&t == handle) return &t;
  }
  return nullptr;
}

// Runs `fn(impl, toplevel, surface_resource)` on the wayland thread if the
// window's toplevel is still alive.
template <typename Fn>
void PostInput(Impl& impl, WaylandWindow& w, Fn&& fn) {
  if (!impl.ready) return;
  void* handle = w.toplevel_handle.load();
  if (!handle) return;
  impl.epoll.Post([&impl, handle, fn = std::forward<Fn>(fn)] {
    auto* t = FindToplevel(impl, handle);
    if (!t || !t->xdg || !t->xdg->surface) return;
    fn(impl, *t, t->xdg->surface->res.resource());
    // libwayland buffers these events; the epoll Post path (unlike the client-fd
    // path in WaylandListener) never flushes. Without this an idle client only
    // receives the input when it next sends a request of its own — e.g. a
    // cursor-blink commit, ~0.5-1s later — so input appears to lag. Flush now.
    wl_display_flush_clients(impl.display);
  });
}

// Like PostInput, but for any surface in the tree.
template <typename Fn>
void PostSurfaceInput(Impl& impl, WaylandSurface& ws, Fn&& fn) {
  if (!impl.ready) return;
  void* handle = ws.surface_handle.load();
  if (!handle) return;
  impl.epoll.Post([&impl, handle, fn = std::forward<Fn>(fn)] {
    if (Surface* surf = SurfaceOrNull(impl, (wl_resource*)handle)) {
      fn(impl, *surf);
      wl_display_flush_clients(impl.display);
    }
  });
}

void PointerEnterTo(Impl& s, Surface& surf, double lx, double ly) {
  wl_client* c = surf.res.client();
  for (auto& p : s.pointers) {
    if (p.client() != c) continue;
    p.sendEnterRaw(s.serial++, surf.res.resource(), wl_fixed_from_double(lx),
                   wl_fixed_from_double(ly));
    if (p.version() >= 5) p.sendFrame();
  }
}
void PointerLeaveTo(Impl& s, Surface& surf) {
  wl_client* c = surf.res.client();
  for (auto& p : s.pointers) {
    if (p.client() != c) continue;
    p.sendLeaveRaw(s.serial++, surf.res.resource());
    if (p.version() >= 5) p.sendFrame();
  }
}
void PointerMotionTo(Impl& s, Surface& surf, double lx, double ly) {
  wl_client* c = surf.res.client();
  uint32_t time = NowMs();
  for (auto& p : s.pointers) {
    if (p.client() != c) continue;
    p.sendMotion(time, wl_fixed_from_double(lx), wl_fixed_from_double(ly));
    if (p.version() >= 5) p.sendFrame();
  }
}
void PointerButtonTo(Impl& s, Surface& surf, uint32_t button, bool pressed) {
  wl_client* c = surf.res.client();
  uint32_t time = NowMs();
  for (auto& p : s.pointers) {
    if (p.client() != c) continue;
    p.sendButton(s.serial++, time, button,
                 pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    if (p.version() >= 5) p.sendFrame();
  }
}
void PointerAxisTo(Impl& s, Surface& surf, float notches_up) {
  wl_client* c = surf.res.client();
  uint32_t time = NowMs();
  // Wayland's vertical axis is positive downward; our notch count is positive
  // upward, so negate. One detent is ~10 axis units, and version 5 clients also
  // read the integer notch count (axis_discrete) for line-based scrolling.
  //
  // TODO: properly accumulate fractional notches so that discrete eventually increments
  double value = -notches_up * 10.0;
  int discrete = -(int)std::lround(notches_up);
  for (auto& p : s.pointers) {
    if (p.client() != c) continue;
    if (p.version() >= 5) {
      p.sendAxisSource(WL_POINTER_AXIS_SOURCE_WHEEL);
      if (discrete != 0) p.sendAxisDiscrete(WL_POINTER_AXIS_VERTICAL_SCROLL, discrete);
    }
    p.sendAxis(time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(value));
    if (p.version() >= 5) p.sendFrame();
  }
}

// The deepest (topmost) open popup under `xdg`, or null. A press outside it
// dismisses it. Note: Firefox shows context menus on button release, by which
// time the implicit grab serial is gone, so it never calls xdg_popup.grab; the
// compositor must still dismiss the popup on an outside click, which every real
// compositor does.
Popup* TopmostPopup(XdgSurf& xdg) {
  for (auto it = xdg.child_popups.rbegin(); it != xdg.child_popups.rend(); ++it) {
    Popup* pp = *it;
    if (!pp->xdg) continue;
    if (Popup* deeper = TopmostPopup(*pp->xdg)) return deeper;
    return pp;
  }
  return nullptr;
}

}  // namespace

Server::Server(std::stop_token stop) : impl(std::make_unique<Impl>()) {
  impl->thread = std::thread(WaylandThread, impl.get(), std::move(stop));
}

Server::~Server() { impl->thread.join(); }

bool Server::Running() { return impl->ready; }

Str Server::SocketName() { return impl->ready ? impl->socket_name : Str{}; }

void Server::UIFrame() {
  if (!impl->ready) return;
  std::vector<Ptr<Object>> appeared, disappeared;
  {
    auto lock = std::lock_guard(impl->ui_mutex);
    appeared.swap(impl->ui_appeared);
    disappeared.swap(impl->ui_disappeared);
  }
  static int spawn_count = 0;
  for (auto& w : appeared) {
    auto& win = static_cast<WaylandWindow&>(*w);
    // Which Command's child is this client? Its argv becomes the window's
    // respawn recipe, its plate anchors where the window is seated, and the
    // window keeps a Launcher link to it (visible cable, serialized).
    Location* command_location = nullptr;
    library::Command* command = nullptr;
    SkISize win_size = {};
    {
      I64 cpid;
      {
        auto lock = std::lock_guard(win.mutex);
        cpid = win.client_pid;
        win_size = win.dst_size;
      }
      if (cpid) {
        for (auto& loc : vm.root_board->locations) {
          auto* cmd = dynamic_cast<library::Command*>(loc->object.get());
          if (!cmd) continue;
          auto cmd_lock = std::lock_guard(cmd->mutex);
          if (cmd->child_pid == cpid) {
            auto lock = std::lock_guard(win.mutex);
            win.recipe = cmd->argv;
            command_location = loc.get();
            command = cmd;
            break;
          }
        }
      }
    }
    if (command && !win.launcher->IsConnected()) {
      win.launcher->Connect(Interface(command, nullptr));
    }
    auto& loc = vm.root_board->CreateEmpty();
    int n = spawn_count++;
    if (command_location) {
      // Next to its Command: left edge against the plate's right edge, tops
      // aligned; siblings cascade slightly so they stay distinguishable.
      Vec2 plate = library::CommandPlateSize();
      Vec2 size = library::WindowBoardSize(win_size.width(), win_size.height());
      loc.position =
          command_location->position + Vec2(plate.x / 2 + 0.008f + size.x / 2 + 0.006f * (n % 3),
                                            plate.y / 2 - size.y / 2 - 0.012f * (n % 3));
    } else {
      loc.position = Vec2(0.17f + 0.01f * (n % 3), 0.04f - 0.02f * (n % 5));
    }
    loc.InsertHere(std::move(w));
    vm.root_board->WakeToys();
    vm.WakeToys();
  }
  // Respawn deserialized or cloned windows: re-run the recipe and adopt the
  // new client into the existing object.
  for (auto& loc : vm.root_board->locations) {
    auto* win = dynamic_cast<WaylandWindow*>(loc->object.get());
    if (!win) continue;
    Vec<Str> recipe;
    {
      auto lock = std::lock_guard(win->mutex);
      if (!win->pending_respawn) continue;
      win->pending_respawn = false;
      recipe = win->recipe;
    }
    if (recipe.empty()) continue;
    // Prefer the linked Command: it spawns its own argv and keeps ownership
    // of the child (running state, STOP). The recipe is the fallback for
    // windows whose Command is gone or already busy.
    I64 pid = 0;
    Status status;
    auto found = win->launcher->Find();
    if (auto* cmd = dynamic_cast<library::Command*>(found.Owner<Object>())) {
      pid = cmd->AdoptiveLaunch(status);
      if (!pid) status.Reset();
    }
    if (!pid) pid = library::SpawnArgv(recipe, status);
    if (!pid) {
      win->ReportError(status.ToStr());
      continue;
    }
    {
      auto lock = std::lock_guard(win->mutex);
      win->client_pid = pid;
    }
    {
      auto lock = std::lock_guard(impl->adoption_mutex);
      impl->adoptions.emplace_back((pid_t)pid, win->AcquireWeakPtr());
    }
  }
  for (auto& w : disappeared) {
    Location* here = w->here;
    if (here) {
      auto& locations = vm.root_board->locations;
      for (auto it = locations.begin(); it != locations.end(); ++it) {
        if (it->get() == here) {
          locations.erase(it);
          break;
        }
      }
      vm.root_board->WakeToys();
      vm.WakeToys();
    }
  }
}

void Server::NotifyWindowDestroyed(void* handle) {
  if (!handle || !impl->ready) return;
  impl->epoll.Post([this, handle] {
    auto* t = FindToplevel(*impl, handle);
    if (!t) return;
    t->res.sendClose();
    // Same as in PostInput: flush, or the client only sees this on its next request.
    wl_display_flush_clients(impl->display);
    // A client that ignores the request gets SIGTERM after a grace period.
    if (!t->close_timer) {
      auto timer = impl->close_timers.emplace(impl->epoll);
      timer->handler = [pid = t->pid] {
        if (pid > 0) kill(pid, SIGTERM);
      };
      timer->Arm(2.0);
      t->close_timer = &*timer;
    }
  });
}

// ----------------------------------------------------------- input senders --

void Server::SendPointerEnter(WaylandSurface& surf, float lx, float ly) {
  PostSurfaceInput(*impl, surf, [lx, ly](Impl& s, Surface& target) {
    s.pointer_surface = &target;
    PointerEnterTo(s, target, lx, ly);
  });
}

void Server::SendPointerMotion(WaylandSurface& surf, float lx, float ly) {
  PostSurfaceInput(*impl, surf,
                   [lx, ly](Impl& s, Surface& target) { PointerMotionTo(s, target, lx, ly); });
}

void Server::SendPointerButton(WaylandSurface& surf, uint32_t button, bool pressed) {
  PostSurfaceInput(*impl, surf, [button, pressed](Impl& s, Surface& target) {
    // A press that lands on something other than a popup dismisses the topmost
    // open popup (a menu) instead of reaching the client - covering grabbing
    // popups and the non-grabbing ones Firefox uses for context menus.
    if (pressed && !(target.xdg && target.xdg->popup)) {
      if (Toplevel* t = OwningToplevel(target))
        if (t->xdg)
          if (Popup* top = TopmostPopup(*t->xdg)) {
            top->res.sendPopupDone();
            return;
          }
    }
    PointerButtonTo(s, target, button, pressed);
  });
}

void Server::SendPointerAxis(WaylandSurface& surf, float notches_up) {
  PostSurfaceInput(*impl, surf, [notches_up](Impl& s, Surface& target) {
    PointerAxisTo(s, target, notches_up);
  });
}

void Server::SendPointerLeave(WaylandSurface& surf) {
  PostSurfaceInput(*impl, surf, [](Impl& s, Surface& target) {
    if (s.pointer_surface == &target) s.pointer_surface = nullptr;
    PointerLeaveTo(s, target);
  });
}

void Server::SendKeyboardEnter(WaylandWindow& w) {
  PostInput(*impl, w, [](Impl& s, Toplevel& t, wl_resource*) {
    KeyboardFocusTo(s, t.xdg ? t.xdg->surface : nullptr);
  });
}

void Server::SendKeyboardLeave(WaylandWindow& w) {
  PostInput(*impl, w, [](Impl& s, Toplevel&, wl_resource*) {
    if (!s.keyboard_surface) return;
    wl_client* c = s.keyboard_surface->res.client();
    for (auto& k : s.keyboards) {
      if (k.client() != c) continue;
      k.sendLeaveRaw(s.serial++, s.keyboard_surface->res.resource());
    }
    s.keyboard_surface = nullptr;
  });
}

void Server::SendKey(WaylandWindow& w, uint32_t evdev_keycode, bool pressed, bool ctrl, bool alt,
                     bool shift, bool super) {
  PostInput(*impl, w,
            [evdev_keycode, pressed, ctrl, alt, shift, super](Impl& s, Toplevel&, wl_resource*) {
              if (!s.keyboard_surface) return;
              wl_client* c = s.keyboard_surface->res.client();
              uint32_t mods = (ctrl ? s.mod_ctrl : 0) | (alt ? s.mod_alt : 0) |
                              (shift ? s.mod_shift : 0) | (super ? s.mod_super : 0);
              uint32_t time = NowMs();
              for (auto& k : s.keyboards) {
                if (k.client() != c) continue;
                k.sendModifiers(s.serial++, mods, 0, 0, 0);
                k.sendKey(s.serial++, time, evdev_keycode,
                          pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
              }
            });
}

Optional<Server> server;

}  // namespace automat::wayland

#endif  // __linux__
