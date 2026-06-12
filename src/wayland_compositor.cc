// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "wayland_compositor.hh"

#if defined(__linux__)

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <linux/input-event-codes.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include <csignal>

#include "xcb.hh"

#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "automat.hh"
#include "library_command.hh"
#include "library_wayland_window.hh"
#include "log.hh"
#include "thread_name.hh"
#include "time.hh"

// The generated headers #define F std::function; they must come after all
// Automat headers.
#include "../build/generated/wayland.hpp"
#include "../build/generated/xdg-decoration-unstable-v1.hpp"
#include "../build/generated/xdg-shell.hpp"

namespace automat::wayland {

using library::WaylandWindow;

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
  std::vector<std::unique_ptr<CWlCallback>> frame_cbs;
  Toplevel* toplevel = nullptr;

  Surface(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Surface(const Surface&) = delete;
};

// An xdg_surface: the middle of the role chain wl_surface -> xdg_surface ->
// xdg_toplevel. Its job is the configure handshake: after the client commits
// the unmapped surface, the compositor sends one configure bundle (toplevel
// configure first, then xdg_surface.configure with a serial) and nothing may
// send xdg_surface.configure out of band before that - GLFW clients
// disconnect if it happens. The client acks and only then attaches pixels.
struct XdgSurf {
  CXdgSurface res;
  Surface* surface = nullptr;
  Toplevel* toplevel = nullptr;
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
  pid_t pid = 0;                           // client process, for close escalation
  wl_event_source* close_timer = nullptr;  // armed when a close was requested
  WeakPtr<Object> window_weak;
  // Recycled frame allocations: an entry is reclaimed once nothing else
  // (the window's SkImage, the renderer) still references it.
  std::vector<sk_sp<SkData>> frame_pool;

  Toplevel(wl_client* client, int version, uint32_t id) : res(client, version, id) {}
  Toplevel(const Toplevel&) = delete;
};

Ptr<Object> LockWindow(Toplevel& t) { return t.window_weak.Lock(); }

struct Server {
  wl_display* display = nullptr;
  wl_event_loop* loop = nullptr;
  int post_fd = -1;
  Str socket_name;
  std::thread thread;
  bool quit = false;  // wayland thread only
  uint32_t serial = 1;

  std::mutex post_mutex;
  std::vector<std::function<void()>> posted;

  // Protocol objects; wayland thread only.
  std::vector<std::unique_ptr<CWlCompositor>> compositor_resources;
  std::vector<std::unique_ptr<CWlSubcompositor>> subcompositors;
  std::vector<std::unique_ptr<CWlSubsurface>> subsurfaces;
  std::vector<std::unique_ptr<CWlRegion>> regions;
  std::vector<std::unique_ptr<CWlDataDeviceManager>> data_device_managers;
  std::vector<std::unique_ptr<CWlDataDevice>> data_devices;
  std::vector<std::unique_ptr<CWlDataSource>> data_sources;
  std::vector<std::unique_ptr<CWlOutput>> outputs;
  std::vector<std::unique_ptr<CWlSeat>> seats;
  std::vector<std::unique_ptr<CWlPointer>> pointers;
  std::vector<std::unique_ptr<CWlKeyboard>> keyboards;
  std::vector<std::unique_ptr<CXdgWmBase>> wm_bases;
  std::vector<std::unique_ptr<CXdgPositioner>> positioners;
  std::vector<std::unique_ptr<CZxdgDecorationManagerV1>> decoration_managers;
  std::vector<std::unique_ptr<CZxdgToplevelDecorationV1>> decorations;
  std::vector<std::unique_ptr<Surface>> surfaces;
  std::vector<std::unique_ptr<XdgSurf>> xdg_surfaces;
  std::vector<std::unique_ptr<Toplevel>> toplevels;
  std::unordered_map<wl_resource*, Surface*> surface_by_res;
  std::unordered_map<wl_resource*, Toplevel*> toplevel_by_res;

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
  std::vector<std::pair<pid_t, WeakPtr<Object>>> adoptions;

  void Post(std::function<void()> fn) {
    {
      auto lock = std::lock_guard(post_mutex);
      posted.push_back(std::move(fn));
    }
    uint64_t one = 1;
    (void)!write(post_fd, &one, sizeof(one));
  }
};

Server* g_server = nullptr;

template <typename T>
void EraseOwned(std::vector<std::unique_ptr<T>>& v, T* p) {
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (it->get() == p) {
      v.erase(it);
      return;
    }
  }
}

