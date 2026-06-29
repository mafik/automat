// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "wayland.hpp"

#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/pathops/SkPathOps.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

#include "dmabuf.hpp"
#include "format.hpp"
#include "library_command.hpp"
#include "library_wayland_window.hpp"
#include "location.hpp"
#include "log.hpp"
#include "vk.hpp"
#include "vm.hpp"
#include "xcb.hpp"

namespace automat::wayland {

Colony<Client> Client::colony;

Common::Common(Kind kind, U32 id, Client& client) : kind(kind), id(id), client(client) {
  client.SetId(id, this);
}

Common::~Common() {
  client.SetId(id, nullptr);
  if (id < 0xff000000)
    if (Display* display = static_cast<Display*>(client.GetId(1))) display->DeleteId(id);
}

void Display::OnSync(Callback& callback) {
  callback.Done(0);
  callback.ColonyDestroy();
}

void Display::OnGetRegistry(Registry& registry) {
  registry.Global(1, "wl_compositor", 6);
  registry.Global(2, "wl_subcompositor", 1);
  registry.Global(3, "wl_shm", 1);
  registry.Global(4, "wl_seat", 7);
  registry.Global(5, "wl_output", 4);
  registry.Global(6, "wl_data_device_manager", 3);
  registry.Global(7, "xdg_wm_base", 6);
  registry.Global(8, "wp_viewporter", 1);
  registry.Global(9, "zxdg_decoration_manager_v1", 2);
  registry.Global(10, "wp_cursor_shape_manager_v1", 1);
  registry.Global(11, "zwp_linux_dmabuf_v1", 4);
}

void Registry::OnBind(U32, StrView id_interface, U32 id_version, U32 id) {
  if (id_interface == "wl_compositor") {
    Compositor::ColonyMake(id, client);
  } else if (id_interface == "wl_subcompositor") {
    Subcompositor::ColonyMake(id, client);
  } else if (id_interface == "wl_shm") {
    Shm& shm = Shm::ColonyMake(id, client);
    shm.Format(Shm::FormatArgb8888);
    shm.Format(Shm::FormatXrgb8888);
  } else if (id_interface == "wl_seat") {
    Seat& seat = Seat::ColonyMake(id, client);
    seat.version = id_version;
    seat.Capabilities(
        static_cast<Seat::Capability>(Seat::CapabilityPointer | Seat::CapabilityKeyboard));
    if (id_version >= 2) seat.Name("seat0");
  } else if (id_interface == "wl_output") {
    Output& output = Output::ColonyMake(id, client);
    output.Geometry(0, 0, 300, 190, static_cast<Output::Subpixel>(0), "Automat", "Dummy",
                    static_cast<Output::Transform>(0));
    output.Mode(static_cast<enum Output::Mode>(Output::ModeCurrent | Output::ModePreferred), 1920,
                1080, 60000);
    if (id_version >= 4) output.Name("AUTOMAT-1");
    if (id_version >= 2) {
      output.Scale(1);
      output.Done();
    }
  } else if (id_interface == "wl_data_device_manager") {
    DataDeviceManager::ColonyMake(id, client);
  } else if (id_interface == "xdg_wm_base") {
    XdgWmBase::ColonyMake(id, client).version = id_version;
  } else if (id_interface == "wp_viewporter") {
    Viewporter::ColonyMake(id, client);
  } else if (id_interface == "zxdg_decoration_manager_v1") {
    ZxdgDecorationManagerV1::ColonyMake(id, client);
  } else if (id_interface == "wp_cursor_shape_manager_v1") {
    CursorShapeManagerV1::ColonyMake(id, client);
  } else if (id_interface == "zwp_linux_dmabuf_v1") {
    AdvertiseDmabufOnBind(LinuxDmabufV1::ColonyMake(id, client), id_version);
  }
}

void Client::ProtocolError(U32 object_id, U32 code, StrView message) {
  if (errored) return;
  errored = true;
  static_cast<Display*>(GetId(1))->Error(object_id, code, message);
}

void Client::Disconnect() {
  Status status;
  server.epoll.Del(this, status);
  server.epoll.Post([this] { Client::colony.erase(Client::colony.get_iterator(this)); });
}

void Client::NotifyRead(Status&) {
  for (;;) {
    char buf[4096];
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    iovec iov{buf, sizeof(buf)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(fd.fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n > 0) {
      in.append(buf, n);
      for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
          int count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
          int fds[16];
          std::memcpy(fds, CMSG_DATA(cm), sizeof(int) * count);
          for (int k = 0; k < count; ++k) recv_fds.push_back(fds[k]);
        }
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      Disconnect();
      return;
    }
    break;
  }
  size_t offset = 0;
  while (in.size() - offset >= 8) {
    U32 id, word;
    std::memcpy(&id, in.data() + offset, 4);
    std::memcpy(&word, in.data() + offset + 4, 4);
    U32 size = word >> 16, opcode = word & 0xffff;
    if (size < 8 || in.size() - offset < size) break;
    if (Common* o = GetId(id))
      o->GenericDispatch(opcode, in.data() + offset + 8, in.data() + offset + size);
    offset += size;
    if (errored) break;
  }
  in.erase(0, offset);
  server.FlushAll();
  if (errored) Disconnect();
}
StrView Client::Name() const { return "WaylandClient"sv; }

void Server::NotifyRead(Status&) {
  for (;;) {
    int client_fd = accept4(fd.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    Client& client = *Client::colony.emplace(*this);
    client.fd = FD(client_fd);
    Display::ColonyMake(1, client);
    Status status;
    epoll.Add(&client, status);
  }
}
StrView Server::Name() const { return "WaylandServer"sv; }

void Server::FlushAll() {
  for (Client& client : Client::colony) {
    if (client.out.empty()) {
      client.out_fds.clear();
      continue;
    }
    msghdr msg{};
    iovec iov{client.out.data(), client.out.size()};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    if (!client.out_fds.empty()) {
      int count = std::min<int>(client.out_fds.size(), 16);
      msg.msg_control = control;
      msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
      cmsghdr* cm = CMSG_FIRSTHDR(&msg);
      cm->cmsg_level = SOL_SOCKET;
      cm->cmsg_type = SCM_RIGHTS;
      cm->cmsg_len = CMSG_LEN(sizeof(int) * count);
      int fds[16];
      for (int k = 0; k < count; ++k) fds[k] = client.out_fds[k].fd;
      std::memcpy(CMSG_DATA(cm), fds, sizeof(int) * count);
    }
    ssize_t n = sendmsg(client.fd.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) client.out.erase(0, n);
    client.out_fds.clear();
  }
}

std::unique_ptr<Server> MakeServer(mux::Epoll& epoll, Status& status) {
  const char* runtime = getenv("XDG_RUNTIME_DIR");
  if (!runtime) {
    AppendErrorMessage(status) += "XDG_RUNTIME_DIR is not set";
    return nullptr;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    AppendErrorMessage(status) += f("socket(): {}", strerror(errno));
    return nullptr;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  Str name, sock_path;
  FD lock_fd;
  for (int n = 0; n <= 32 && name.empty(); ++n) {
    Str candidate = f("wayland-{}", n);
    Str path = f("{}/{}", runtime, candidate);
    if (path.size() + 1 > sizeof(addr.sun_path)) continue;
    int lf = open(f("{}.lock", path).c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0660);
    if (lf < 0) continue;
    if (flock(lf, LOCK_EX | LOCK_NB) != 0) {
      close(lf);
      continue;
    }
    unlink(path.c_str());
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      name = candidate;
      sock_path = path;
      lock_fd = FD(lf);
    } else {
      close(lf);
    }
  }
  if (name.empty()) {
    close(fd);
    AppendErrorMessage(status) += "no free wayland-N socket in XDG_RUNTIME_DIR";
    return nullptr;
  }
  if (listen(fd, 16) < 0) {
    close(fd);
    AppendErrorMessage(status) += f("listen(): {}", strerror(errno));
    return nullptr;
  }
  setenv("WAYLAND_DISPLAY", name.c_str(), 1);
  LOG << f("Wayland server listening on {} (WAYLAND_DISPLAY={})", addr.sun_path, name);

  auto srv = std::make_unique<Server>(epoll);
  srv->fd = FD(fd);
  srv->socket_path = sock_path;
  srv->lock_fd = std::move(lock_fd);
  epoll.Post([s = srv.get()] {
    Status status;
    s->epoll.Add(s, status);
    if (!OK(status)) ERROR << "wayland: failed to watch the listening socket: " << status.ToStr();
  });
  return srv;
}

std::unique_ptr<Server> server;

Server::~Server() {
  if (socket_path) {
    Status ignore;
    socket_path.Unlink(ignore, true);
    socket_path.WithExt("lock").Unlink(ignore, true);
  }
}

bool Server::Running() { return true; }

using library::WaylandSurface;
using library::WaylandWindow;

// Walks up to the toplevel owning this surface's window, following subsurface- and popup-parent
// links, or null if the surface is not part of a toplevel tree.
XdgToplevel* Surface::OwningToplevel() {
  for (Surface* p = this; p;) {
    if (p->xdg && p->xdg->toplevel) return p->xdg->toplevel;
    if (p->xdg && p->xdg->popup && p->xdg->popup->parent) {
      p = p->xdg->popup->parent->surface;
      continue;
    }
    p = p->as_subsurface ? p->as_subsurface->parent : nullptr;
  }
  return nullptr;
}

namespace {

WaylandSurface* GetOrCreateObject(Surface& surf) {
  if (surf.object == nullptr) {
    surf.object = MAKE_PTR(WaylandSurface);
  }
  return surf.object.Get<WaylandSurface>();
}

// Copies a surface's committed texture and input region into its board object. Subsurface and
// popup children are added in later stages.
void UpdateSurfaceNode(WaylandSurface& object, Surface& surf) {
  Vec<WaylandSurface::Child> below, above;
  bool seen_self = false;
  for (Surface* entry : surf.stack) {
    if (entry == &surf) {
      seen_self = true;
      continue;
    }
    WaylandSurface* child = GetOrCreateObject(*entry);
    UpdateSurfaceNode(*child, *entry);
    (seen_self ? above : below).push_back({child->AcquirePtr(), entry->as_subsurface->pos});
  }
  if (surf.xdg) {
    for (XdgPopup* pp : surf.xdg->child_popups) {
      if (!pp->xdg || !pp->xdg->surface) continue;
      WaylandSurface* child = GetOrCreateObject(*pp->xdg->surface);
      UpdateSurfaceNode(*child, *pp->xdg->surface);
      SkIPoint base = surf.xdg->geo.topLeft();
      above.push_back({child->AcquirePtr(), base + pp->geo.topLeft(), true, base + pp->flipped,
                       pp->flip_x, pp->flip_y, pp->slide_x, pp->slide_y});
    }
  }
  {
    auto lock = std::lock_guard(object.mutex);
    object.image = surf.content.image;
    object.src_crop = surf.content.src_crop;
    object.dst_size = surf.content.dst_size;
    object.input_region = surf.input_region.infinite
                              ? SkPath::Rect(SkRect::MakeIWH(surf.content.dst_size.width(),
                                                             surf.content.dst_size.height()))
                              : surf.input_region.path;
    object.below = std::move(below);
    object.above = std::move(above);
    object.WakeToys();
  }
  object.surface_handle = &surf;
}

void UnmapToplevel(XdgToplevel& t) {
  if (!t.mapped) return;
  t.mapped = false;
  Server& s = t.client.server;
  if (Ptr<ReferenceCounted> win_obj = t.window.Lock()) {
    auto win = win_obj.Cast<WaylandWindow>();
    win->toplevel_handle.store(nullptr);
    {
      auto lock = std::lock_guard(win->mutex);
      win->client_gone = true;
    }
    win->WakeToys();
    {
      auto lock = std::lock_guard(s.ui_mutex);
      s.ui_disappeared.push_back(win);
    }
    vm.WakeToys();
  }
  t.window = {};
}

void UpdateDecoration(XdgToplevel&);  // defined below

void ApplyAndPublish(Surface& surf) {
  XdgToplevel* owner = surf.OwningToplevel();
  if (!owner) return;
  XdgToplevel& t = *owner;
  Server& s = t.client.server;

  bool adopted = false;
  Ptr<ReferenceCounted> win_obj;
  if (surf.xdg && surf.xdg->toplevel == owner) {
    if (Ptr<ReferenceCounted> win = t.window.Lock()) {
      win_obj = win;
    } else if (!t.mapped) {
      Ptr<WaylandWindow> win;
      {
        auto lock = std::lock_guard(s.adoption_mutex);
        for (auto it = s.adoptions.begin(); it != s.adoptions.end(); ++it) {
          if (it->first == t.pid) {
            if (Ptr<ReferenceCounted> o = it->second.Lock()) win = o.Cast<WaylandWindow>();
            s.adoptions.erase(it);
            break;
          }
        }
      }
      if (win) {
        adopted = true;
        win->toplevel_handle.store(&t);
        auto lock = std::lock_guard(win->mutex);
        win->client_gone = false;
        win->client_pid = t.pid;
        if (!t.title.empty()) win->title = t.title;
        win->app_id = t.app_id;
      } else {
        win = MAKE_PTR(WaylandWindow);
        win->toplevel_handle.store(&t);
        auto lock = std::lock_guard(win->mutex);
        win->title = t.title;
        win->app_id = t.app_id;
        win->client_pid = t.pid;
      }
      t.window = win->AcquireWeakPtr();
      // Now that the window is linked, settle the decoration mode for its
      // (possibly restored) preference: the get_toplevel_decoration / set_mode
      // handshake ran earlier, when the preference still read as the default.
      UpdateDecoration(t);
      win_obj = win;
    }
  } else {
    win_obj = t.window.Lock();
  }
  if (!win_obj) return;

  // Mirror the toplevel's surface tree into the window object.
  WaylandWindow& wwin = static_cast<WaylandWindow&>(*win_obj);
  if (t.xdg && t.xdg->surface) {
    UpdateSurfaceNode(wwin, *t.xdg->surface);
  } else {
    auto lock = std::lock_guard(wwin.mutex);
    wwin.image = nullptr;
    wwin.dst_size = {};
    wwin.below.clear();
    wwin.above.clear();
    wwin.WakeToys();
  }

  // Wake the toy; on the first frame insert the window onto the board (unless it was adopted, in
  // which case its Location already exists).
  static_cast<Object*>(win_obj.Get())->WakeToys();
  vm.WakeToys();
  if (!t.mapped) {
    t.mapped = true;
    if (!adopted) {
      auto lock = std::lock_guard(s.ui_mutex);
      s.ui_appeared.push_back(std::move(win_obj));
    }
  }
}

// Derives the displayed surface size and the buffer rectangle to sample from the wp_viewport (if
// any) and the buffer. Posts the two commit-time viewport errors and returns false on them.
bool ResolveGeometry(Surface& surf, SkISize buf, CutoutGeometry& out) {
  Viewport* vp = surf.viewport;
  bool src_set = vp && vp->src_w >= 0;
  bool dst_set = vp && vp->dst_size.width() >= 0;
  float sx = src_set ? vp->src_x : 0;
  float sy = src_set ? vp->src_y : 0;
  float sw = src_set ? vp->src_w : (float)buf.width();
  float sh = src_set ? vp->src_h : (float)buf.height();
  if (src_set) {
    if (!dst_set && (sw != std::floor(sw) || sh != std::floor(sh))) {
      surf.client.ProtocolError(vp->id, Viewport::ErrorBadSize,
                                "non-integer crop without a destination size");
      return false;
    }
    if (sx + sw > buf.width() || sy + sh > buf.height() || sx < 0 || sy < 0) {
      surf.client.ProtocolError(vp->id, Viewport::ErrorOutOfBuffer,
                                "source rectangle reaches outside the buffer");
      return false;
    }
  }
  out.src_crop.setXYWH(sx, sy, sw, sh);
  if (dst_set) {
    out.dst_size = vp->dst_size;
  } else {
    int scale = surf.buffer_scale > 0 ? surf.buffer_scale : 1;
    int dw = (int)sw / scale, dh = (int)sh / scale;
    bool swap = surf.buffer_transform == 1 || surf.buffer_transform == 3 ||
                surf.buffer_transform == 5 || surf.buffer_transform == 7;  // 90/270 rotations
    out.dst_size = {swap ? dh : dw, swap ? dw : dh};
  }
  return true;
}

// The format/modifier pairs the compositor accepts. AR24/XR24 are the 32-bit BGRA formats every
// toolkit produces; both import as VK_FORMAT_B8G8R8A8_UNORM. LINEAR is what the all-software Mesa
// stack allocates here; INVALID lets a client send an implicitly-allocated buffer.
struct DmabufFormatModifier {
  U32 format;
  U64 modifier;
};
constexpr DmabufFormatModifier kDmabufFormats[] = {
    {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR},
    {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR},
    {DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID},
    {DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID},
};
constexpr int kDmabufFormatCount = (int)(sizeof(kDmabufFormats) / sizeof(kDmabufFormats[0]));

// Tells Mesa which DRM device to allocate on; without it clients fall back to wl_shm.
void SendDmabufFeedback(Server& s, LinuxDmabufFeedbackV1& fb) {
  // Build the version-4 feedback inputs once: the render node clients allocate on (its dev_t
  // becomes main_device) and a sealed format table the feedback events index into.
  if (!s.dmabuf_inited) {
    s.dmabuf_inited = true;
    struct stat st;
    if (stat("/dev/dri/renderD128", &st) == 0) {
      s.dmabuf_main_device = st.st_rdev;
      s.dmabuf_has_device = true;
    } else {
      ERROR << "Wayland dmabuf: can't stat /dev/dri/renderD128; clients fall back to shm.";
    }
    struct TableEntry {
      U32 format;
      U32 pad;
      U64 modifier;
    };
    static_assert(sizeof(TableEntry) == 16, "dmabuf format table entry must be 16 bytes");
    TableEntry table[kDmabufFormatCount];
    for (int i = 0; i < kDmabufFormatCount; ++i)
      table[i] = {kDmabufFormats[i].format, 0, kDmabufFormats[i].modifier};
    int fd = memfd_create("automat-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
      ERROR << "Wayland dmabuf: memfd_create failed; feedback disabled.";
    } else if (write(fd, table, sizeof(table)) != (ssize_t)sizeof(table)) {
      close(fd);
    } else {
      fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
      s.dmabuf_format_table_fd = FD(fd);
      s.dmabuf_format_table_size = (U32)sizeof(table);
    }
  }
  if (s.dmabuf_format_table_fd.fd >= 0)
    fb.FormatTable(FD(dup(s.dmabuf_format_table_fd.fd)), s.dmabuf_format_table_size);
  Span<> device =
      s.dmabuf_has_device ? Span<>((char*)&s.dmabuf_main_device, sizeof(dev_t)) : Span<>();
  fb.MainDevice(device);
  fb.TrancheTargetDevice(device);
  fb.TrancheFlags((enum LinuxDmabufFeedbackV1::TrancheFlags)0);
  uint16_t indices[kDmabufFormatCount];
  for (int i = 0; i < kDmabufFormatCount; ++i) indices[i] = (uint16_t)i;
  fb.TrancheFormats(Span<>((char*)indices, sizeof(indices)));
  fb.TrancheDone();
  fb.Done();
}

// Releases the dmabuf buffer a surface holds for zero-copy display, if any.
void ReleaseHeldDmabuf(Surface& surf) {
  if (Buffer* b = surf.held_dmabuf) b->Release();
  surf.held_dmabuf = nullptr;
}

// Moves a finished params' planes and format into a new dmabuf wl_buffer.
void FillDmabufBuffer(LinuxBufferParamsV1& p, Buffer& buf, I32 width, I32 height, U32 format,
                      bool y_invert) {
  buf.width = width;
  buf.height = height;
  buf.drm_format = format;
  buf.dmabuf_modifier = p.modifier;
  buf.y_invert = y_invert;
  buf.dmabuf_plane_count = p.plane_count;
  for (int i = 0; i < p.plane_count; ++i) buf.dmabuf_planes[i] = std::move(p.planes[i]);
}

// At a parent surface's commit, swap its pending child stack in and apply each subsurface's pending
// position and (in sync mode) cached frame, recursing into sync children. Returns whether anything
// visible changed.
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

// place_above/place_below reorders a subsurface in its parent's pending stack relative to `ref`
// (a sibling's surface or the parent itself); becomes visible at the parent's next commit.
void RestackSubsurface(Subsurface& sub, Surface* ref, bool above) {
  Surface* parent = sub.parent;
  Surface* child = sub.surface;
  if (!parent || !child) return;
  auto& stack = parent->pending_stack;
  std::erase(stack, child);
  auto it = std::find(stack.begin(), stack.end(), ref);
  if (it == stack.end())
    stack.push_back(child);
  else
    stack.insert(it + (above ? 1 : 0), child);
}

// Removes a subsurface from its parent's tree and orphans it. Idempotent.
void DetachSubsurface(Subsurface& sub) {
  Surface* parent = sub.parent;
  if (parent && sub.surface) {
    std::erase(parent->stack, sub.surface);
    std::erase(parent->pending_stack, sub.surface);
  }
  if (sub.surface) sub.surface->as_subsurface = nullptr;
  sub.surface = nullptr;
  sub.parent = nullptr;
  if (parent) ApplyAndPublish(*parent);
}

XdgToplevel* LiveToplevel(XdgToplevel* t) {
  for (XdgToplevel& x : XdgToplevel::colony)
    if (&x == t) return &x;
  return nullptr;
}

void KeyboardFocusTo(Server& s, Surface* target) {
  if (s.keyboard_surface == target) return;
  if (s.keyboard_surface) {
    Client* c = &s.keyboard_surface->client;
    for (Keyboard& k : Keyboard::colony)
      if (&k.client == c) k.Leave(s.serial++, *s.keyboard_surface);
  }
  s.keyboard_surface = target;
  if (target) {
    Client* c = &target->client;
    for (Keyboard& k : Keyboard::colony) {
      if (&k.client != c) continue;
      k.Enter(s.serial++, *target, {});
      k.Modifiers(s.serial++, 0, 0, 0, 0);
    }
  }
}

// Places the popup by its positioner's anchor, gravity and offset, and records the flipped
// alternative (mirrored about the anchor) plus the client's flip/slide permissions. The compositor
// does not constrain to screen - the toy does (it alone knows the window's on-screen position).
void ResolvePopup(XdgPositioner& p, XdgPopup& popup) {
  using A = XdgPositioner;
  auto is_left = [](U32 e) {
    return e == A::AnchorLeft || e == A::AnchorTopLeft || e == A::AnchorBottomLeft;
  };
  auto is_right = [](U32 e) {
    return e == A::AnchorRight || e == A::AnchorTopRight || e == A::AnchorBottomRight;
  };
  auto is_top = [](U32 e) {
    return e == A::AnchorTop || e == A::AnchorTopLeft || e == A::AnchorTopRight;
  };
  auto is_bottom = [](U32 e) {
    return e == A::AnchorBottom || e == A::AnchorBottomLeft || e == A::AnchorBottomRight;
  };
  auto mirror_x = [](U32 e) -> U32 {
    switch (e) {
      case A::AnchorLeft:
        return A::AnchorRight;
      case A::AnchorRight:
        return A::AnchorLeft;
      case A::AnchorTopLeft:
        return A::AnchorTopRight;
      case A::AnchorTopRight:
        return A::AnchorTopLeft;
      case A::AnchorBottomLeft:
        return A::AnchorBottomRight;
      case A::AnchorBottomRight:
        return A::AnchorBottomLeft;
      default:
        return e;
    }
  };
  auto mirror_y = [](U32 e) -> U32 {
    switch (e) {
      case A::AnchorTop:
        return A::AnchorBottom;
      case A::AnchorBottom:
        return A::AnchorTop;
      case A::AnchorTopLeft:
        return A::AnchorBottomLeft;
      case A::AnchorBottomLeft:
        return A::AnchorTopLeft;
      case A::AnchorTopRight:
        return A::AnchorBottomRight;
      case A::AnchorBottomRight:
        return A::AnchorTopRight;
      default:
        return e;
    }
  };
  auto place_x = [&](U32 anchor, U32 gravity) {
    const SkIRect& ar = p.anchor_rect;
    int w = p.size.width();
    int a = is_left(anchor) ? ar.x() : is_right(anchor) ? ar.right() : ar.x() + ar.width() / 2;
    int x = is_left(gravity) ? a - w : is_right(gravity) ? a : a - w / 2;
    return x + p.offset.x();
  };
  auto place_y = [&](U32 anchor, U32 gravity) {
    const SkIRect& ar = p.anchor_rect;
    int h = p.size.height();
    int a = is_top(anchor) ? ar.y() : is_bottom(anchor) ? ar.bottom() : ar.y() + ar.height() / 2;
    int y = is_top(gravity) ? a - h : is_bottom(gravity) ? a : a - h / 2;
    return y + p.offset.y();
  };
  U32 ca = p.constraint_adjustment;
  popup.geo = SkIRect::MakeXYWH(place_x(p.anchor, p.gravity), place_y(p.anchor, p.gravity),
                                p.size.width(), p.size.height());
  popup.flipped = {place_x(mirror_x(p.anchor), mirror_x(p.gravity)),
                   place_y(mirror_y(p.anchor), mirror_y(p.gravity))};
  popup.flip_x = ca & XdgPositioner::ConstraintAdjustmentFlipX;
  popup.flip_y = ca & XdgPositioner::ConstraintAdjustmentFlipY;
  popup.slide_x = ca & XdgPositioner::ConstraintAdjustmentSlideX;
  popup.slide_y = ca & XdgPositioner::ConstraintAdjustmentSlideY;
}

// Offers the current selection to one data_device: a fresh server-allocated wl_data_offer, then its
// MIME list, then the selection event. With no selection, just clears that device's selection.
void OfferSelectionTo(Server& s, DataDevice& dev) {
  if (!s.selection) {
    dev.Selection(nullptr);
    return;
  }
  DataOffer& offer = DataOffer::ColonyMake(dev.client.server_id_pool.Grab(), dev.client);
  offer.source = s.selection;
  dev.DataOffer(offer);
  for (const Str& mime : s.selection->mimes) offer.Offer(mime);
  dev.Selection(&offer);
}

}  // namespace

uint32_t NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

Surface::~Surface() {
  ReleaseHeldDmabuf(*this);  // hand any zero-copy buffer back so the client may reuse it
  if (as_subsurface) DetachSubsurface(*as_subsurface);
}

Subsurface::~Subsurface() { DetachSubsurface(*this); }

XdgPopup::~XdgPopup() {
  Server& s = client.server;
  if (parent) std::erase(parent->child_popups, this);
  if (s.grabbing_popup == this) {
    Surface* target = nullptr;
    if (parent && parent->surface)
      if (XdgToplevel* t = parent->surface->OwningToplevel())
        if (t->xdg) target = t->xdg->surface;
    KeyboardFocusTo(s, target);
  }
  if (parent && parent->surface) ApplyAndPublish(*parent->surface);
}

XdgToplevel::~XdgToplevel() { UnmapToplevel(*this); }

Buffer::~Buffer() {
  if (data) munmap(data, map_size);
}

void Compositor::OnCreateSurface(Surface& id) {
  id.stack.push_back(&id);
  id.pending_stack.push_back(&id);
}

void Region::OnAdd(I32 x, I32 y, I32 width, I32 height) {
  SkPath out;
  if (Op(path, SkPath::Rect(SkRect::MakeXYWH(x, y, width, height)), kUnion_SkPathOp, &out))
    path = std::move(out);
}
void Region::OnSubtract(I32 x, I32 y, I32 width, I32 height) {
  SkPath out;
  if (Op(path, SkPath::Rect(SkRect::MakeXYWH(x, y, width, height)), kDifference_SkPathOp, &out))
    path = std::move(out);
}

void Shm::OnCreatePool(ShmPool& id, FD&& fd, I32 size) {
  if (size > 0) id.size = size;
  id.fd = std::move(fd);
}

void ShmPool::OnCreateBuffer(Buffer& id, I32 offset, I32 width, I32 height, I32 stride,
                             enum Shm::Format format) {
  if (size > 0 && fd.fd >= 0) {
    void* d = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd.fd, 0);
    if (d != MAP_FAILED) {
      id.data = d;
      id.map_size = size;
    }
  }
  id.offset = offset;
  id.width = width;
  id.height = height;
  id.stride = stride;
  id.format = (U32)format;
}

void ShmPool::OnResize(I32 new_size) {
  if (new_size > 0) size = new_size;
}

void Surface::OnAttach(Buffer* buffer, I32, I32) {
  pending_buffer = buffer;
  buffer_attached = true;
}

void Surface::OnFrame(Callback& callback) { frame_callbacks.push_back(callback.id); }

void Surface::OnSetBufferScale(I32 scale) {
  if (scale > 0) buffer_scale = scale;
}

void Surface::OnSetBufferTransform(enum Output::Transform transform) {
  buffer_transform = (int32_t)transform;
}

void Surface::OnSetInputRegion(Region* region) {
  InputRegion ir;
  if (region) {
    ir.infinite = false;
    ir.path = region->path;
  }
  pending_input_region = std::move(ir);
}

void Surface::OnCommit() {
  Surface& surf = *this;
  Server& s = surf.client.server;

  // Initial configure handshake, before the client attaches pixels: the toplevel configure first,
  // then xdg_surface.configure with a serial; nothing else may send xdg_surface.configure before
  // this bundle.
  if (XdgSurface* xp = surf.xdg; xp && !xp->initial_configure_sent) {
    if (XdgToplevel* tl = xp->toplevel) {
      // wm_capabilities (v5+) must precede the first xdg_surface.configure; an empty set means
      // none of maximize/minimize/fullscreen/window-menu (Automat draws the chrome itself).
      if (tl->version >= 5) tl->WmCapabilities({});
      tl->Configure(0, 0, {});
      xp->Configure(xp->last_configure_serial = s.serial++);
      xp->initial_configure_sent = true;
    } else if (XdgPopup* pp = xp->popup) {
      pp->Configure(pp->geo.x(), pp->geo.y(), pp->geo.width(), pp->geo.height());
      xp->Configure(xp->last_configure_serial = s.serial++);
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
    Buffer* buf = surf.pending_buffer;
    surf.pending_buffer = nullptr;
    if (!buf) {
      // Null (or freed) buffer = unmap: a toplevel removes its window.
      surf.content.image = nullptr;
      if (surf.xdg && surf.xdg->toplevel)
        UnmapToplevel(*surf.xdg->toplevel);
      else
        dirty = true;
    } else {
      SurfaceCutout content;
      bool is_dmabuf = buf->dmabuf_plane_count > 0;
      if (is_dmabuf) {
        // Import the dmabuf planes into a GPU texture, dup'ing the fds because the buffer outlives
        // this commit and may be re-imported next frame (vk::ImportDmabuf consumes the dups).
        SkISize size = {buf->width, buf->height};
        CutoutGeometry geom;
        if (!size.isEmpty() && ResolveGeometry(surf, size, geom)) {
          DmabufImage desc;
          desc.width = buf->width;
          desc.height = buf->height;
          desc.drm_format = buf->drm_format;
          desc.modifier = buf->dmabuf_modifier;
          desc.opaque = buf->drm_format == DRM_FORMAT_XRGB8888;
          desc.y_invert = buf->y_invert;
          desc.plane_count = buf->dmabuf_plane_count;
          bool fds_ok = true;
          for (int i = 0; i < buf->dmabuf_plane_count; ++i) {
            desc.fds[i] = fcntl(buf->dmabuf_planes[i].fd.fd, F_DUPFD_CLOEXEC, 0);
            desc.offsets[i] = buf->dmabuf_planes[i].offset;
            desc.strides[i] = buf->dmabuf_planes[i].stride;
            if (desc.fds[i] < 0) {
              fds_ok = false;
              break;
            }
          }
          if (fds_ok) {
            content.image = vk::ImportDmabuf(std::move(desc));
            content.src_crop = geom.src_crop;
            content.dst_size = geom.dst_size;
          }
        }
      } else {
        // Row-copy the shm buffer into a pooled allocation and wrap it as a raster image.
        int w = buf->width, h = buf->height, stride = buf->stride;
        CutoutGeometry geom;
        if (buf->data && w > 0 && h > 0 && stride >= w * 4 && buf->offset >= 0 &&
            (size_t)buf->offset + (size_t)(h - 1) * stride + (size_t)w * 4 <= buf->map_size &&
            ResolveGeometry(surf, {w, h}, geom)) {
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
          const uint8_t* src = (const uint8_t*)buf->data + buf->offset;
          uint8_t* dst = (uint8_t*)data->writable_data();
          for (int row = 0; row < h; ++row)
            memcpy(dst + (size_t)row * w * 4, src + (size_t)row * stride, (size_t)w * 4);
          auto info = SkImageInfo::Make(
              w, h, kBGRA_8888_SkColorType,
              buf->format == (U32)Shm::FormatXrgb8888 ? kOpaque_SkAlphaType : kPremul_SkAlphaType);
          content.image = SkImages::RasterFromData(info, data, (size_t)w * 4);
          content.src_crop = geom.src_crop;
          content.dst_size = geom.dst_size;
          surf.frame_pool.push_back(std::move(data));
          if (surf.frame_pool.size() > 4) surf.frame_pool.erase(surf.frame_pool.begin());
        }
      }
      if (content.image) {
        if (surf.as_subsurface && surf.as_subsurface->sync)
          surf.as_subsurface->cache = std::move(content);  // sync child: hold until parent commits
        else {
          surf.content = std::move(content);
          dirty = true;
        }
      }
      if (is_dmabuf) {
        // Zero-copy: hold this buffer until the next frame replaces it, then release the previous.
        if (surf.held_dmabuf != buf) ReleaseHeldDmabuf(surf);
        surf.held_dmabuf = buf;
      } else {
        buf->Release();           // shm: copy-release, the client may reuse it immediately
        ReleaseHeldDmabuf(surf);  // any previously held dmabuf is no longer shown
      }
    }
  }

  // This surface's commit applies its subsurfaces' positions and sync caches.
  if (ApplyChildren(surf)) dirty = true;
  if (dirty) ApplyAndPublish(surf);

  if (!surf.frame_callbacks.empty() && !s.frame_pending) {
    if (!s.frame_timer) {
      s.frame_timer = std::make_unique<mux::Timer>(s.epoll);
      // At ~60 Hz, complete every pending wl_surface.frame callback so animating clients pace
      // themselves instead of spinning.
      s.frame_timer->handler = [srv = &s] {
        uint32_t now = NowMs();
        for (Surface& surf : Surface::colony) {
          for (U32 cb_id : surf.frame_callbacks) {
            Common* cb_obj = surf.client.GetId(cb_id);
            if (cb_obj && cb_obj->kind == Callback::Kind) {
              auto& cb = static_cast<Callback&>(*cb_obj);
              cb.Done(now);
              cb.ColonyDestroy();
            }
          }
          surf.frame_callbacks.clear();
        }
        srv->frame_pending = false;
        srv->FlushAll();
      };
    }
    s.frame_pending = true;
    s.frame_timer->Arm(1.0 / 60);
  }
}

void XdgWmBase::OnGetXdgSurface(XdgSurface& id, Surface& surface) {
  id.surface = &surface;
  id.version = version;
  surface.xdg = &id;
}

void XdgSurface::OnGetToplevel(XdgToplevel& id) {
  id.xdg = this;
  id.version = version;
  toplevel = &id;
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (getsockopt(client.fd.fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) id.pid = cred.pid;
}

void XdgSurface::OnDestroy() {
  if (toplevel || popup)
    client.ProtocolError(id, XdgSurface::ErrorDefunctRoleObject,
                         "xdg_surface destroyed before its role object");
}

void XdgSurface::OnAckConfigure(U32 serial) {
  if (serial > last_configure_serial)
    client.ProtocolError(id, XdgSurface::ErrorInvalidSerial,
                         "ack of a configure that was never sent");
}

void XdgSurface::OnSetWindowGeometry(I32 x, I32 y, I32 width, I32 height) {
  geo = SkIRect::MakeXYWH(x, y, width, height);
}

void XdgToplevel::OnSetTitle(StrView title) {
  this->title = Str(title);
  if (Ptr<ReferenceCounted> win_obj = window.Lock()) {
    auto win = win_obj.Cast<WaylandWindow>();
    {
      auto lock = std::lock_guard(win->mutex);
      win->title = Str(title);
    }
    win->WakeToys();
  }
}

void XdgToplevel::OnSetAppId(StrView app_id) {
  this->app_id = Str(app_id);
  if (Ptr<ReferenceCounted> win_obj = window.Lock()) {
    auto win = win_obj.Cast<WaylandWindow>();
    auto lock = std::lock_guard(win->mutex);
    win->app_id = Str(app_id);
  }
}

void Subcompositor::OnGetSubsurface(Subsurface& id, Surface& surface, Surface& parent) {
  if (surface.as_subsurface || surface.xdg) {
    client.ProtocolError(this->id, Subcompositor::ErrorBadSurface,
                         "the surface already has a role");
    return;
  }
  for (Surface* a = &parent; a; a = a->as_subsurface ? a->as_subsurface->parent : nullptr)
    if (a == &surface) {
      client.ProtocolError(this->id, Subcompositor::ErrorBadParent,
                           "the parent is the surface or a descendant");
      return;
    }
  id.surface = &surface;
  id.parent = &parent;
  surface.as_subsurface = &id;
  parent.pending_stack.push_back(&surface);  // newest on top
}

void Subsurface::OnSetPosition(I32 x, I32 y) {
  pending_pos = {x, y};
  position_dirty = true;
}
void Subsurface::OnSetSync() { sync = true; }
void Subsurface::OnSetDesync() { sync = false; }
void Subsurface::OnPlaceAbove(Surface& sibling) { RestackSubsurface(*this, &sibling, true); }
void Subsurface::OnPlaceBelow(Surface& sibling) { RestackSubsurface(*this, &sibling, false); }

void Viewporter::OnGetViewport(Viewport& id, Surface& surface) {
  if (surface.viewport) {
    client.ProtocolError(this->id, Viewporter::ErrorViewportExists,
                         "surface already has a viewport");
    return;
  }
  id.surface = &surface;
  surface.viewport = &id;
}

void Viewport::OnSetSource(float x, float y, float width, float height) {
  if (!surface) {
    client.ProtocolError(id, Viewport::ErrorNoSurface, "the wl_surface was destroyed");
    return;
  }
  if (x == -1 && y == -1 && width == -1 && height == -1) {
    src_x = src_y = src_w = src_h = -1;
    return;
  }
  if (x < 0 || y < 0 || width <= 0 || height <= 0) {
    client.ProtocolError(id, Viewport::ErrorBadValue,
                         "negative origin or non-positive source size");
    return;
  }
  src_x = x;
  src_y = y;
  src_w = width;
  src_h = height;
}

void Viewport::OnSetDestination(I32 width, I32 height) {
  if (!surface) {
    client.ProtocolError(id, Viewport::ErrorNoSurface, "the wl_surface was destroyed");
    return;
  }
  if (width == -1 && height == -1) {
    dst_size = {-1, -1};
    return;
  }
  if (width <= 0 || height <= 0) {
    client.ProtocolError(id, Viewport::ErrorBadValue, "non-positive destination size");
    return;
  }
  dst_size = {width, height};
}

namespace {

void UpdateDecoration(XdgToplevel& t) {
  using M = ZxdgToplevelDecorationV1::Mode;
  using P = WaylandWindow::DecorationPreference;
  M mode = t.decoration ? (M)t.decoration->client_mode : M::ModeClientSide;
  if (Ptr<ReferenceCounted> ptr = t.window.Lock()) {
    WaylandWindow* w = ptr.Get<WaylandWindow>();
    P pref = w->decoration_preference.load(std::memory_order_relaxed);
    if (pref == P::ServerSide) {
      mode = M::ModeServerSide;
    } else if (pref == P::ClientSide) {
      mode = M::ModeClientSide;
    }
    // TODO: this should be stored only on surface Commit
    w->server_side_decorated.store(mode == M::ModeServerSide, std::memory_order_relaxed);
    w->WakeToys();
  }
  if (t.decoration) {
    t.decoration->Configure(mode);
    if (XdgSurface* xdg = t.xdg.Get(); xdg && xdg->initial_configure_sent)
      xdg->Configure(xdg->last_configure_serial = t.client.server.serial++);
  }
}

}  // namespace

void ZxdgDecorationManagerV1::OnGetToplevelDecoration(ZxdgToplevelDecorationV1& id,
                                                      XdgToplevel& toplevel) {
  if (toplevel.decoration) {
    id.client.ProtocolError(id.id, ZxdgToplevelDecorationV1::ErrorAlreadyConstructed,
                            "xdg_toplevel already has a decoration object");
    return;
  }
  id.toplevel = &toplevel;
  toplevel.decoration = &id;
  UpdateDecoration(toplevel);
}
void ZxdgToplevelDecorationV1::OnSetMode(enum Mode mode) {
  if (mode != ModeClientSide && mode != ModeServerSide) {
    client.ProtocolError(id, ErrorInvalidMode, "invalid decoration mode");
    return;
  }
  client_mode = mode;
  if (toplevel) UpdateDecoration(*toplevel);
}
void ZxdgToplevelDecorationV1::OnUnsetMode() {
  client_mode = 0;
  if (toplevel) UpdateDecoration(*toplevel);
}

void XdgPositioner::OnSetSize(I32 width, I32 height) { size = {width, height}; }
void XdgPositioner::OnSetAnchorRect(I32 x, I32 y, I32 width, I32 height) {
  anchor_rect = SkIRect::MakeXYWH(x, y, width, height);
}
void XdgPositioner::OnSetAnchor(enum Anchor a) { anchor = (U32)a; }
void XdgPositioner::OnSetGravity(enum Gravity g) { gravity = (U32)g; }
void XdgPositioner::OnSetOffset(I32 x, I32 y) { offset = {x, y}; }
void XdgPositioner::OnSetConstraintAdjustment(enum ConstraintAdjustment ca) {
  constraint_adjustment = (U32)ca;
}

void XdgSurface::OnGetPopup(XdgPopup& id, XdgSurface* parent, XdgPositioner& positioner) {
  id.xdg = this;
  popup = &id;
  id.parent = parent;
  ResolvePopup(positioner, id);
  if (parent) parent->child_popups.push_back(&id);
}

XdgPopup* XdgSurface::TopmostPopup() {
  for (auto it = child_popups.rbegin(); it != child_popups.rend(); ++it) {
    XdgPopup* pp = *it;
    if (!pp->xdg) continue;
    if (XdgPopup* deeper = pp->xdg->TopmostPopup()) return deeper;
    return pp;
  }
  return nullptr;
}

void XdgPopup::OnDestroy() {
  if (xdg && !xdg->child_popups.empty())
    client.ProtocolError(id, XdgWmBase::ErrorNotTheTopmostPopup,
                         "destroyed a popup that is not the topmost");
}

void XdgPopup::OnGrab(Seat&, U32) {
  Server& s = client.server;
  s.grabbing_popup = this;
  if (xdg && xdg->surface) KeyboardFocusTo(s, xdg->surface);
}
void XdgPopup::OnReposition(XdgPositioner& positioner, U32 token) {
  Server& s = client.server;
  ResolvePopup(positioner, *this);
  Repositioned(token);
  Configure(geo.x(), geo.y(), geo.width(), geo.height());
  if (xdg) xdg->Configure(xdg->last_configure_serial = s.serial++);
  if (parent && parent->surface) ApplyAndPublish(*parent->surface);
}

void CursorShapeDeviceV1::OnSetShape(U32, enum Shape shape) {
  Server& s = client.server;
  for (auto* handle : client.client_ids) {
    if (auto* t = dyn_cast_if_present<XdgToplevel>(handle)) {
      if (Ptr<ReferenceCounted> w = t->window.Lock()) {
        auto win = w.Cast<WaylandWindow>();
        win->cursor_shape.store((uint32_t)shape, std::memory_order_relaxed);
        win->WakeToys();
      }
    }
  }
}

void Seat::OnGetPointer(Pointer& id) { id.version = version; }

void Seat::OnGetKeyboard(Keyboard& id) {
  id.version = version;
  Server& s = client.server;
  if (s.keymap_fd < 0) {
    if (xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
      xkb_keymap* keymap = nullptr;
      if (xcb::connection) {
        xkb_x11_setup_xkb_extension(
            xcb::connection, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr, nullptr);
        int32_t device = xkb_x11_get_core_keyboard_device_id(xcb::connection);
        if (device >= 0)
          keymap = xkb_x11_keymap_new_from_device(ctx, xcb::connection, device,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
      }
      if (!keymap) keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (!keymap) {
        ERROR << "Wayland: couldn't build an xkb keymap; keyboard input disabled.";
      } else {
        char* str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        s.keymap_size = strlen(str) + 1;
        s.keymap_fd = memfd_create("automat-keymap", MFD_CLOEXEC);
        if (s.keymap_fd >= 0) (void)!write(s.keymap_fd, str, s.keymap_size);
        auto Mask = [&](const char* name) -> U32 {
          xkb_mod_index_t i = xkb_keymap_mod_get_index(keymap, name);
          return i == XKB_MOD_INVALID ? 0 : (1u << i);
        };
        s.mod_shift = Mask(XKB_MOD_NAME_SHIFT);
        s.mod_ctrl = Mask(XKB_MOD_NAME_CTRL);
        s.mod_alt = Mask(XKB_MOD_NAME_ALT);
        s.mod_super = Mask(XKB_MOD_NAME_LOGO);
        free(str);
        xkb_keymap_unref(keymap);
      }
      xkb_context_unref(ctx);
    }
  }
  if (s.keymap_fd >= 0) id.Keymap(Keyboard::KeymapFormatXkbV1, FD(dup(s.keymap_fd)), s.keymap_size);
  // Automat forwards the host's key auto-repeat, so clients must not also repeat.
  if (version >= 4) id.RepeatInfo(0, 0);
}

void DataDeviceManager::OnGetDataDevice(DataDevice& id, Seat&) {
  OfferSelectionTo(client.server, id);
}
void DataDevice::OnSetSelection(DataSource* source, U32) {
  Server& s = client.server;
  if (s.selection && s.selection != source) s.selection->Cancelled();
  s.selection = source;
  for (DataDevice& dev : DataDevice::colony) OfferSelectionTo(s, dev);
}
void DataSource::OnOffer(StrView mime) { mimes.push_back(Str(mime)); }

void DataOffer::OnReceive(StrView mime, FD&& fd) {
  if (source) {
    source->Send(mime, std::move(fd));
    client.server.FlushAll();
  }
}

void LinuxDmabufV1::OnGetDefaultFeedback(LinuxDmabufFeedbackV1& id) {
  SendDmabufFeedback(client.server, id);
}
void LinuxDmabufV1::OnGetSurfaceFeedback(LinuxDmabufFeedbackV1& id, Surface&) {
  SendDmabufFeedback(client.server, id);
}

void LinuxBufferParamsV1::OnAdd(FD&& fd, U32 plane_idx, U32 offset, U32 stride, U32 mod_hi,
                                U32 mod_lo) {
  if (plane_idx >= 4) {
    client.ProtocolError(id, LinuxBufferParamsV1::ErrorPlaneIdx, "plane index out of bounds");
    return;
  }
  if (planes[plane_idx].fd.fd >= 0) {
    client.ProtocolError(id, LinuxBufferParamsV1::ErrorPlaneSet, "plane already set");
    return;
  }
  planes[plane_idx] = {std::move(fd), offset, stride};
  if ((int)plane_idx + 1 > plane_count) plane_count = plane_idx + 1;
  modifier = ((U64)mod_hi << 32) | mod_lo;
}

void LinuxBufferParamsV1::OnCreate(I32 width, I32 height, U32 format, enum Flags flags) {
  if (used) {
    client.ProtocolError(id, LinuxBufferParamsV1::ErrorAlreadyUsed, "params already used");
    return;
  }
  used = true;
  if (width <= 0 || height <= 0 || plane_count == 0) {
    Failed();
    return;
  }
  Buffer& buf = Buffer::ColonyMake(client.server_id_pool.Grab(), client);
  FillDmabufBuffer(*this, buf, width, height, format,
                   ((U32)flags & LinuxBufferParamsV1::FlagsYInvert) != 0);
  Created(buf);
}

void LinuxBufferParamsV1::OnCreateImmed(Buffer& buffer_id, I32 width, I32 height, U32 format,
                                        enum Flags flags) {
  if (used) {
    client.ProtocolError(id, LinuxBufferParamsV1::ErrorAlreadyUsed, "params already used");
    return;
  }
  used = true;
  if (width <= 0 || height <= 0 || plane_count == 0) {
    client.ProtocolError(id, LinuxBufferParamsV1::ErrorIncomplete,
                         "missing planes or bad dimensions");
    return;
  }
  FillDmabufBuffer(*this, buffer_id, width, height, format,
                   ((U32)flags & LinuxBufferParamsV1::FlagsYInvert) != 0);
}

void AdvertiseDmabufOnBind(LinuxDmabufV1& obj, U32 version) {
  if (version >= 4) return;  // version 4+ clients learn the formats through feedback
  for (auto& fm : kDmabufFormats) {
    obj.Format(fm.format);
    if (version >= 3)
      obj.Modifier(fm.format, (U32)(fm.modifier >> 32), (U32)(fm.modifier & 0xffffffffu));
  }
}

void Server::SendKeyboardEnter(library::WaylandWindow& w) {
  auto* h = (XdgToplevel*)w.toplevel_handle.load();
  if (!h) return;
  epoll.Post([this, h] {
    XdgToplevel* t = LiveToplevel(h);
    if (!t || !t->xdg || !t->xdg->surface) return;
    KeyboardFocusTo(*this, t->xdg->surface);
    FlushAll();
  });
}
void Server::SendKeyboardLeave(library::WaylandWindow& w) {
  auto* h = (XdgToplevel*)w.toplevel_handle.load();
  if (!h) return;
  epoll.Post([this, h] {
    if (!LiveToplevel(h)) return;
    KeyboardFocusTo(*this, nullptr);
    FlushAll();
  });
}
void Server::SendKey(library::WaylandWindow& w, uint32_t evdev_keycode, bool pressed, bool ctrl,
                     bool alt, bool shift, bool super) {
  auto* h = (XdgToplevel*)w.toplevel_handle.load();
  if (!h) return;
  epoll.Post([this, h, evdev_keycode, pressed, ctrl, alt, shift, super] {
    if (!LiveToplevel(h) || !keyboard_surface) return;
    Client* c = &keyboard_surface->client;
    U32 mods = (ctrl ? mod_ctrl : 0) | (alt ? mod_alt : 0) | (shift ? mod_shift : 0) |
               (super ? mod_super : 0);
    U32 time = NowMs();
    for (Keyboard& k : Keyboard::colony) {
      if (&k.client != c) continue;
      k.Modifiers(serial++, mods, 0, 0, 0);
      k.Key(serial++, time, evdev_keycode,
            pressed ? Keyboard::KeyStatePressed : Keyboard::KeyStateReleased);
    }
    FlushAll();
  });
}
void Server::SendDecorationPreference(library::WaylandWindow& w) {
  auto* h = (XdgToplevel*)w.toplevel_handle.load();
  if (!h) return;
  epoll.Post([this, h] {
    if (XdgToplevel* t = LiveToplevel(h)) {
      UpdateDecoration(*t);
      FlushAll();
    }
  });
}

// Deleting a window object asks its client to close, then SIGTERMs it after a grace period.
void Server::NotifyWindowDestroyed(void* handle) {
  if (!handle) return;
  epoll.Post([this, handle] {
    XdgToplevel* t = LiveToplevel(static_cast<XdgToplevel*>(handle));
    if (!t) return;
    t->Close();
    FlushAll();
    if (!t->sigterm_timer) {
      t->sigterm_timer = std::make_unique<mux::Timer>(epoll);
      t->sigterm_timer->handler = [pid = t->pid] {
        if (pid > 0) kill(pid, SIGTERM);
      };
      t->sigterm_timer->Arm(2.0);
    }
  });
}

void Server::UIFrame() {
  std::vector<Ptr<ReferenceCounted>> appeared, disappeared;
  {
    auto lock = std::lock_guard(ui_mutex);
    appeared.swap(ui_appeared);
    disappeared.swap(ui_disappeared);
  }
  static int spawn_count = 0;
  for (auto& w : appeared) {
    auto& win = static_cast<WaylandWindow&>(*w);
    // Find the Command whose child this client is: its argv becomes the respawn recipe, its plate
    // anchors where the window is seated, and the window keeps a Launcher cable to it.
    Location* command_location = nullptr;
    library::Command* command = nullptr;
    SkISize win_size = {};
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
    if (command && !win.launcher->IsConnected()) win.launcher->Connect(Interface(command, nullptr));
    auto& loc = vm.root_board->CreateEmpty();
    int n = spawn_count++;
    if (command_location) {
      Vec2 plate = library::CommandPlateSize();
      Vec2 size = library::WindowBoardSize(win_size.width(), win_size.height());
      loc.position =
          command_location->position + Vec2(plate.x / 2 + 0.008f + size.x / 2 + 0.006f * (n % 3),
                                            plate.y / 2 - size.y / 2 - 0.012f * (n % 3));
    } else {
      loc.position = Vec2(0.01f * (n % 3), -0.02f * (n % 5));
    }
    loc.InsertHere(w.Cast<Object>());
    vm.root_board->WakeToys();
    vm.WakeToys();
  }
  // Respawn deserialized or cloned windows: re-run the recipe (preferring the linked Command) and
  // register the new pid for adoption into the existing object.
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
      auto lock = std::lock_guard(adoption_mutex);
      adoptions.emplace_back(pid, win->AcquireWeakPtr());
    }
  }
  for (auto& w : disappeared) {
    Location* here = static_cast<Object*>(w.Get())->here;
    if (!here) continue;
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

void XdgWmBase::OnDestroy() {}
void XdgWmBase::OnCreatePositioner(XdgPositioner& id) {}
void XdgWmBase::OnPong(U32 serial) {}
void XdgPositioner::OnDestroy() {}
void XdgPositioner::OnSetReactive() {}
void XdgPositioner::OnSetParentSize(I32 parent_width, I32 parent_height) {}
void XdgPositioner::OnSetParentConfigure(U32 serial) {}
void XdgToplevel::OnDestroy() {}
void XdgToplevel::OnSetParent(XdgToplevel* parent) {}
void XdgToplevel::OnShowWindowMenu(Seat& seat, U32 serial, I32 x, I32 y) {}
void XdgToplevel::OnMove(Seat& seat, U32 serial) {}
void XdgToplevel::OnResize(Seat& seat, U32 serial, enum ResizeEdge edges) {}
void XdgToplevel::OnSetMaxSize(I32 width, I32 height) {}
void XdgToplevel::OnSetMinSize(I32 width, I32 height) {}
void XdgToplevel::OnSetMaximized() {}
void XdgToplevel::OnUnsetMaximized() {}
void XdgToplevel::OnSetFullscreen(Output* output) {}
void XdgToplevel::OnUnsetFullscreen() {}
void XdgToplevel::OnSetMinimized() {}
void Viewporter::OnDestroy() {}
void Viewport::OnDestroy() {}
void Compositor::OnCreateRegion(Region& id) {}
void Shm::OnRelease() {}
void Buffer::OnDestroy() {}
void DataDevice::OnStartDrag(DataSource* source, Surface& origin, Surface* icon, U32 serial) {}
void DataDevice::OnRelease() {}
void DataDeviceManager::OnCreateDataSource(DataSource& id) {}
void Shell::OnGetShellSurface(ShellSurface& id, Surface& surface) {}
void ShellSurface::OnPong(U32 serial) {}
void ShellSurface::OnMove(Seat& seat, U32 serial) {}
void ShellSurface::OnResize(Seat& seat, U32 serial, enum Resize edges) {}
void ShellSurface::OnSetToplevel() {}
void ShellSurface::OnSetTransient(Surface& parent, I32 x, I32 y, enum Transient flags) {}
void ShellSurface::OnSetFullscreen(enum FullscreenMethod method, U32 framerate, Output* output) {}
void ShellSurface::OnSetPopup(Seat& seat, U32 serial, Surface& parent, I32 x, I32 y,
                              enum Transient flags) {}
void ShellSurface::OnSetMaximized(Output* output) {}
void ShellSurface::OnSetTitle(StrView title) {}
void ShellSurface::OnSetClass(StrView class_) {}
void Seat::OnGetTouch(Touch& id) {}
void Seat::OnRelease() {}
void Pointer::OnSetCursor(U32 serial, Surface* surface, I32 hotspot_x, I32 hotspot_y) {}
void Pointer::OnRelease() {}
void Keyboard::OnRelease() {}
void Touch::OnRelease() {}
void Output::OnRelease() {}
void Region::OnDestroy() {}
void Subcompositor::OnDestroy() {}
void Subsurface::OnDestroy() {}
void Fixes::OnDestroy() {}
void Fixes::OnDestroyRegistry(Registry& registry) {}
void CursorShapeManagerV1::OnDestroy() {}
void CursorShapeManagerV1::OnGetPointer(CursorShapeDeviceV1& cursor_shape_device,
                                        Pointer& pointer) {}
void CursorShapeManagerV1::OnGetTabletToolV2(CursorShapeDeviceV1& cursor_shape_device,
                                             U32 tablet_tool) {}
void CursorShapeDeviceV1::OnDestroy() {}
void LinuxDmabufV1::OnDestroy() {}
void LinuxDmabufV1::OnCreateParams(LinuxBufferParamsV1& params_id) {}
void LinuxBufferParamsV1::OnDestroy() {}
void LinuxDmabufFeedbackV1::OnDestroy() {}
void ZxdgDecorationManagerV1::OnDestroy() {}
void ZxdgToplevelDecorationV1::OnDestroy() {}
void ShmPool::OnDestroy() {}
void DataOffer::OnAccept(U32 serial, StrView mime_type) {}
void DataOffer::OnDestroy() {}
void DataOffer::OnFinish() {}
void DataOffer::OnSetActions(enum DataDeviceManager::DndAction dnd_actions,
                             enum DataDeviceManager::DndAction preferred_action) {}
void DataSource::OnDestroy() {}
void DataSource::OnSetActions(enum DataDeviceManager::DndAction dnd_actions) {}
void Surface::OnDestroy() {}
void Surface::OnDamage(I32 x, I32 y, I32 width, I32 height) {}
void Surface::OnSetOpaqueRegion(Region* region) {}
void Surface::OnDamageBuffer(I32 x, I32 y, I32 width, I32 height) {}
void Surface::OnOffset(I32 x, I32 y) {}

}  // namespace automat::wayland