uint32_t NowMs() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             time::SteadyNow().time_since_epoch())
      .count();
}

// ----------------------------------------------------------------- surfaces --

void UnmapToplevel(Server& s, Toplevel& t) {
  if (!t.mapped) return;
  t.mapped = false;
  if (auto win_obj = LockWindow(t)) {
    auto& win = static_cast<WaylandWindow&>(*win_obj);
    win.toplevel_handle.store(nullptr);
    {
      auto lock = std::lock_guard(win.mutex);
      win.client_gone = true;
    }
    win.WakeToys();
    {
      auto lock = std::lock_guard(s.ui_mutex);
      s.ui_disappeared.push_back(std::move(win_obj));
    }
    vm.WakeToys();  // wakes RootWidget::Tick (via PackFrame) so UIFrame drains
  }
  t.window_weak = {};
}

void Commit(Server& s, Surface& surf) {
  Toplevel* t = surf.toplevel;

  if (t && t->xdg && !t->xdg->initial_configure_sent) {
    wl_array states;
    wl_array_init(&states);
    t->res.sendConfigure(0, 0, &states);
    wl_array_release(&states);
    t->xdg->res.sendConfigure(s.serial++);
    t->xdg->initial_configure_sent = true;
  }

  if (surf.buffer_attached) {
    surf.buffer_attached = false;
    wl_resource* buf = surf.pending_buffer;
    surf.pending_buffer = nullptr;
    if (!buf) {
      if (t) UnmapToplevel(s, *t);
    } else if (wl_shm_buffer* shm = wl_shm_buffer_get(buf)) {
      int w = wl_shm_buffer_get_width(shm);
      int h = wl_shm_buffer_get_height(shm);
      int stride = wl_shm_buffer_get_stride(shm);
      uint32_t format = wl_shm_buffer_get_format(shm);
      if (t && w > 0 && h > 0) {
        Ptr<Object> win_obj = LockWindow(*t);
        bool adopted = false;
        if (!win_obj && !t->mapped) {
          {  // A respawned recipe adopts its existing board object.
            auto lock = std::lock_guard(s.adoption_mutex);
            for (auto it = s.adoptions.begin(); it != s.adoptions.end(); ++it) {
              if (it->first == t->pid) {
                win_obj = it->second.Lock();
                s.adoptions.erase(it);
                break;
              }
            }
          }
          if (win_obj) {
            adopted = true;
            auto& win = static_cast<WaylandWindow&>(*win_obj);
            win.toplevel_handle.store(t);
            {
              auto lock = std::lock_guard(win.mutex);
              win.client_gone = false;
              win.client_pid = t->pid;
              if (!t->title.empty()) win.title = t->title;
              win.app_id = t->app_id;
            }
            t->window_weak = win.AcquireWeakPtr();
          } else {
            auto created = MAKE_PTR(WaylandWindow);
            created->toplevel_handle.store(t);
            {
              auto lock = std::lock_guard(created->mutex);
              created->title = t->title;
              created->app_id = t->app_id;
              created->client_pid = t->pid;
            }
            t->window_weak = created->AcquireWeakPtr();
            win_obj = std::move(created);
          }
        }
        if (win_obj) {
          auto& win = static_cast<WaylandWindow&>(*win_obj);
          // The one unavoidable copy (we release the client's buffer right
          // after commit): shm rows land in a pooled allocation, and the
          // SkImage wraps that allocation without copying again.
          size_t need = (size_t)w * 4 * h;
          sk_sp<SkData> data;
          for (auto it = t->frame_pool.begin(); it != t->frame_pool.end(); ++it) {
            if ((*it)->unique() && (*it)->size() == need) {
              data = std::move(*it);
              t->frame_pool.erase(it);
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
          sk_sp<SkImage> img = SkImages::RasterFromData(info, data, (size_t)w * 4);
          t->frame_pool.push_back(std::move(data));
          if (t->frame_pool.size() > 4) t->frame_pool.erase(t->frame_pool.begin());
          {
            auto lock = std::lock_guard(win.mutex);
            win.content = std::move(img);
            win.width = w;
            win.height = h;
            win.content_serial++;
          }
          win.WakeToys();
          vm.WakeToys();
          if (!t->mapped) {
            t->mapped = true;
            if (!adopted) {  // adopted windows are already on the board
              auto lock = std::lock_guard(s.ui_mutex);
              s.ui_appeared.push_back(std::move(win_obj));
            }
          }
        }
      }
      // Copy-release: the client may reuse the buffer immediately.
      wl_buffer_send_release(buf);
    } else {
      // Not a shm buffer (dmabuf comes later); release it so clients don't stall.
      wl_buffer_send_release(buf);
    }
  }

  // Frame callbacks are the client asking "when is it worth drawing the next
  // frame?". Completing them right at commit answers "immediately": honest
  // for input-driven clients like terminals, and the place to add real
  // pacing (tied to board presentation) if animating clients ever matter.
  uint32_t now = NowMs();
  for (auto& cb : surf.frame_cbs) {
    cb->sendDone(now);
  }
  surf.frame_cbs.clear();
}

void NewSurface(Server& s, wl_client* client, int version, uint32_t id) {
  auto surf = std::make_unique<Surface>(client, version, id);
  Surface* sp = surf.get();
  auto* res = &surf->res;
  s.surface_by_res[res->resource()] = sp;

  res->setAttach([&s, sp](CWlSurface*, wl_resource* buffer, int32_t, int32_t) {
    sp->pending_buffer = buffer;
    sp->buffer_attached = true;
  });
  res->setFrame([&s, sp](CWlSurface* r, uint32_t cb_id) {
    sp->frame_cbs.push_back(std::make_unique<CWlCallback>(r->client(), 1, cb_id));
  });
  res->setCommit([&s, sp](CWlSurface*) { Commit(s, *sp); });
  res->setOnDestroy([&s, sp](CWlSurface* r) {
    if (sp->toplevel && sp->toplevel->xdg) sp->toplevel->xdg->surface = nullptr;
    s.surface_by_res.erase(r->resource());
    EraseOwned(s.surfaces, sp);
  });

  s.surfaces.push_back(std::move(surf));
}

// ---------------------------------------------------------------- xdg-shell --

void NewToplevel(Server& s, XdgSurf& xs, uint32_t id) {
  auto top = std::make_unique<Toplevel>(xs.res.client(), xs.res.version(), id);
  Toplevel* tp = top.get();
  top->xdg = &xs;
  xs.toplevel = tp;
  if (xs.surface) xs.surface->toplevel = tp;
  uid_t uid;
  gid_t gid;
  wl_client_get_credentials(xs.res.client(), &tp->pid, &uid, &gid);
  s.toplevel_by_res[top->res.resource()] = tp;

  auto* res = &top->res;
  res->setSetTitle([&s, tp](CXdgToplevel*, const char* title) {
    tp->title = title;
    if (auto win_obj = LockWindow(*tp)) {
      auto& win = static_cast<WaylandWindow&>(*win_obj);
      {
        auto lock = std::lock_guard(win.mutex);
        win.title = title;
      }
      win.WakeToys();
    }
  });
  res->setSetAppId([&s, tp](CXdgToplevel*, const char* app_id) {
    tp->app_id = app_id;
    if (auto win_obj = LockWindow(*tp)) {
      auto& win = static_cast<WaylandWindow&>(*win_obj);
      auto lock = std::lock_guard(win.mutex);
      win.app_id = app_id;
    }
  });
  res->setOnDestroy([&s, tp](CXdgToplevel* r) {
    UnmapToplevel(s, *tp);
    if (tp->close_timer) wl_event_source_remove(tp->close_timer);
    s.toplevel_by_res.erase(r->resource());
    if (tp->xdg) tp->xdg->toplevel = nullptr;
    if (tp->xdg && tp->xdg->surface) tp->xdg->surface->toplevel = nullptr;
    EraseOwned(s.toplevels, tp);
  });

  s.toplevels.push_back(std::move(top));
}

void NewXdgSurface(Server& s, wl_client* client, int version, uint32_t id,
                   wl_resource* surface_res) {
  auto xs = std::make_unique<XdgSurf>(client, version, id);
  XdgSurf* xp = xs.get();
  auto it = s.surface_by_res.find(surface_res);
  xs->surface = it == s.surface_by_res.end() ? nullptr : it->second;

  auto* res = &xs->res;
  res->setGetToplevel([&s, xp](CXdgSurface*, uint32_t id) { NewToplevel(s, *xp, id); });
  res->setAckConfigure([](CXdgSurface*, uint32_t) {});
  res->setOnDestroy([&s, xp](CXdgSurface*) {
    if (xp->toplevel) xp->toplevel->xdg = nullptr;
    EraseOwned(s.xdg_surfaces, xp);
  });

  s.xdg_surfaces.push_back(std::move(xs));
}

// ------------------------------------------------------------------ globals --

void BindCompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Server*)data;
  auto res = std::make_unique<CWlCompositor>(client, version, id);
  CWlCompositor* rp = res.get();
  rp->setCreateSurface([&s](CWlCompositor* r, uint32_t id) {
    NewSurface(s, r->client(), r->version(), id);
  });
  rp->setCreateRegion([&s](CWlCompositor* r, uint32_t id) {
    auto region = std::make_unique<CWlRegion>(r->client(), r->version(), id);
    CWlRegion* gp = region.get();
    gp->setOnDestroy([&s, gp](CWlRegion*) { EraseOwned(s.regions, gp); });
    s.regions.push_back(std::move(region));
  });
  rp->setOnDestroy([&s, rp](CWlCompositor*) { EraseOwned(s.compositor_resources, rp); });
  s.compositor_resources.push_back(std::move(res));
}

void BindSubcompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Server*)data;
  auto res = std::make_unique<CWlSubcompositor>(client, version, id);
  CWlSubcompositor* rp = res.get();
  rp->setGetSubsurface(
      [&s](CWlSubcompositor* r, uint32_t id, wl_resource* surface, wl_resource* parent) {
        // Accepted so clients can use them; their content is not composited yet.
        auto sub = std::make_unique<CWlSubsurface>(r->client(), r->version(), id);
        CWlSubsurface* sp = sub.get();
        sp->setOnDestroy([&s, sp](CWlSubsurface*) { EraseOwned(s.subsurfaces, sp); });
        s.subsurfaces.push_back(std::move(sub));
      });
  rp->setOnDestroy([&s, rp](CWlSubcompositor*) { EraseOwned(s.subcompositors, rp); });
  s.subcompositors.push_back(std::move(res));
}

void BindDataDeviceManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Server*)data;
  auto res = std::make_unique<CWlDataDeviceManager>(client, version, id);
  CWlDataDeviceManager* rp = res.get();
  // Clipboard plumbing comes later; the objects exist so clients are happy.
  rp->setCreateDataSource([&s](CWlDataDeviceManager* r, uint32_t id) {
    auto src = std::make_unique<CWlDataSource>(r->client(), r->version(), id);
    CWlDataSource* dp = src.get();
    dp->setOnDestroy([&s, dp](CWlDataSource*) { EraseOwned(s.data_sources, dp); });
    s.data_sources.push_back(std::move(src));
  });
  rp->setGetDataDevice([&s](CWlDataDeviceManager* r, uint32_t id, wl_resource*) {
    auto dev = std::make_unique<CWlDataDevice>(r->client(), r->version(), id);
    CWlDataDevice* dp = dev.get();
    dp->setOnDestroy([&s, dp](CWlDataDevice*) { EraseOwned(s.data_devices, dp); });
    s.data_devices.push_back(std::move(dev));
  });
  rp->setOnDestroy([&s, rp](CWlDataDeviceManager*) { EraseOwned(s.data_device_managers, rp); });
  s.data_device_managers.push_back(std::move(res));
}

void BindOutput(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Server*)data;
  auto res = std::make_unique<CWlOutput>(client, version, id);
  CWlOutput* rp = res.get();
  rp->setOnDestroy([&s, rp](CWlOutput*) { EraseOwned(s.outputs, rp); });
  rp->sendGeometry(0, 0, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Automat", "Board",
                   WL_OUTPUT_TRANSFORM_NORMAL);
  rp->sendMode((wl_output_mode)(WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED), 1920, 1080,
               60000);
  if (version >= 4) rp->sendName("AUTOMAT-1");
  if (version >= 2) {
    rp->sendScale(1);
    rp->sendDone();
  }
  s.outputs.push_back(std::move(res));
}

// Builds the xkb keymap once, serialized into a memfd that every
// wl_keyboard.keymap event shares. Keycodes pass through Automat verbatim
// (AnsiKey round-trips through the fixed x11 tables), so clients must see the
// HOST's real keymap - the X server's - or every non-qwerty layout garbles.
void InitKeymap(Server& s) {
  xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  xkb_keymap* keymap = nullptr;
  if (xcb::connection) {
    xkb_x11_setup_xkb_extension(xcb::connection, XKB_X11_MIN_MAJOR_XKB_VERSION,
                                XKB_X11_MIN_MINOR_XKB_VERSION,
                                XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr,
                                nullptr);
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
  auto& s = *(Server*)data;
  auto res = std::make_unique<CWlSeat>(client, version, id);
  CWlSeat* rp = res.get();
  rp->setGetPointer([&s](CWlSeat* r, uint32_t id) {
    auto ptr = std::make_unique<CWlPointer>(r->client(), r->version(), id);
    CWlPointer* pp = ptr.get();
    pp->setOnDestroy([&s, pp](CWlPointer*) { EraseOwned(s.pointers, pp); });
    s.pointers.push_back(std::move(ptr));
  });
  rp->setGetKeyboard([&s](CWlSeat* r, uint32_t id) {
    auto kbd = std::make_unique<CWlKeyboard>(r->client(), r->version(), id);
    CWlKeyboard* kp = kbd.get();
    kp->setOnDestroy([&s, kp](CWlKeyboard*) { EraseOwned(s.keyboards, kp); });
    if (s.keymap_fd >= 0) {
      kp->sendKeymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, s.keymap_fd, s.keymap_size);
    }
    if (kp->version() >= 4) kp->sendRepeatInfo(30, 500);
    s.keyboards.push_back(std::move(kbd));
  });
  rp->setOnDestroy([&s, rp](CWlSeat*) { EraseOwned(s.seats, rp); });
  rp->sendCapabilities(
      (wl_seat_capability)(WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD));
  if (version >= 2) rp->sendName("automat");
  s.seats.push_back(std::move(res));
}

void BindDecorationManager(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Server*)data;
  auto res = std::make_unique<CZxdgDecorationManagerV1>(client, version, id);
  CZxdgDecorationManagerV1* rp = res.get();
  rp->setGetToplevelDecoration(
      [&s](CZxdgDecorationManagerV1* r, uint32_t id, wl_resource* toplevel_res) {
        auto deco = std::make_unique<CZxdgToplevelDecorationV1>(r->client(), r->version(), id);
        CZxdgToplevelDecorationV1* dp = deco.get();
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
        s.decorations.push_back(std::move(deco));
      });
  rp->setOnDestroy([&s, rp](CZxdgDecorationManagerV1*) { EraseOwned(s.decoration_managers, rp); });
  s.decoration_managers.push_back(std::move(res));
}

void BindXdgWmBase(wl_client* client, void* data, uint32_t version, uint32_t id) {
  auto& s = *(Server*)data;
  auto res = std::make_unique<CXdgWmBase>(client, version, id);
  CXdgWmBase* rp = res.get();
  rp->setGetXdgSurface([&s](CXdgWmBase* r, uint32_t id, wl_resource* surface) {
    NewXdgSurface(s, r->client(), r->version(), id, surface);
  });
  rp->setCreatePositioner([&s](CXdgWmBase* r, uint32_t id) {
    auto pos = std::make_unique<CXdgPositioner>(r->client(), r->version(), id);
    CXdgPositioner* pp = pos.get();
    pp->setOnDestroy([&s, pp](CXdgPositioner*) { EraseOwned(s.positioners, pp); });
    s.positioners.push_back(std::move(pos));
  });
  rp->setPong([](CXdgWmBase*, uint32_t) {});
  rp->setOnDestroy([&s, rp](CXdgWmBase*) { EraseOwned(s.wm_bases, rp); });
  s.wm_bases.push_back(std::move(res));
}

int PostFdDispatch(int fd, uint32_t mask, void* data) {
  auto& s = *(Server*)data;
  uint64_t value;
  (void)!read(fd, &value, sizeof(value));
  std::vector<std::function<void()>> fns;
  {
    auto lock = std::lock_guard(s.post_mutex);
    fns.swap(s.posted);
  }
  for (auto& fn : fns) fn();
  return 0;
}

void WaylandThread(Server* s) {
  SetThreadName("Wayland");
  while (!s->quit) {
    wl_display_flush_clients(s->display);
    wl_event_loop_dispatch(s->loop, -1);
  }
  wl_display_flush_clients(s->display);
}

Toplevel* FindToplevel(Server& s, void* handle) {
  for (auto& t : s.toplevels) {
    if (t.get() == handle) return t.get();
  }
  return nullptr;
}

// Runs `fn(server, toplevel, surface_resource)` on the wayland thread if the
// window's toplevel is still alive.
template <typename Fn>
void PostInput(WaylandWindow& w, Fn&& fn) {
  auto* s = g_server;
  if (!s) return;
  void* handle = w.toplevel_handle.load();
  if (!handle) return;
  s->Post([s, handle, fn = std::forward<Fn>(fn)] {
    auto* t = FindToplevel(*s, handle);
    if (!t || !t->xdg || !t->xdg->surface) return;
    fn(*s, *t, t->xdg->surface->res.resource());
  });
}

}  // namespace

void Start() {
  if (g_server) return;
  if (!getenv("XDG_RUNTIME_DIR")) {
    ERROR << "Wayland compositor disabled: XDG_RUNTIME_DIR is not set.";
    return;
  }
  auto* s = new Server();
  s->display = wl_display_create();
  const char* socket = wl_display_add_socket_auto(s->display);
  if (!socket) {
    ERROR << "Wayland compositor disabled: couldn't open a socket.";
    wl_display_destroy(s->display);
    delete s;
    return;
  }
  s->socket_name = socket;
  s->loop = wl_display_get_event_loop(s->display);
  wl_display_init_shm(s->display);
  wl_global_create(s->display, &wl_compositor_interface, 6, s, BindCompositor);
  wl_global_create(s->display, &wl_subcompositor_interface, 1, s, BindSubcompositor);
  wl_global_create(s->display, &wl_output_interface, 4, s, BindOutput);
  wl_global_create(s->display, &wl_seat_interface, 5, s, BindSeat);
  wl_global_create(s->display, &wl_data_device_manager_interface, 3, s, BindDataDeviceManager);
  wl_global_create(s->display, &xdg_wm_base_interface, 6, s, BindXdgWmBase);
  wl_global_create(s->display, &zxdg_decoration_manager_v1_interface, 1, s,
                   BindDecorationManager);
  InitKeymap(*s);
  s->post_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  wl_event_loop_add_fd(s->loop, s->post_fd, WL_EVENT_READABLE, PostFdDispatch, s);
  s->thread = std::thread(WaylandThread, s);
  g_server = s;
  // Frames may have already run and gone idle before the server existed; wake
  // the UI so UIFrame sees it (deserialized windows wait there to respawn).
  vm.WakeToys();
  LOG << "Wayland compositor listening on " << s->socket_name;
}

void Stop() {
  if (!g_server) return;
  auto* s = g_server;
  g_server = nullptr;
  s->Post([s] { s->quit = true; });
  s->thread.join();
  wl_display_destroy_clients(s->display);
  wl_display_destroy(s->display);
  close(s->post_fd);
  if (s->keymap_fd >= 0) close(s->keymap_fd);
  // The Server allocation is deliberately leaked: the render thread may race
  // one last UIFrame/input call against this shutdown, and a dangling
  // g_server snapshot must still point at valid memory. Stop only runs once,
  // at process exit.
}

bool Running() { return g_server != nullptr; }

Str SocketName() { return g_server ? g_server->socket_name : Str{}; }

void UIFrame() {
  auto* s = g_server;
  if (!s) return;
  std::vector<Ptr<Object>> appeared, disappeared;
  {
    auto lock = std::lock_guard(s->ui_mutex);
    appeared.swap(s->ui_appeared);
    disappeared.swap(s->ui_disappeared);
  }
  static int spawn_count = 0;
  for (auto& w : appeared) {
    auto& win = static_cast<WaylandWindow&>(*w);
    // Which Command's child is this client? Its argv becomes the window's
    // respawn recipe, its plate anchors where the window is seated, and the
    // window keeps a Launcher link to it (visible cable, serialized).
    Location* command_location = nullptr;
    library::Command* command = nullptr;
    int win_w, win_h;
    {
      I64 cpid;
      {
        auto lock = std::lock_guard(win.mutex);
        cpid = win.client_pid;
        win_w = win.width;
        win_h = win.height;
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
      Vec2 size = library::WindowBoardSize(win_w, win_h);
      loc.position = command_location->position +
                     Vec2(plate.x / 2 + 0.008f + size.x / 2 + 0.006f * (n % 3),
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
      auto lock = std::lock_guard(s->adoption_mutex);
      s->adoptions.emplace_back((pid_t)pid, win->AcquireWeakPtr());
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

void NotifyWindowDestroyed(void* handle) {
  auto* s = g_server;
  if (!s || !handle) return;
  s->Post([s, handle] {
    auto* t = FindToplevel(*s, handle);
    if (!t) return;
    t->res.sendClose();
    // A client that ignores the request gets SIGTERM shortly after.
    if (!t->close_timer) {
      t->close_timer = wl_event_loop_add_timer(
          s->loop,
          [](void* data) -> int {
            auto* t = (Toplevel*)data;
            if (t->pid > 0) kill(t->pid, SIGTERM);
            return 0;
          },
          t);
      if (t->close_timer) wl_event_source_timer_update(t->close_timer, 2000);
    }
  });
}

// ----------------------------------------------------------- input senders --

void SendPointerEnter(WaylandWindow& w, float sx, float sy) {
  PostInput(w, [sx, sy](Server& s, Toplevel&, wl_resource* surf) {
    wl_client* c = wl_resource_get_client(surf);
    for (auto& p : s.pointers) {
      if (p->client() != c) continue;
      p->sendEnterRaw(s.serial++, surf, wl_fixed_from_double(sx), wl_fixed_from_double(sy));
      if (p->version() >= 5) p->sendFrame();
    }
  });
}

void SendPointerMotion(WaylandWindow& w, float sx, float sy) {
  PostInput(w, [sx, sy](Server& s, Toplevel&, wl_resource* surf) {
    wl_client* c = wl_resource_get_client(surf);
    uint32_t time = NowMs();
    for (auto& p : s.pointers) {
      if (p->client() != c) continue;
      p->sendMotion(time, wl_fixed_from_double(sx), wl_fixed_from_double(sy));
      if (p->version() >= 5) p->sendFrame();
    }
  });
}

void SendPointerButton(WaylandWindow& w, uint32_t button, bool pressed) {
  PostInput(w, [button, pressed](Server& s, Toplevel&, wl_resource* surf) {
    wl_client* c = wl_resource_get_client(surf);
    uint32_t time = NowMs();
    for (auto& p : s.pointers) {
      if (p->client() != c) continue;
      p->sendButton(s.serial++, time, button,
                    pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
      if (p->version() >= 5) p->sendFrame();
    }
  });
}

void SendPointerLeave(WaylandWindow& w) {
  PostInput(w, [](Server& s, Toplevel&, wl_resource* surf) {
    wl_client* c = wl_resource_get_client(surf);
    for (auto& p : s.pointers) {
      if (p->client() != c) continue;
      p->sendLeaveRaw(s.serial++, surf);
      if (p->version() >= 5) p->sendFrame();
    }
  });
}

void SendKeyboardEnter(WaylandWindow& w) {
  PostInput(w, [](Server& s, Toplevel&, wl_resource* surf) {
    wl_client* c = wl_resource_get_client(surf);
    wl_array keys;
    wl_array_init(&keys);
    for (auto& k : s.keyboards) {
      if (k->client() != c) continue;
      k->sendModifiers(s.serial++, 0, 0, 0, 0);
      k->sendEnterRaw(s.serial++, surf, &keys);
    }
    wl_array_release(&keys);
  });
}

void SendKeyboardLeave(WaylandWindow& w) {
  PostInput(w, [](Server& s, Toplevel&, wl_resource* surf) {
    wl_client* c = wl_resource_get_client(surf);
    for (auto& k : s.keyboards) {
      if (k->client() != c) continue;
      k->sendLeaveRaw(s.serial++, surf);
    }
  });
}

void SendKey(WaylandWindow& w, uint32_t evdev_keycode, bool pressed, bool ctrl, bool alt,
             bool shift, bool super) {
  PostInput(w, [evdev_keycode, pressed, ctrl, alt, shift,
                super](Server& s, Toplevel&, wl_resource* surf) {
    wl_client* c = wl_resource_get_client(surf);
    uint32_t mods = (ctrl ? s.mod_ctrl : 0) | (alt ? s.mod_alt : 0) | (shift ? s.mod_shift : 0) |
                    (super ? s.mod_super : 0);
    uint32_t time = NowMs();
    for (auto& k : s.keyboards) {
      if (k->client() != c) continue;
      k->sendModifiers(s.serial++, mods, 0, 0, 0);
      k->sendKey(s.serial++, time, evdev_keycode,
                 pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    }
  });
}

}  // namespace automat::wayland

#endif  // __linux__
