// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "x11.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkFont.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkRegion.h>
#include <include/core/SkShader.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTextBlob.h>
#include <include/effects/SkGradient.h>
#include <include/pathops/SkPathOps.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
// windows.h macros that collide with X11 request names and our helpers.
#undef CreateWindow
#undef GetAtomName
#undef SetProp
#undef GetProp
#undef RemoveProp
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#endif

#include "color.hpp"
#include "format.hpp"
#include "keyboard.hpp"
#include "keymap.hpp"
#include "location.hpp"
#include "log.hpp"
#include "math.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "toy.hpp"
#include "ui_beta.hpp"
#include "units.hpp"
#include "vk.hpp"
#include "vm.hpp"
#include "window_frame.hpp"
#include "x11_keys.hpp"
#include "x11_protocol.hpp"
#include "x11_xkb.hpp"

using namespace automat::x11;

namespace automat::x11 {

std::unique_ptr<Server> server;

// The client whose request burst is being dispatched. Set around a dispatch so the
// Writer (which holds only an out buffer) can route out-of-band fds to it.
Client* g_current_client = nullptr;

Colony<Client> Client::colony;

// Fixed server-side resource ids (below any client's id_base).
constexpr U32 kRoot = 0x40;
constexpr U32 kColormap = 0x44;
constexpr U32 kVisual24 = 0x21;
constexpr U32 kVisual32 = 0x22;
constexpr U32 kWmCheck = 0x50;  // the _NET_SUPPORTING_WM_CHECK window

// One client pixel on the board, matching the Wayland compositor's scale.
constexpr float kPx = 0.20_mm;
constexpr float kMinContent = 3_cm;

// The virtual root screen. Clients place windows in this space; Automat lays them out on
// the board instead, but apps still query these dimensions.
constexpr int kScreenW = 1920, kScreenH = 1080;

#if defined(_WIN32)
// TCP loopback carries no SO_PEERCRED; find the peer's pid by locating the mirrored
// connection (their local == our remote) in the system TCP table.
static I64 PeerPid(int socket_fd) {
  sockaddr_in peer{}, local{};
  int peer_len = sizeof(peer), local_len = sizeof(local);
  if (getpeername((SOCKET)socket_fd, (sockaddr*)&peer, &peer_len) != 0) return 0;
  if (getsockname((SOCKET)socket_fd, (sockaddr*)&local, &local_len) != 0) return 0;
  ULONG size = 0;
  GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
  std::string buf(size, '\0');
  auto* table = (MIB_TCPTABLE_OWNER_PID*)buf.data();
  if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) !=
      NO_ERROR)
    return 0;
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    auto& row = table->table[i];
    if ((row.dwLocalPort & 0xffff) == peer.sin_port && row.dwLocalAddr == peer.sin_addr.s_addr &&
        (row.dwRemotePort & 0xffff) == local.sin_port && row.dwRemoteAddr == local.sin_addr.s_addr)
      return row.dwOwningPid;
  }
  return 0;
}
#endif

// ---- wire helpers ------------------------------------------------------------------

FD Reader::TakeFd(Client& client) {
  if (client.recv_fds.empty()) return FD();
  FD fd = std::move(client.recv_fds.front());
  client.recv_fds.pop_front();
  return fd;
}

Writer::Writer(Client& client, size_t total) : out(client.out), pos(client.out.size()) {
  out.resize(out.size() + total, 0);
}
void Writer::TakeFd(FD&& fd) {
  if (g_current_client) g_current_client->out_fds.push_back(std::move(fd));
}

U16 ClientSequence(Client& client) { return client.sequence; }

bool DecodeFailed(Client& client, U16 sequence) {
  client.sequence = sequence;
  client.Error(16 /* Length */, 0);
  return true;
}

// ---- little-endian scratch encoders (setup blob, hand-built replies) ---------------

struct Buf {
  std::string bytes;
  template <class T>
  void P(T v) {
    size_t o = bytes.size();
    bytes.resize(o + sizeof(T));
    std::memcpy(bytes.data() + o, &v, sizeof(T));
  }
  void Pad(size_t n) { bytes.resize(bytes.size() + n, 0); }
  void Str(StrView s) { bytes.append(s.data(), s.size()); }
  void AlignTo4() { bytes.resize((bytes.size() + 3) & ~size_t(3), 0); }
};

// ---- atoms -------------------------------------------------------------------------

// The 68 predefined atoms, in protocol order (1..68).
static const char* kPredefinedAtoms[] = {
    "PRIMARY",
    "SECONDARY",
    "ARC",
    "ATOM",
    "BITMAP",
    "CARDINAL",
    "COLORMAP",
    "CURSOR",
    "CUT_BUFFER0",
    "CUT_BUFFER1",
    "CUT_BUFFER2",
    "CUT_BUFFER3",
    "CUT_BUFFER4",
    "CUT_BUFFER5",
    "CUT_BUFFER6",
    "CUT_BUFFER7",
    "DRAWABLE",
    "FONT",
    "INTEGER",
    "PIXMAP",
    "POINT",
    "RECTANGLE",
    "RESOURCE_MANAGER",
    "RGB_COLOR_MAP",
    "RGB_BEST_MAP",
    "RGB_BLUE_MAP",
    "RGB_DEFAULT_MAP",
    "RGB_GRAY_MAP",
    "RGB_GREEN_MAP",
    "RGB_RED_MAP",
    "STRING",
    "VISUALID",
    "WINDOW",
    "WM_COMMAND",
    "WM_HINTS",
    "WM_CLIENT_MACHINE",
    "WM_ICON_NAME",
    "WM_ICON_SIZE",
    "WM_NAME",
    "WM_NORMAL_HINTS",
    "WM_SIZE_HINTS",
    "WM_ZOOM_HINTS",
    "MIN_SPACE",
    "NORM_SPACE",
    "MAX_SPACE",
    "END_SPACE",
    "SUPERSCRIPT_X",
    "SUPERSCRIPT_Y",
    "SUBSCRIPT_X",
    "SUBSCRIPT_Y",
    "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS",
    "STRIKEOUT_ASCENT",
    "STRIKEOUT_DESCENT",
    "ITALIC_ANGLE",
    "X_HEIGHT",
    "QUAD_WIDTH",
    "WEIGHT",
    "POINT_SIZE",
    "RESOLUTION",
    "COPYRIGHT",
    "NOTICE",
    "FONT_NAME",
    "FAMILY_NAME",
    "FULL_NAME",
    "CAP_HEIGHT",
    "WM_CLASS",
    "WM_TRANSIENT_FOR",
};

U32 Server::InternAtom(StrView name, bool only_if_exists) {
  auto it = atom_ids.find(name);
  if (it != atom_ids.end()) return it->second;
  if (only_if_exists) return 0;
  U32 id = (U32)atom_names.size();
  atom_names.push_back(Str(name));
  atom_ids[atom_names.back()] = id;
  return id;
}
StrView Server::AtomName(U32 atom) {
  return atom < atom_names.size() ? StrView(atom_names[atom]) : StrView();
}

// ---- resource cleanup --------------------------------------------------------------

ShmSeg::~ShmSeg() {
#if !defined(_WIN32)
  if (!addr) return;
  if (sysv)
    shmdt(addr);
  else
    munmap(addr, size);
#endif
}

void UnmapAndForgetToplevel(Window& w);  // fwd

void Server::Free(U32 xid) {
  auto it = resources.find(xid);
  if (it == resources.end()) return;
  Resource* r = it->second.get();
  if (r->type == ResType::Window) {
    Window& w = *static_cast<Window*>(r);
    // Recursing into the children erases from `resources` and mutates `w.children`, so
    // snapshot the ids first and erase this window by key afterwards - the `it` above and
    // `w.children` are both invalid once the recursion runs.
    Vec<U32> child_ids;
    child_ids.reserve(w.children.size());
    for (Window* child : w.children) child_ids.push_back(child->xid);
    if (w.parent) std::erase(w.parent->children, &w);
    if (w.is_toplevel) UnmapAndForgetToplevel(w);
    for (U32 child_id : child_ids) Free(child_id);
  }
  resources.erase(xid);
}

void Server::DestroyClientResources(Client& c) {
  Vec<U32> mine;
  for (auto& [xid, res] : resources)
    if (res->owner == &c) mine.push_back(xid);
  for (U32 xid : mine) Free(xid);
  for (auto it = selection_owner.begin(); it != selection_owner.end();) {
    Window* w = Get<Window>(it->second, ResType::Window);
    if (!w || w->owner == &c)
      it = selection_owner.erase(it);
    else
      ++it;
  }
}

// ---- drawable backing store --------------------------------------------------------

void Drawable::Ensure(int w, int h, U8 d) {
  width = w;
  height = h;
  depth = d;
  if (w <= 0 || h <= 0) {
    surface = nullptr;
    return;
  }
  auto info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
  surface = SkSurfaces::Raster(info);
  if (surface) surface->getCanvas()->clear(SK_ColorTRANSPARENT);
}
SkCanvas* Drawable::Canvas() { return surface ? surface->getCanvas() : nullptr; }

// The top-level ancestor of `w` (child of root), or w itself if it is one.
static Window* ToplevelOf(Window* w) {
  while (w && w->parent && w->parent != server->root) w = w->parent;
  return w;
}

static SkColor PixelToColor(U32 px, U8 depth) {
  if (depth >= 32) return px;               // ARGB
  return 0xff000000u | (px & 0x00ffffffu);  // opaque
}

// Sets a server-managed property on a window (used for the EWMH WM identity and per-window
// WM_STATE, which toolkits read to decide a window is managed and paintable).
static void SetProp(Window& w, StrView name, U32 type_atom, U8 format, const void* data,
                    size_t bytes) {
  Window::Property& p = w.props[server->InternAtom(name, false)];
  p.type = type_atom;
  p.format = format;
  p.data.assign((const U8*)data, (const U8*)data + bytes);
}

// Dirtied top-levels, published once at the end of a request burst.
static Vec<Window*> g_dirty;
static void MarkDirty(Window* w) {
  Window* top = ToplevelOf(w);
  if (top && top->is_toplevel && std::find(g_dirty.begin(), g_dirty.end(), top) == g_dirty.end())
    g_dirty.push_back(top);
}

// ---- window tree composite ---------------------------------------------------------

static void CompositeInto(SkCanvas& canvas, Window& w) {
  if (w.surface) {
    canvas.save();
    canvas.clipIRect(SkIRect::MakeWH(w.width, w.height));
    w.surface->draw(&canvas, 0, 0, SkSamplingOptions(), nullptr);
    canvas.restore();
  }
  for (Window* child : w.children) {
    if (!child->mapped || child->win_class == 2 /* InputOnly */) continue;
    canvas.save();
    canvas.translate(child->x, child->y);
    CompositeInto(canvas, *child);
    canvas.restore();
  }
}

void Server::PublishWindow(Window& w) {
  if (!w.is_toplevel) return;
  Ptr<library::X11Window> obj_ptr = w.object.Lock();
  if (!obj_ptr) return;
  sk_sp<SkImage> snapshot;
  if (w.width > 0 && w.height > 0) {
    if (w.children.empty()) {
      snapshot = w.surface ? w.surface->makeImageSnapshot() : nullptr;
    } else {
      auto info = SkImageInfo::Make(w.width, w.height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
      auto comp = SkSurfaces::Raster(info);
      comp->getCanvas()->clear(SK_ColorTRANSPARENT);
      CompositeInto(*comp->getCanvas(), w);
      snapshot = comp->makeImageSnapshot();
    }
  }
  library::X11Window& obj = *obj_ptr;
  {
    auto lock = std::lock_guard(obj.mutex);
    obj.image = snapshot;
    obj.content_size = {w.width, w.height};
    obj.input_region = SkPath::Rect(SkRect::MakeIWH(w.width, w.height));
  }
  obj.WakeToys();
  vm.WakeToys();
}

}  // namespace automat::x11

// ============================================================================
//  Client connection: handshake, dispatch, flush
// ============================================================================

namespace automat::x11 {

using library::X11Window;

// Fills the connection setup reply (everything after the initial 8-byte header).
static void BuildSetupReply(Client& client, Buf& b) {
  b.P<U32>(1);               // release number
  b.P<U32>(client.id_base);  // resource-id-base
  b.P<U32>(client.id_mask);  // resource-id-mask
  b.P<U32>(0);               // motion buffer size
  const char* vendor = "Automat";
  U16 vendor_len = (U16)strlen(vendor);
  b.P<U16>(vendor_len);
  b.P<U16>(0xffff);  // maximum-request-length (BIG-REQUESTS raises it)
  b.P<U8>(1);        // number of screens
  b.P<U8>(2);        // number of pixmap formats
  b.P<U8>(0);        // image byte order: LSBFirst
  b.P<U8>(0);        // bitmap format bit order: LeastSignificant
  b.P<U8>(32);       // bitmap format scanline unit
  b.P<U8>(32);       // bitmap format scanline pad
  b.P<U8>(8);        // min keycode
  b.P<U8>(255);      // max keycode
  b.Pad(4);
  b.Str(vendor);
  b.AlignTo4();
  // Pixmap formats (depth, bpp, scanline pad, 5 pad).
  for (U8 depth : {(U8)24, (U8)32}) {
    b.P<U8>(depth);
    b.P<U8>(32);
    b.P<U8>(32);
    b.Pad(5);
  }
  // One screen.
  b.P<U32>(kRoot);       // root
  b.P<U32>(kColormap);   // default colormap
  b.P<U32>(0xffffffff);  // white pixel
  b.P<U32>(0xff000000);  // black pixel
  b.P<U32>(0);           // current input masks
  b.P<U16>(kScreenW);
  b.P<U16>(kScreenH);
  b.P<U16>(508);        // width mm
  b.P<U16>(285);        // height mm
  b.P<U16>(1);          // min installed maps
  b.P<U16>(1);          // max installed maps
  b.P<U32>(kVisual24);  // root visual
  b.P<U8>(0);           // backing stores: Never
  b.P<U8>(0);           // save unders
  b.P<U8>(24);          // root depth
  b.P<U8>(2);           // number of allowed depths
  // depth 24: one TrueColor visual.
  auto visual = [&](U32 id) {
    b.P<U32>(id);
    b.P<U8>(4);  // class TrueColor
    b.P<U8>(8);  // bits per rgb
    b.P<U16>(256);
    b.P<U32>(0x00ff0000);  // red mask
    b.P<U32>(0x0000ff00);  // green mask
    b.P<U32>(0x000000ff);  // blue mask
    b.Pad(4);
  };
  b.P<U8>(24);
  b.Pad(1);
  b.P<U16>(1);
  b.Pad(4);
  visual(kVisual24);
  b.P<U8>(32);
  b.Pad(1);
  b.P<U16>(1);
  b.Pad(4);
  visual(kVisual32);
}

static void HandleSetup(Client& client) {
  const char* p = client.in.data();
  U8 order = (U8)p[0];
  if (order != 0x6c /* 'l' */) {
    // Refuse big-endian; reply with a failure.
    U8 reason[] = "x11: only little-endian clients are supported";
    Buf b;
    b.P<U8>(0);  // failed
    b.P<U8>((U8)(sizeof(reason) - 1));
    b.P<U16>(11);
    b.P<U16>(0);
    b.P<U16>((U16)((sizeof(reason) - 1 + 3) / 4));
    b.Str((const char*)reason);
    b.AlignTo4();
    client.out.append(b.bytes);
    client.Flush();
    client.Disconnect();
    return;
  }
  client.id_base = client.index << 22;
  client.id_mask = 0x3fffff;

  Buf extra;
  BuildSetupReply(client, extra);
  Buf hdr;
  hdr.P<U8>(1);    // success
  hdr.P<U8>(0);    // pad
  hdr.P<U16>(11);  // protocol major
  hdr.P<U16>(0);   // protocol minor
  hdr.P<U16>((U16)(extra.bytes.size() / 4));
  client.out.append(hdr.bytes);
  client.out.append(extra.bytes);
  client.setup_done = true;
}

bool DispatchExtension(Client& client, U8 major, U8 opcode, Reader& r, const U8* raw,
                       size_t raw_len);

void Client::NotifyRead(Status&) {
  for (;;) {
    char buf[8192];
#if defined(_WIN32)
    // Plain TCP: no ancillary data, so recv_fds stays empty.
    int n = recv((SOCKET)fd.fd, buf, sizeof(buf), 0);
    if (n > 0) {
      in.append(buf, n);
      continue;
    }
    if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
      Disconnect();
      return;
    }
#else
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
          for (int k = 0; k < count; ++k) {
            int rfd;
            std::memcpy(&rfd, CMSG_DATA(cm) + k * sizeof(int), sizeof(int));
            recv_fds.push_back(FD(rfd));
          }
        }
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      Disconnect();
      return;
    }
#endif
    break;
  }

  g_current_client = this;
  size_t offset = 0;
  if (!setup_done) {
    if (in.size() < 12) {
      g_current_client = nullptr;
      return;
    }
    U16 name_len, data_len;
    std::memcpy(&name_len, in.data() + 6, 2);
    std::memcpy(&data_len, in.data() + 8, 2);
    size_t total = 12 + ((name_len + 3) & ~3) + ((data_len + 3) & ~3);
    if (in.size() < total) {
      g_current_client = nullptr;
      return;
    }
    HandleSetup(*this);
    offset = total;
  }

  while (setup_done && in.size() - offset >= 4) {
    const char* p = in.data() + offset;
    U8 major = (U8)p[0];
    U8 data_byte = (U8)p[1];
    U16 len_units;
    std::memcpy(&len_units, p + 2, 2);
    size_t len = (size_t)len_units * 4;
    size_t body = 4;
    if (len == 0) {  // BIG-REQUESTS: real length follows
      if (!big_requests || in.size() - offset < 8) break;
      U32 big;
      std::memcpy(&big, p + 4, 4);
      len = (size_t)big * 4;
      body = 8;
    }
    if (len < 4 || in.size() - offset < len) break;
    ++sequence;
    Reader r{p + body, p + len, data_byte, true};
    bool handled = major < 128 ? DispatchCore(*this, major, r)
                               : DispatchExtension(*this, major, data_byte, r, (const U8*)p, len);
    if (!handled) Error(1 /* Request */, 0, 0, major);
    offset += len;
  }
  in.erase(0, offset);

  for (Window* w : g_dirty) server.PublishWindow(*w);
  g_dirty.clear();
  g_current_client = nullptr;
  server.FlushAll();
}

bool DispatchExtension(Client& client, U8 major, U8 opcode, Reader& r, const U8* raw,
                       size_t raw_len) {
  switch (major) {
    case bigreq::kMajorOpcode:
      return bigreq::Dispatch(client, opcode, r);
    case shm::kMajorOpcode:
      return shm::Dispatch(client, opcode, r);
    case render::kMajorOpcode:
      return render::Dispatch(client, opcode, r);
    case dri3::kMajorOpcode:
      return dri3::Dispatch(client, opcode, r);
    case present::kMajorOpcode:
      return present::Dispatch(client, opcode, r);
    case xc_misc::kMajorOpcode:
      return xc_misc::Dispatch(client, opcode, r);
    case xkb::kMajorOpcode:
      return xkb::Dispatch(client, opcode, raw, raw_len);
    default:
      return false;
  }
}

void Client::Flush() {
  if (out.empty()) {
    out_fds.clear();
    return;
  }
  size_t sent_total = 0;
#if defined(_WIN32)
  // TCP carries no fds; the handlers never queue any on Windows.
  while (sent_total < out.size()) {
    int n = send((SOCKET)fd.fd, out.data() + sent_total, (int)(out.size() - sent_total), 0);
    if (n <= 0) break;
    sent_total += n;
  }
#else
  while (sent_total < out.size()) {
    msghdr msg{};
    iovec iov{out.data() + sent_total, out.size() - sent_total};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 16)];
    if (sent_total == 0 && !out_fds.empty()) {
      int count = std::min<int>(out_fds.size(), 16);
      msg.msg_control = control;
      msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
      cmsghdr* cm = CMSG_FIRSTHDR(&msg);
      cm->cmsg_level = SOL_SOCKET;
      cm->cmsg_type = SCM_RIGHTS;
      cm->cmsg_len = CMSG_LEN(sizeof(int) * count);
      for (int k = 0; k < count; ++k) {
        int f = out_fds[k].fd;
        std::memcpy(CMSG_DATA(cm) + k * sizeof(int), &f, sizeof(int));
      }
    }
    ssize_t n = sendmsg(fd.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n <= 0) break;
    sent_total += n;
  }
#endif
  out.erase(0, sent_total);
  out_fds.clear();
}

void Client::Disconnect() {
  Status status;
  server.epoll.Del(this, status);
  server.DestroyClientResources(*this);
  server.epoll.Post([this] { Client::colony.erase(Client::colony.get_iterator(this)); });
}

Client::~Client() {}

void Client::Error(U8 code, U32 bad_value, U16 minor, U8 major) {
  Writer wr(*this, 32);
  wr.Fixed<U8>(0);  // error
  wr.Fixed<U8>(code);
  wr.Fixed<U16>(sequence);
  wr.Fixed<U32>(bad_value);
  wr.Fixed<U16>(minor);
  wr.Fixed<U8>(major);
  wr.Skip(21);
}

bool Client::CheckNewId(U32 xid) {
  if ((xid & ~id_mask) != id_base) {
    Error(14 /* IDChoice */, xid);
    return false;
  }
  if (server.resources.count(xid)) {
    Error(14 /* IDChoice */, xid);
    return false;
  }
  return true;
}

// ============================================================================
//  Events
// ============================================================================

// Sends a core event to the one client that owns `w`, if it selected `mask`.
static void EventToOwner(Window& w, U32 mask, auto&& fill) {
  if (!w.owner) return;
  if (mask && !(w.event_mask & mask)) return;
  Client* saved = g_current_client;
  g_current_client = w.owner;
  fill(*w.owner);
  g_current_client = saved;
}

static U32 g_last_x = 0, g_last_y = 0;  // last pointer position in root space

// ============================================================================
//  Core requests (xproto.xml)
// ============================================================================

void CreateWindow::Handle(Client& client) {
  if (!client.CheckNewId(wid)) return;
  Window* parent = server->Get<Window>(this->parent, ResType::Window);
  if (!parent) {
    client.Error(3 /* Window */, this->parent);
    return;
  }
  Window& w = server->Add<Window>(wid, ResType::Window, client);
  w.parent = parent;
  w.x = x;
  w.y = y;
  w.border_width = border_width;
  w.win_class = class_ == 0 ? parent->win_class : class_;
  w.depth = depth ? depth : parent->depth;
  w.override_redirect = override_redirect.value_or(0) != 0;
  w.event_mask = event_mask.value_or(0);
  w.do_not_propagate = do_not_propogate_mask.value_or(0);
  if (background_pixel) {
    w.background_pixel = *background_pixel;
    w.has_background_pixel = true;
  }
  if (w.win_class != 2 /* InputOnly */) w.Ensure(width, height, w.depth);
  parent->children.push_back(&w);
  w.is_toplevel = (parent == server->root);
}

void ChangeWindowAttributes::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) {
    client.Error(3, window);
    return;
  }
  if (event_mask) w->event_mask = *event_mask;
  if (override_redirect) w->override_redirect = *override_redirect != 0;
  if (background_pixel) {
    w->background_pixel = *background_pixel;
    w->has_background_pixel = true;
  }
  if (do_not_propogate_mask) w->do_not_propagate = *do_not_propogate_mask;
}

void GetWindowAttributes::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) {
    client.Error(3, window);
    return;
  }
  GetWindowAttributesReply reply{};
  reply.backing_store = 0;
  reply.visual = w->depth >= 32 ? kVisual32 : kVisual24;
  reply.class_ = w->win_class;
  reply.map_state = w->mapped ? 2 : 0;
  reply.your_event_mask = w->event_mask;
  reply.all_event_masks = w->event_mask;
  reply.override_redirect = w->override_redirect;
  reply.colormap = kColormap;
  Reply(client, std::move(reply));
}

void ClearWindowArea(Window& w, int x, int y, int width, int height, bool exposures);

void DestroyWindow::Handle(Client& client) {
  if (server->Get<Window>(window, ResType::Window)) server->Free(window);
}
void DestroySubwindows::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) return;
  Vec<Window*> kids = w->children;
  for (Window* c : kids) server->Free(c->xid);
}

// A client that draws its own frame says so with _MOTIF_WM_HINTS (a decorations field of
// zero) or, for GTK client-side decorations, by setting _GTK_FRAME_EXTENTS.
static bool WantsClientDecorations(Window& w) {
  if (w.props.contains(server->InternAtom("_GTK_FRAME_EXTENTS", false))) return true;
  if (auto it = w.props.find(server->InternAtom("_MOTIF_WM_HINTS", false));
      it != w.props.end() && it->second.data.size() >= 12) {
    U32 flags, decorations;
    std::memcpy(&flags, it->second.data.data(), 4);
    std::memcpy(&decorations, it->second.data.data() + 8, 4);
    if ((flags & 2 /* MWM_HINTS_DECORATIONS */) && decorations == 0) return true;
  }
  return false;
}

static void UpdateClientDecorations(Window& w) {
  if (Ptr<X11Window> obj = w.object.Lock()) {
    {
      auto lock = std::lock_guard(obj->mutex);
      obj->client_decorated = WantsClientDecorations(w);
    }
    obj->WakeToys();
  }
}

void MapWindow::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w || w->mapped) return;
  w->mapped = true;
  if (w->has_background_pixel && w->surface)
    w->surface->getCanvas()->clear(PixelToColor(w->background_pixel, w->depth));
  U32 map_event = w->parent == server->root ? window : w->parent->xid;
  EventToOwner(*w, EventMaskStructureNotify, [&](Client& c) {
    MapNotify ev{};
    ev.event = map_event;
    ev.window = window;
    ev.override_redirect = w->override_redirect;
    ev.Send(c);
  });
  // A toolkit that waits for the window-manager's post-map ConfigureNotify (GTK does)
  // stalls its first paint without one; Automat is the WM here, so synthesize it.
  EventToOwner(*w, EventMaskStructureNotify, [&](Client& c) {
    ConfigureNotify ev{};
    ev.event = window;
    ev.window = window;
    ev.x = w->x;
    ev.y = w->y;
    ev.width = w->width;
    ev.height = w->height;
    ev.border_width = w->border_width;
    ev.Send(c);
  });
  EventToOwner(*w, EventMaskVisibilityChange, [&](Client& c) {
    VisibilityNotify ev{};
    ev.window = window;
    ev.state = 0;  // VisibilityUnobscured
    ev.Send(c);
  });
  // Ask the client to paint: send an Expose for the whole window.
  EventToOwner(*w, EventMaskExposure, [&](Client& c) {
    Expose ev{};
    ev.window = window;
    ev.width = w->width;
    ev.height = w->height;
    ev.Send(c);
  });
  if (w->is_toplevel) {
    // ICCCM/EWMH: a managed, viewable toplevel carries WM_STATE=Normal and zero frame
    // extents. GTK waits for these before it runs its first paint.
    U32 wm_state[2] = {1 /* NormalState */, 0 /* no icon window */};
    SetProp(*w, "WM_STATE", server->InternAtom("WM_STATE", false), 32, wm_state, sizeof(wm_state));
    U32 extents[4] = {0, 0, 0, 0};
    SetProp(*w, "_NET_FRAME_EXTENTS", server->InternAtom("CARDINAL", false), 32, extents,
            sizeof(extents));
    EventToOwner(*w, EventMaskPropertyChange, [&](Client& c) {
      PropertyNotify ev{};
      ev.window = window;
      ev.atom = server->InternAtom("WM_STATE", false);
      ev.Send(c);
    });
  }
  if (w->is_toplevel && !w->object.Lock()) {
    I64 pid = 0;
#if defined(_WIN32)
    pid = PeerPid(client.fd.fd);
#else
    ucred cred{};
    socklen_t len = sizeof(cred);
    if (getsockopt(client.fd.fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) pid = cred.pid;
#endif
    w->client_pid = pid;

    // A restored window's launch aims at its existing board object (its Location already
    // exists); a fresh one is queued for Tick to insert into a Board.
    Ptr<Launch> launch = Launch::Find(pid);
    Ptr<X11Window> obj = launch ? launch->LockRestoring<X11Window>() : nullptr;
    bool restored = (bool)obj;
    if (!obj) obj = MAKE_PTR(X11Window);
    w->object = obj->AcquireWeakPtr();  // weak; the Location (below, or the restored one) owns it
    obj->window_handle.store(w);
    {
      auto lock = std::lock_guard(obj->mutex);
      obj->override_redirect = w->override_redirect;
      obj->client_gone = false;
      obj->client_pid = pid;
      // Clients set their name properties before mapping, which predates this object: seed
      // the title (and WM_CLASS fallback) from the window's stored properties.
      auto PropStr = [&](const char* name) -> StrView {
        auto it = w->props.find(server->InternAtom(name, false));
        if (it == w->props.end()) return {};
        StrView v((const char*)it->second.data.data(), it->second.data.size());
        while (!v.empty() && v.back() == '\0') v.remove_suffix(1);
        return v;
      };
      StrView title = PropStr("_NET_WM_NAME");
      if (title.empty()) title = PropStr("WM_NAME");
      if (!title.empty()) obj->title = Str(title);
      StrView wm_class = PropStr("WM_CLASS");  // instance NUL class
      if (auto nul = wm_class.find('\0'); nul != StrView::npos)
        obj->app_id = Str(wm_class.substr(nul + 1));
      obj->client_decorated = WantsClientDecorations(*w);
      if (!restored && launch) {
        obj->recipe = launch->argv;
        obj->launched_by = launch;
      }
    }
    if (restored) {
      launch->RestoredInto(*obj);
    } else {
      auto lock = std::lock_guard(server->ui_mutex);
      server->ui_appeared.emplace_back(obj, launch);
    }
  }
  MarkDirty(w);
}
void MapSubwindows::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) return;
  for (Window* c : w->children) {
    MapWindow m{};
    m.sequence = sequence;
    m.window = c->xid;
    m.Handle(client);
  }
}

void UnmapWindow::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w || !w->mapped) return;
  w->mapped = false;
  EventToOwner(*w, EventMaskStructureNotify, [&](Client& c) {
    UnmapNotify ev{};
    ev.event = w->parent == server->root ? window : w->parent->xid;
    ev.window = window;
    ev.Send(c);
  });
  if (w->is_toplevel)
    UnmapAndForgetToplevel(*w);
  else
    MarkDirty(w);
}
void UnmapSubwindows::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) return;
  for (Window* c : w->children) {
    UnmapWindow m{};
    m.sequence = sequence;
    m.window = c->xid;
    m.Handle(client);
  }
}

void ConfigureWindow::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) {
    client.Error(3, window);
    return;
  }
  bool resized = false, moved = false;
  if (x) {
    w->x = (I16)*x;
    moved = true;
  }
  if (y) {
    w->y = (I16)*y;
    moved = true;
  }
  int nw = w->width, nh = w->height;
  if (width) {
    nw = *width;
    resized = true;
  }
  if (height) {
    nh = *height;
    resized = true;
  }
  if (border_width) w->border_width = *border_width;
  if (resized && (nw != w->width || nh != w->height) && w->win_class != 2) {
    // Preserve existing content in the top-left corner.
    sk_sp<SkImage> old = w->surface ? w->surface->makeImageSnapshot() : nullptr;
    w->Ensure(nw, nh, w->depth);
    if (old && w->surface) w->surface->getCanvas()->drawImage(old, 0, 0);
  } else if (resized) {
    w->width = nw;
    w->height = nh;
  }
  EventToOwner(*w, EventMaskStructureNotify, [&](Client& c) {
    ConfigureNotify ev{};
    ev.event = window;
    ev.window = window;
    ev.x = w->x;
    ev.y = w->y;
    ev.width = w->width;
    ev.height = w->height;
    ev.border_width = w->border_width;
    ev.Send(c);
  });
  MarkDirty(w);
}

void GetGeometry::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  if (!d) {
    client.Error(9 /* Drawable */, drawable);
    return;
  }
  GetGeometryReply reply{};
  reply.depth = d->depth;
  reply.root = kRoot;
  if (d->type == ResType::Window) {
    Window* w = static_cast<Window*>(d);
    reply.x = w->x;
    reply.y = w->y;
    reply.border_width = w->border_width;
  }
  reply.width = d->width;
  reply.height = d->height;
  Reply(client, std::move(reply));
}

void QueryTree::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  QueryTreeReply reply{};
  reply.root = kRoot;
  if (w) {
    reply.parent = w->parent ? w->parent->xid : 0;
    static Vec<U32> ids;
    ids.clear();
    for (Window* c : w->children) ids.push_back(c->xid);
    reply.children = Span<const U32>(ids.data(), ids.size());
    reply.children_len = (U16)ids.size();
  }
  Reply(client, std::move(reply));
}

void InternAtom::Handle(Client& client) {
  InternAtomReply reply{};
  reply.atom = server->InternAtom(name, only_if_exists != 0);
  Reply(client, std::move(reply));
}
void GetAtomName::Handle(Client& client) {
  StrView n = server->AtomName(atom);
  if (n.empty() && atom != 0) {
    client.Error(5 /* Atom */, atom);
    return;
  }
  GetAtomNameReply reply{};
  reply.name = n;
  reply.name_len = (U16)n.size();
  Reply(client, std::move(reply));
}

void ChangeProperty::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) {
    client.Error(3, window);
    return;
  }
  int unit = format / 8;
  auto& prop = w->props[property];
  Span<const U8> bytes = data;
  if (mode == 0 /* Replace */) {
    prop.data.assign(bytes.begin(), bytes.end());
  } else {  // Prepend/Append
    size_t at = mode == 2 ? 0 : prop.data.size();
    prop.data.insert(prop.data.begin() + at, bytes.begin(), bytes.end());
  }
  prop.type = type;
  prop.format = format;
  (void)unit;
  EventToOwner(*w, EventMaskPropertyChange, [&](Client& c) {
    PropertyNotify ev{};
    ev.window = window;
    ev.atom = property;
    ev.state = 0;  // NewValue
    ev.Send(c);
  });
  if (property == server->InternAtom("WM_NAME", false) ||
      property == server->InternAtom("_NET_WM_NAME", false)) {
    if (Ptr<X11Window> obj = w->object.Lock()) {
      {
        auto lock = std::lock_guard(obj->mutex);
        obj->title = Str((const char*)prop.data.data(), prop.data.size());
      }
      obj->WakeToys();
    }
  }
  if (property == server->InternAtom("_MOTIF_WM_HINTS", false) ||
      property == server->InternAtom("_GTK_FRAME_EXTENTS", false)) {
    UpdateClientDecorations(*w);
  }
}
void DeleteProperty::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) return;
  w->props.erase(property);
  if (property == server->InternAtom("_MOTIF_WM_HINTS", false) ||
      property == server->InternAtom("_GTK_FRAME_EXTENTS", false)) {
    UpdateClientDecorations(*w);
  }
}
void GetProperty::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) {
    client.Error(3, window);
    return;
  }
  GetPropertyReply reply{};
  auto it = w->props.find(property);
  if (it != w->props.end()) {
    auto& prop = it->second;
    reply.format = prop.format;
    reply.type = prop.type;
    int unit = prop.format / 8;
    U32 total = unit ? (U32)prop.data.size() / unit : 0;
    U32 off = long_offset * 4;
    U32 want = std::min<U32>(long_length * 4, off < prop.data.size() ? prop.data.size() - off : 0);
    if (delete_ && off + want >= prop.data.size()) {
      // deletion handled after copying
    }
    reply.value = Span<const U8>(prop.data.data() + std::min<size_t>(off, prop.data.size()), want);
    reply.value_len = unit ? want / unit : 0;
    reply.bytes_after = (U32)prop.data.size() - std::min<U32>(off + want, prop.data.size());
    Reply(client, std::move(reply));
    if (delete_ && reply.bytes_after == 0) w->props.erase(property);
    (void)total;
    return;
  }
  Reply(client, std::move(reply));
}
void ListProperties::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  ListPropertiesReply reply{};
  static Vec<U32> atoms;
  atoms.clear();
  if (w)
    for (auto& [atom, prop] : w->props) atoms.push_back(atom);
  reply.atoms = Span<const U32>(atoms.data(), atoms.size());
  reply.atoms_len = (U16)atoms.size();
  Reply(client, std::move(reply));
}

// ---- graphics contexts -------------------------------------------------------------

static void ApplyGC(GContext& gc, auto& m) {
  if (m.foreground) gc.foreground = *m.foreground;
  if (m.line_width) gc.line_width = *m.line_width;
  if (m.clip_x_origin) gc.clip_x_origin = *m.clip_x_origin;
  if (m.clip_y_origin) gc.clip_y_origin = *m.clip_y_origin;
  if (m.clip_mask) gc.clip_rects.clear();  // a pixmap clip mask is treated as unclipped
}

void CreateGC::Handle(Client& client) {
  if (!client.CheckNewId(cid)) return;
  GContext& gc = server->Add<GContext>(cid, ResType::GContext, client);
  ApplyGC(gc, *this);
}
void ChangeGC::Handle(Client& client) {
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!gcon) {
    client.Error(13 /* GContext */, this->gc);
    return;
  }
  ApplyGC(*gcon, *this);
}
void CopyGC::Handle(Client& client) {
  GContext* src = server->Get<GContext>(src_gc, ResType::GContext);
  GContext* dst = server->Get<GContext>(dst_gc, ResType::GContext);
  if (src && dst) {
    U32 saved_xid = dst->xid;
    Client* saved_owner = dst->owner;
    *dst = *src;
    dst->xid = saved_xid;
    dst->owner = saved_owner;
  }
}
void FreeGC::Handle(Client& client) { server->Free(this->gc); }
void SetClipRectangles::Handle(Client& client) {
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!gcon) return;
  gcon->clip_rects.clear();
  gcon->clip_x_origin = clip_x_origin;
  gcon->clip_y_origin = clip_y_origin;
  for (auto& r : rectangles)
    gcon->clip_rects.push_back(
        SkIRect::MakeXYWH(clip_x_origin + r.x, clip_y_origin + r.y, r.width, r.height));
}
void SetDashes::Handle(Client& client) {}

// Sets up a canvas clip for a GC's clip rectangles.
static void ApplyClip(SkCanvas& canvas, GContext& gc) {
  if (gc.clip_rects.empty()) return;
  SkRegion region;
  for (auto& r : gc.clip_rects) region.op(r, SkRegion::kUnion_Op);
  canvas.clipRegion(region);
}

// ---- pixmaps -----------------------------------------------------------------------

void CreatePixmap::Handle(Client& client) {
  if (!client.CheckNewId(pid)) return;
  Pixmap& p = server->Add<Pixmap>(pid, ResType::Pixmap, client);
  p.Ensure(width, height, depth);
}
void FreePixmap::Handle(Client& client) { server->Free(pixmap); }

// ---- core drawing ------------------------------------------------------------------

void PolyFillRectangle::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!d || !gcon || !d->Canvas()) return;
  SkCanvas& canvas = *d->Canvas();
  canvas.save();
  ApplyClip(canvas, *gcon);
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  for (auto& r : rectangles) canvas.drawRect(SkRect::MakeXYWH(r.x, r.y, r.width, r.height), paint);
  canvas.restore();
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void PolyRectangle::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!d || !gcon || !d->Canvas()) return;
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(gcon->line_width ? gcon->line_width : 1);
  for (auto& r : rectangles)
    d->Canvas()->drawRect(SkRect::MakeXYWH(r.x, r.y, r.width, r.height), paint);
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void PolyPoint::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!d || !gcon || !d->Canvas()) return;
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  int px = 0, py = 0;
  for (auto& pt : points) {
    px = coordinate_mode && !(&pt == &points.front()) ? px + pt.x : pt.x;
    py = coordinate_mode && !(&pt == &points.front()) ? py + pt.y : pt.y;
    d->Canvas()->drawPoint(px, py, paint);
  }
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
static void DrawPolyline(Drawable* d, GContext* gcon, Span<const POINT> points, U8 mode,
                         bool close) {
  if (!d || !gcon || !d->Canvas() || points.empty()) return;
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(gcon->line_width ? gcon->line_width : 1);
  SkPathBuilder path;
  int px = points[0].x, py = points[0].y;
  path.moveTo(px, py);
  for (size_t i = 1; i < points.size(); ++i) {
    if (mode) {
      px += points[i].x;
      py += points[i].y;
    } else {
      px = points[i].x;
      py = points[i].y;
    }
    path.lineTo(px, py);
  }
  if (close) path.close();
  d->Canvas()->drawPath(path.detach(), paint);
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void PolyLine::Handle(Client& client) {
  DrawPolyline(server->GetDrawable(drawable), server->Get<GContext>(this->gc, ResType::GContext),
               points, coordinate_mode, false);
}
void PolySegment::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!d || !gcon || !d->Canvas()) return;
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(gcon->line_width ? gcon->line_width : 1);
  for (auto& s : segments) d->Canvas()->drawLine(s.x1, s.y1, s.x2, s.y2, paint);
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void FillPoly::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!d || !gcon || !d->Canvas() || points.empty()) return;
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  SkPathBuilder path;
  int px = points[0].x, py = points[0].y;
  path.moveTo(px, py);
  for (size_t i = 1; i < points.size(); ++i) {
    if (coordinate_mode) {
      px += points[i].x;
      py += points[i].y;
    } else {
      px = points[i].x;
      py = points[i].y;
    }
    path.lineTo(px, py);
  }
  path.close();
  d->Canvas()->drawPath(path.detach(), paint);
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void PolyArc::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!d || !gcon || !d->Canvas()) return;
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(gcon->line_width ? gcon->line_width : 1);
  paint.setAntiAlias(true);
  for (auto& a : arcs) {
    SkRect oval = SkRect::MakeXYWH(a.x, a.y, a.width, a.height);
    d->Canvas()->drawArc(oval, -a.angle1 / 64.0f, -a.angle2 / 64.0f, false, paint);
  }
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void PolyFillArc::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  GContext* gcon = server->Get<GContext>(this->gc, ResType::GContext);
  if (!d || !gcon || !d->Canvas()) return;
  SkPaint paint;
  paint.setColor(PixelToColor(gcon->foreground, d->depth));
  paint.setAntiAlias(true);
  for (auto& a : arcs) {
    SkRect oval = SkRect::MakeXYWH(a.x, a.y, a.width, a.height);
    d->Canvas()->drawArc(oval, -a.angle1 / 64.0f, -a.angle2 / 64.0f, true, paint);
  }
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}

void PutImage::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  if (!d || !d->surface) return;
  if (format != 2 /* ZPixmap */) return;  // Automat's visuals are all Z-format
  int bpp = depth >= 24 ? 4 : (depth > 8 ? 2 : 1);
  size_t stride = ((size_t)width * bpp + 3) & ~size_t(3);
  if (data.size() < stride * height) return;
  SkImageInfo info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                                       depth >= 32 ? kUnpremul_SkAlphaType : kOpaque_SkAlphaType);
  if (bpp == 4) {
    SkPixmap pm(info, data.data(), stride);
    d->surface->writePixels(pm, dst_x, dst_y);
  }
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void GetImage::Handle(Client& client) {
  Drawable* d = server->GetDrawable(drawable);
  if (!d || !d->surface) {
    client.Error(9, drawable);
    return;
  }
  GetImageReply reply{};
  reply.depth = d->depth;
  reply.visual = 0;
  size_t stride = (size_t)width * 4;
  static Vec<U8> pixels;
  pixels.assign(stride * height, 0);
  SkImageInfo info =
      SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kUnpremul_SkAlphaType);
  SkPixmap pm(info, pixels.data(), stride);
  d->surface->readPixels(pm, x, y);
  reply.data = Span<const U8>(pixels.data(), pixels.size());
  Reply(client, std::move(reply));
}

void CopyDrawable(Drawable* src, Drawable* dst, int sx, int sy, int w, int h, int dx, int dy) {
  if (!src || !dst || !src->surface || !dst->Canvas()) return;
  sk_sp<SkImage> img = src->surface->makeImageSnapshot();
  dst->Canvas()->drawImageRect(img, SkRect::MakeXYWH(sx, sy, w, h), SkRect::MakeXYWH(dx, dy, w, h),
                               SkSamplingOptions(), nullptr, SkCanvas::kStrict_SrcRectConstraint);
}
void CopyArea::Handle(Client& client) {
  Drawable* s = server->GetDrawable(src_drawable);
  Drawable* d = server->GetDrawable(dst_drawable);
  CopyDrawable(s, d, src_x, src_y, width, height, dst_x, dst_y);
  if (d && d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}
void CopyPlane::Handle(Client& client) {
  Drawable* s = server->GetDrawable(src_drawable);
  Drawable* d = server->GetDrawable(dst_drawable);
  CopyDrawable(s, d, src_x, src_y, width, height, dst_x, dst_y);
  if (d && d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
}

void ClearWindowArea(Window& w, int x, int y, int width, int height, bool exposures) {
  if (w.surface) {
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setColor(w.has_background_pixel ? PixelToColor(w.background_pixel, w.depth)
                                          : SK_ColorTRANSPARENT);
    int cw = width ? width : w.width - x;
    int ch = height ? height : w.height - y;
    w.surface->getCanvas()->drawRect(SkRect::MakeXYWH(x, y, cw, ch), paint);
  }
  if (exposures) {
    EventToOwner(w, EventMaskExposure, [&](Client& c) {
      Expose ev{};
      ev.window = w.xid;
      ev.x = x;
      ev.y = y;
      ev.width = width ? width : w.width - x;
      ev.height = height ? height : w.height - y;
      ev.Send(c);
    });
  }
}
void ClearArea::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  if (!w) return;
  ClearWindowArea(*w, x, y, width, height, exposures != 0);
  MarkDirty(w);
}

void ReparentWindow::Handle(Client& client) {
  Window* w = server->Get<Window>(window, ResType::Window);
  Window* p = server->Get<Window>(parent, ResType::Window);
  if (!w || !p) return;
  if (w->parent) std::erase(w->parent->children, w);
  w->parent = p;
  w->x = x;
  w->y = y;
  p->children.push_back(w);
  bool was_top = w->is_toplevel;
  w->is_toplevel = (p == server->root);
  if (was_top && !w->is_toplevel) UnmapAndForgetToplevel(*w);
}

// ---- extensions query --------------------------------------------------------------

struct ExtEntry {
  const char* name;
  U8 major, first_event, first_error;
};
static const ExtEntry kExtensions[] = {
    {"BIG-REQUESTS", bigreq::kMajorOpcode, 0, 0},
#if !defined(_WIN32)
    // Both need shared memory / fd passing, which the TCP transport cannot carry.
    {"MIT-SHM", shm::kMajorOpcode, shm::kFirstEvent, shm::kFirstError},
#endif
    {"RENDER", render::kMajorOpcode, 0, render::kFirstError},
#if !defined(_WIN32)
    {"DRI3", dri3::kMajorOpcode, 0, 0},
#endif
    {"Present", present::kMajorOpcode, 0, 0},
    {"XC-MISC", xc_misc::kMajorOpcode, 0, 0},
    {"XKEYBOARD", xkb::kMajorOpcode, xkb::kFirstEvent, xkb::kFirstError},
};
void QueryExtension::Handle(Client& client) {
  QueryExtensionReply reply{};
  for (auto& e : kExtensions) {
    if (name.size() == strlen(e.name) && memcmp(name.data(), e.name, name.size()) == 0) {
      reply.present = 1;
      reply.major_opcode = e.major;
      reply.first_event = e.first_event;
      reply.first_error = e.first_error;
      break;
    }
  }
  Reply(client, std::move(reply));
}
void ListExtensions::Handle(Client& client) {
  ListExtensionsReply reply{};
  static Vec<StrView> names;
  names.clear();
  for (auto& e : kExtensions) names.push_back(e.name);
  reply.names = Span<const StrView>(names.data(), names.size());
  reply.names_len = (U8)names.size();
  Reply(client, std::move(reply));
}

// ---- keyboard / pointer info -------------------------------------------------------

void GetKeyboardMapping::Handle(Client& client) {
  GetKeyboardMappingReply reply{};
  reply.keysyms_per_keycode = (U8)server->keysyms_per_code;
  int lo = std::max<int>(first_keycode, server->keymap_min);
  int hi = std::min<int>(first_keycode + count, server->keymap_max + 1);
  static Vec<U32> out;
  out.clear();
  for (int kc = first_keycode; kc < first_keycode + count; ++kc) {
    for (int i = 0; i < server->keysyms_per_code; ++i) {
      int idx = (kc - server->keymap_min) * server->keysyms_per_code + i;
      out.push_back(kc >= lo && kc < hi && idx >= 0 && idx < (int)server->keysyms.size()
                        ? server->keysyms[idx]
                        : 0);
    }
  }
  reply.keysyms = Span<const U32>(out.data(), out.size());
  Reply(client, std::move(reply));
}
void GetModifierMapping::Handle(Client& client) {
  GetModifierMappingReply reply{};
  reply.keycodes_per_modifier = 4;
  static Vec<U8> codes;
  codes.assign(32, 0);
  for (int m = 0; m < 8; ++m)
    for (int k = 0; k < 4; ++k) codes[m * 4 + k] = server->mod_map[m][k];
  reply.keycodes = Span<const U8>(codes.data(), codes.size());
  Reply(client, std::move(reply));
}
void GetKeyboardControl::Handle(Client& client) {
  GetKeyboardControlReply reply{};
  reply.global_auto_repeat = 1;
  reply.key_click_percent = 0;
  reply.bell_percent = 0;
  Reply(client, std::move(reply));
}
void GetPointerControl::Handle(Client& client) {
  GetPointerControlReply reply{};
  reply.acceleration_numerator = 2;
  reply.acceleration_denominator = 1;
  reply.threshold = 4;
  Reply(client, std::move(reply));
}
void GetPointerMapping::Handle(Client& client) {
  GetPointerMappingReply reply{};
  static U8 map[] = {1, 2, 3, 4, 5};
  reply.map = Span<const U8>(map, 5);
  reply.map_len = 5;
  Reply(client, std::move(reply));
}
void SetPointerMapping::Handle(Client& client) {
  SetPointerMappingReply reply{};
  Reply(client, std::move(reply));
}
void SetModifierMapping::Handle(Client& client) {
  SetModifierMappingReply reply{};
  Reply(client, std::move(reply));
}
void QueryKeymap::Handle(Client& client) {
  QueryKeymapReply reply{};
  Reply(client, std::move(reply));
}
void ChangeKeyboardMapping::Handle(Client& client) {}
void ChangeKeyboardControl::Handle(Client& client) {}
void ChangePointerControl::Handle(Client& client) {}
void Bell::Handle(Client& client) {}

// ---- input focus, grabs, pointer ---------------------------------------------------

void SetInputFocus::Handle(Client& client) {}
void GetInputFocus::Handle(Client& client) {
  GetInputFocusReply reply{};
  reply.revert_to = 0;
  reply.focus = 0;
  if (auto obj = server->keyboard_focus.Lock()) {
    if (auto* w = (Window*)obj->window_handle.load()) reply.focus = w->xid;
  }
  Reply(client, std::move(reply));
}
void GrabPointer::Handle(Client& client) {
  GrabPointerReply reply{};
  reply.status = 0;  // Success
  Reply(client, std::move(reply));
}
void UngrabPointer::Handle(Client& client) {}
void GrabButton::Handle(Client& client) {}
void UngrabButton::Handle(Client& client) {}
void ChangeActivePointerGrab::Handle(Client& client) {}
void GrabKeyboard::Handle(Client& client) {
  GrabKeyboardReply reply{};
  reply.status = 0;
  Reply(client, std::move(reply));
}
void UngrabKeyboard::Handle(Client& client) {}
void GrabKey::Handle(Client& client) {}
void UngrabKey::Handle(Client& client) {}
void AllowEvents::Handle(Client& client) {}
void GrabServer::Handle(Client& client) {}
void UngrabServer::Handle(Client& client) {}
void QueryPointer::Handle(Client& client) {
  QueryPointerReply reply{};
  reply.same_screen = 1;
  reply.root = kRoot;
  reply.root_x = (I16)g_last_x;
  reply.root_y = (I16)g_last_y;
  reply.win_x = (I16)g_last_x;
  reply.win_y = (I16)g_last_y;
  Reply(client, std::move(reply));
}
void GetMotionEvents::Handle(Client& client) {
  GetMotionEventsReply reply{};
  Reply(client, std::move(reply));
}
void TranslateCoordinates::Handle(Client& client) {
  TranslateCoordinatesReply reply{};
  reply.same_screen = 1;
  reply.dst_x = src_x;
  reply.dst_y = src_y;
  Reply(client, std::move(reply));
}
void WarpPointer::Handle(Client& client) {}

// ---- selections (clipboard) --------------------------------------------------------

void SetSelectionOwner::Handle(Client& client) {
  if (owner)
    server->selection_owner[selection] = owner;
  else
    server->selection_owner.erase(selection);
}
void GetSelectionOwner::Handle(Client& client) {
  GetSelectionOwnerReply reply{};
  auto it = server->selection_owner.find(selection);
  reply.owner = it != server->selection_owner.end() ? it->second : 0;
  Reply(client, std::move(reply));
}
void ConvertSelection::Handle(Client& client) {
  auto it = server->selection_owner.find(selection);
  if (it == server->selection_owner.end()) return;
  Window* owner = server->Get<Window>(it->second, ResType::Window);
  if (!owner || !owner->owner) return;
  Client* saved = g_current_client;
  g_current_client = owner->owner;
  SelectionRequest ev{};
  ev.time = time;
  ev.owner = it->second;
  ev.requestor = requestor;
  ev.selection = selection;
  ev.target = target;
  ev.property = property;
  ev.Send(*owner->owner);
  g_current_client = saved;
}

void SendEvent::Handle(Client& client) {
  Window* w = server->Get<Window>(destination, ResType::Window);
  if (!w) return;
  if (w == server->root && (event[0] & 0x7f) == 33) {
    U32 type, target, direction;
    std::memcpy(&target, event + 4, 4);
    std::memcpy(&type, event + 8, 4);
    std::memcpy(&direction, event + 12 + 8, 4);
    constexpr U32 kNetWmMoveResizeMove = 8;
    if (type == server->InternAtom("_NET_WM_MOVERESIZE", false) &&
        direction == kNetWmMoveResizeMove) {
      if (Window* tw = server->Get<Window>(target, ResType::Window)) {
        {
          auto lock = std::lock_guard(server->ui_mutex);
          server->ui_move_requests.push_back(tw->object);
        }
        vm.WakeToys();
      }
    }
    return;
  }
  if (!w->owner) return;
  Client* saved = g_current_client;
  g_current_client = w->owner;
  Writer wr(*w->owner, 32);
  size_t at = wr.pos;
  wr.Bytes(event, 32);
  // The server stamps the delivered event with the receiving client's current sequence
  // number (the client's own value in the event is meaningless). Without this, xcb's
  // sequence-widening sees a bogus jump and declares the connection inconsistent.
  // KeymapNotify (11) is the one event with no sequence field.
  if ((event[0] & 0x7f) != 11) {
    U16 seq = w->owner->sequence;
    std::memcpy(w->owner->out.data() + at + 2, &seq, 2);
  }
  g_current_client = saved;
}

// ---- fonts, colors, cursors (minimal) ----------------------------------------------

void OpenFont::Handle(Client& client) {
  if (client.CheckNewId(fid)) server->Add<Font>(fid, ResType::Font, client);
}
void CloseFont::Handle(Client& client) { server->Free(font); }
void QueryFont::Handle(Client& client) {
  QueryFontReply reply{};
  Reply(client, std::move(reply));
}
void QueryTextExtents::Handle(Client& client) {
  QueryTextExtentsReply reply{};
  Reply(client, std::move(reply));
}
void ListFonts::Handle(Client& client) {
  ListFontsReply reply{};
  Reply(client, std::move(reply));
}
void ListFontsWithInfo::Handle(Client& client) {
  ListFontsWithInfoReply reply{};  // a zero last_reply terminates the sequence
  Reply(client, std::move(reply));
}
void SetFontPath::Handle(Client& client) {}
void GetFontPath::Handle(Client& client) {
  GetFontPathReply reply{};
  Reply(client, std::move(reply));
}
void ImageText8::Handle(Client& client) {}
void ImageText16::Handle(Client& client) {}
void PolyText8::Handle(Client& client) {}
void PolyText16::Handle(Client& client) {}

void CreateColormap::Handle(Client& client) {
  if (client.CheckNewId(mid)) server->Add<Colormap>(mid, ResType::Colormap, client);
}
void FreeColormap::Handle(Client& client) { server->Free(cmap); }
void CopyColormapAndFree::Handle(Client& client) {
  if (client.CheckNewId(mid)) server->Add<Colormap>(mid, ResType::Colormap, client);
}
void InstallColormap::Handle(Client& client) {}
void UninstallColormap::Handle(Client& client) {}
void ListInstalledColormaps::Handle(Client& client) {
  ListInstalledColormapsReply reply{};
  static U32 cm = kColormap;
  reply.cmaps = Span<const U32>(&cm, 1);
  reply.cmaps_len = 1;
  Reply(client, std::move(reply));
}
void AllocColor::Handle(Client& client) {
  AllocColorReply reply{};
  reply.red = red;
  reply.green = green;
  reply.blue = blue;
  reply.pixel = 0xff000000u | ((red >> 8) << 16) | ((green >> 8) << 8) | (blue >> 8);
  Reply(client, std::move(reply));
}
void AllocNamedColor::Handle(Client& client) {
  AllocNamedColorReply reply{};
  Reply(client, std::move(reply));
}
void AllocColorCells::Handle(Client& client) {
  AllocColorCellsReply reply{};
  Reply(client, std::move(reply));
}
void AllocColorPlanes::Handle(Client& client) {
  AllocColorPlanesReply reply{};
  Reply(client, std::move(reply));
}
void FreeColors::Handle(Client& client) {}
void StoreColors::Handle(Client& client) {}
void StoreNamedColor::Handle(Client& client) {}
void QueryColors::Handle(Client& client) {
  // TrueColor: the pixel value directly encodes the channels, so echo them back.
  QueryColorsReply reply{};
  static Vec<RGB> out;
  out.clear();
  for (U32 px : pixels) {
    RGB c{};
    c.red = (U16)(((px >> 16) & 0xff) * 257);
    c.green = (U16)(((px >> 8) & 0xff) * 257);
    c.blue = (U16)((px & 0xff) * 257);
    out.push_back(c);
  }
  reply.colors_len = (U16)out.size();
  reply.colors = Span<const RGB>(out.data(), out.size());
  Reply(client, std::move(reply));
}
void LookupColor::Handle(Client& client) {
  LookupColorReply reply{};
  Reply(client, std::move(reply));
}
void CreateCursor::Handle(Client& client) {
  if (client.CheckNewId(cid)) server->Add<Cursor>(cid, ResType::Cursor, client);
}
void CreateGlyphCursor::Handle(Client& client) {
  if (client.CheckNewId(cid)) server->Add<Cursor>(cid, ResType::Cursor, client);
}
void FreeCursor::Handle(Client& client) { server->Free(cursor); }
void RecolorCursor::Handle(Client& client) {}
void QueryBestSize::Handle(Client& client) {
  QueryBestSizeReply reply{};
  reply.width = width;
  reply.height = height;
  Reply(client, std::move(reply));
}

// ---- misc --------------------------------------------------------------------------

void ChangeSaveSet::Handle(Client& client) {}
void CirculateWindow::Handle(Client& client) {}
void ChangeHosts::Handle(Client& client) {}
void ListHosts::Handle(Client& client) {
  // ListHosts has a variable reply the generator leaves manual; the empty form is 32 bytes.
  Writer wr(client, 32);
  wr.Fixed<U8>(1);
  wr.Fixed<U8>(1);  // mode: enabled
  wr.Fixed<U16>(sequence);
  wr.Fixed<U32>(0);
  wr.Fixed<U16>(0);  // number of hosts
  wr.Skip(22);
}
void SetAccessControl::Handle(Client& client) {}
void SetCloseDownMode::Handle(Client& client) {}
void KillClient::Handle(Client& client) {}
void RotateProperties::Handle(Client& client) {}
void ForceScreenSaver::Handle(Client& client) {}
void SetScreenSaver::Handle(Client& client) {}
void GetScreenSaver::Handle(Client& client) {
  GetScreenSaverReply reply{};
  Reply(client, std::move(reply));
}
void NoOperation::Handle(Client& client) {}

// ============================================================================
//  BIG-REQUESTS, XC-MISC
// ============================================================================

namespace bigreq {
void Enable::Handle(Client& client) {
  client.big_requests = true;
  EnableReply reply{};
  reply.maximum_request_length = 0x400000;  // 16 MiB in 4-byte units cap
  Reply(client, std::move(reply));
}
}  // namespace bigreq

namespace xc_misc {
void GetVersion::Handle(Client& client) {
  GetVersionReply reply{};
  reply.server_major_version = 1;
  reply.server_minor_version = 1;
  Reply(client, std::move(reply));
}
void GetXIDRange::Handle(Client& client) {
  // Clients that exhaust their id range ask for more; hand back a slice of the owner's.
  GetXIDRangeReply reply{};
  reply.start_id = client.id_base | 0x100000;
  reply.count = 0xffff;
  Reply(client, std::move(reply));
}
void GetXIDList::Handle(Client& client) {
  GetXIDListReply reply{};
  static Vec<U32> ids;
  ids.clear();
  for (U32 i = 0; i < count; ++i) ids.push_back(client.id_base | (0x100000 + i));
  reply.ids = Span<const U32>(ids.data(), ids.size());
  reply.ids_len = (U32)ids.size();
  Reply(client, std::move(reply));
}
}  // namespace xc_misc

// ============================================================================
//  MIT-SHM
// ============================================================================

namespace shm {
void QueryVersion::Handle(Client& client) {
  QueryVersionReply reply{};
  reply.shared_pixmaps = 1;
  reply.major_version = 1;
  reply.minor_version = 2;
#if !defined(_WIN32)
  reply.uid = getuid();
  reply.gid = getgid();
#endif
  reply.pixmap_format = 2;  // ZPixmap
  Reply(client, std::move(reply));
}
void Attach::Handle(Client& client) {
#if defined(_WIN32)
  // MIT-SHM is not advertised on Windows; register the id so Detach stays harmless.
  server->Add<ShmSeg>(shmseg, ResType::ShmSeg, client);
#else
  // Classic MIT-SHM: the client allocated a SysV segment (shmget) and asks the server to
  // attach it by id. This is what libXext's XShmAttach uses; GTK renders through it.
  ShmSeg& seg = server->Add<ShmSeg>(shmseg, ResType::ShmSeg, client);
  void* addr = shmat(shmid, nullptr, SHM_RDONLY);
  if (addr != (void*)-1) {
    seg.addr = addr;
    seg.sysv = true;
    struct shmid_ds ds;
    if (shmctl(shmid, IPC_STAT, &ds) == 0) seg.size = ds.shm_segsz;
  }
#endif
}
void AttachFd::Handle(Client& client) {
#if defined(_WIN32)
  server->Add<ShmSeg>(shmseg, ResType::ShmSeg, client);
#else
  ShmSeg& seg = server->Add<ShmSeg>(shmseg, ResType::ShmSeg, client);
  seg.fd = std::move(shm_fd);
  struct stat st;
  if (fstat(seg.fd.fd, &st) == 0) {
    seg.size = st.st_size;
    seg.addr = mmap(nullptr, seg.size, PROT_READ, MAP_SHARED, seg.fd.fd, 0);
    if (seg.addr == MAP_FAILED) seg.addr = nullptr;
  }
#endif
}
void CreateSegment::Handle(Client& client) {
#if defined(_WIN32)
  server->Add<ShmSeg>(shmseg, ResType::ShmSeg, client);
  CreateSegmentReply reply{};  // nfd = 0: fds cannot travel over TCP
  Reply(client, std::move(reply));
#else
  // The client asks the server to allocate the shared memory and hand back an fd.
  ShmSeg& seg = server->Add<ShmSeg>(shmseg, ResType::ShmSeg, client);
  int fd = memfd_create("automat-x11-shm", MFD_CLOEXEC);
  CreateSegmentReply reply{};
  if (fd >= 0 && ftruncate(fd, size) == 0) {
    seg.size = size;
    seg.addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (seg.addr == MAP_FAILED) seg.addr = nullptr;
    seg.fd = FD(fcntl(fd, F_DUPFD_CLOEXEC, 0));
    reply.nfd = 1;
    reply.shm_fd = FD(fd);
  } else {
    if (fd >= 0) close(fd);
  }
  Reply(client, std::move(reply));
#endif
}
void Detach::Handle(Client& client) { server->Free(shmseg); }
void PutImage::Handle(Client& client) {
  ShmSeg* seg = server->Get<ShmSeg>(shmseg, ResType::ShmSeg);
  Drawable* d = server->GetDrawable(drawable);
  if (!seg || !seg->addr || !d || !d->surface) return;
  if (format != 2 /* ZPixmap */) return;
  size_t stride = (size_t)total_width * 4;
  if (offset + stride * total_height > seg->size) return;
  const U8* base = (const U8*)seg->addr + offset;
  SkImageInfo info = SkImageInfo::Make(total_width, total_height, kBGRA_8888_SkColorType,
                                       depth >= 32 ? kUnpremul_SkAlphaType : kOpaque_SkAlphaType);
  SkPixmap full(info, base, stride);
  SkPixmap sub;
  if (full.extractSubset(&sub, SkIRect::MakeXYWH(src_x, src_y, src_width, src_height)))
    d->surface->writePixels(sub, dst_x, dst_y);
  if (d->type == ResType::Window) MarkDirty(static_cast<Window*>(d));
  if (send_event) {
    Completion ev{};
    ev.drawable = drawable;
    ev.shmseg = shmseg;
    ev.Send(client);
  }
}
void GetImage::Handle(Client& client) {
  ShmSeg* seg = server->Get<ShmSeg>(shmseg, ResType::ShmSeg);
  Drawable* d = server->GetDrawable(drawable);
  GetImageReply reply{};
  if (seg && seg->addr && d && d->surface) {
    reply.depth = d->depth;
    reply.size = width * height * 4;
    // The segment is mapped read-only for us; SHM GetImage is uncommon, so ignore.
  }
  Reply(client, std::move(reply));
}
void CreatePixmap::Handle(Client& client) {
  if (!client.CheckNewId(pid)) return;
  Pixmap& p = server->Add<Pixmap>(pid, ResType::Pixmap, client);
  p.Ensure(width, height, depth);
  ShmSeg* seg = server->Get<ShmSeg>(shmseg, ResType::ShmSeg);
  if (seg && seg->addr && p.surface) {
    size_t stride = (size_t)width * 4;
    if (offset + stride * height <= seg->size) {
      SkImageInfo info =
          SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                            depth >= 32 ? kUnpremul_SkAlphaType : kOpaque_SkAlphaType);
      SkPixmap pm(info, (const U8*)seg->addr + offset, stride);
      p.surface->writePixels(pm, 0, 0);
    }
  }
}
}  // namespace shm

// ============================================================================
//  RENDER
// ============================================================================

namespace render {

static SkColor Color16(const COLOR& c) {
  return SkColorSetARGB(c.alpha >> 8, c.red >> 8, c.green >> 8, c.blue >> 8);
}
static SkBlendMode PictOpToBlend(U8 op) {
  switch (op) {
    case 0:  // Clear
      return SkBlendMode::kClear;
    case 1:  // Src
      return SkBlendMode::kSrc;
    case 2:  // Dst
      return SkBlendMode::kDst;
    case 3:  // Over
      return SkBlendMode::kSrcOver;
    case 4:  // OverReverse
      return SkBlendMode::kDstOver;
    case 5:  // In
      return SkBlendMode::kSrcIn;
    case 6:  // InReverse
      return SkBlendMode::kDstIn;
    case 7:  // Out
      return SkBlendMode::kSrcOut;
    case 8:  // OutReverse
      return SkBlendMode::kDstOut;
    case 9:  // Atop
      return SkBlendMode::kSrcATop;
    case 10:  // AtopReverse
      return SkBlendMode::kDstATop;
    case 11:  // Xor
      return SkBlendMode::kXor;
    case 12:  // Add
      return SkBlendMode::kPlus;
    default:
      return SkBlendMode::kSrcOver;
  }
}

static Drawable* PictureDrawable(Picture* pic) {
  return pic ? server->GetDrawable(pic->drawable) : nullptr;
}

void QueryVersion::Handle(Client& client) {
  QueryVersionReply reply{};
  reply.major_version = 0;
  reply.minor_version = 11;
  Reply(client, std::move(reply));
}

void QueryPictFormats::Handle(Client& client) {
  // Hand-built: four standard formats (ARGB32, RGB24, A8, A1) and one screen mapping
  // Automat's two visuals onto ARGB32 / RGB24. cairo-xlib selects formats from this.
  struct Fmt {
    U32 id;
    U8 depth;
    U8 a_shift, a_bits, r_shift, g_shift, b_shift, rgb_bits;
  };
  const Fmt formats[] = {
      {0x30, 32, 24, 8, 16, 8, 0, 8},  // ARGB32
      {0x31, 24, 0, 0, 16, 8, 0, 8},   // RGB24
      {0x32, 8, 0, 8, 0, 0, 0, 0},     // A8
      {0x33, 1, 0, 1, 0, 0, 0, 0},     // A1
  };
  Buf b;
  b.P<U32>(4);  // num_formats
  b.P<U32>(1);  // num_screens
  b.P<U32>(2);  // num_depths (across screens: depth 24 and 32)
  b.P<U32>(2);  // num_visuals
  b.P<U32>(0);  // num_subpixels
  b.Pad(4);
  auto mask = [](U8 bits) -> U16 { return bits ? (U16)((1u << bits) - 1) : 0; };
  for (auto& f : formats) {
    b.P<U32>(f.id);
    b.P<U8>(1);  // type Direct
    b.P<U8>(f.depth);
    b.Pad(2);
    b.P<U16>(f.r_shift);
    b.P<U16>(mask(f.rgb_bits));
    b.P<U16>(f.g_shift);
    b.P<U16>(mask(f.rgb_bits));
    b.P<U16>(f.b_shift);
    b.P<U16>(mask(f.rgb_bits));
    b.P<U16>(f.a_shift);
    b.P<U16>(mask(f.a_bits));
    b.P<U32>(0);  // colormap
  }
  // One screen: depth 24 -> visual24=RGB24, depth 32 -> visual32=ARGB32.
  b.P<U32>(2);     // num_depths in this screen
  b.P<U32>(0x31);  // fallback format = RGB24
  auto depth = [&](U8 d, U32 visual, U32 fmt) {
    b.P<U8>(d);
    b.Pad(1);
    b.P<U16>(1);  // num_visuals
    b.Pad(4);
    b.P<U32>(visual);
    b.P<U32>(fmt);
  };
  depth(24, kVisual24, 0x31);
  depth(32, kVisual32, 0x30);

  Writer wr(client, Pad4(32 + b.bytes.size()));
  wr.Fixed<U8>(1);
  wr.Skip(1);
  wr.Fixed<U16>(sequence);
  wr.Fixed<U32>((U32)(Pad4(b.bytes.size()) / 4));
  wr.Bytes(b.bytes.data(), b.bytes.size());
}

void QueryPictIndexValues::Handle(Client& client) {
  QueryPictIndexValuesReply reply{};
  Reply(client, std::move(reply));
}
void QueryFilters::Handle(Client& client) {
  QueryFiltersReply reply{};
  Reply(client, std::move(reply));
}

static void ApplyPictureValues(Picture& pic, auto& m) {
  if (m.repeat) pic.repeat = *m.repeat;
  if (m.componentalpha) pic.component_alpha = *m.componentalpha != 0;
  if (m.clipmask) pic.clip_rects.clear();
}

void CreatePicture::Handle(Client& client) {
  if (!client.CheckNewId(pid)) return;
  Picture& pic = server->Add<Picture>(pid, ResType::Picture, client);
  pic.drawable = drawable;
  pic.format = format;
  Drawable* d = server->GetDrawable(drawable);
  pic.has_alpha = d && d->depth >= 32;
  ApplyPictureValues(pic, *this);
}
void ChangePicture::Handle(Client& client) {
  Picture* pic = server->Get<Picture>(picture, ResType::Picture);
  if (pic) ApplyPictureValues(*pic, *this);
}
void SetPictureClipRectangles::Handle(Client& client) {
  Picture* pic = server->Get<Picture>(picture, ResType::Picture);
  if (!pic) return;
  pic->clip_rects.clear();
  for (auto& r : rectangles)
    pic->clip_rects.push_back(
        SkIRect::MakeXYWH(clip_x_origin + r.x, clip_y_origin + r.y, r.width, r.height));
}
void SetPictureTransform::Handle(Client& client) {
  Picture* pic = server->Get<Picture>(picture, ResType::Picture);
  if (!pic) return;
  pic->has_transform = true;
  pic->transform[0] = transform.matrix11 / 65536.0f;
  pic->transform[1] = transform.matrix12 / 65536.0f;
  pic->transform[2] = transform.matrix13 / 65536.0f;
  pic->transform[3] = transform.matrix21 / 65536.0f;
  pic->transform[4] = transform.matrix22 / 65536.0f;
  pic->transform[5] = transform.matrix23 / 65536.0f;
  pic->transform[6] = transform.matrix31 / 65536.0f;
  pic->transform[7] = transform.matrix32 / 65536.0f;
  pic->transform[8] = transform.matrix33 / 65536.0f;
}
void SetPictureFilter::Handle(Client& client) {
  Picture* pic = server->Get<Picture>(picture, ResType::Picture);
  if (pic) pic->filter_bilinear = filter.size() >= 6 && memcmp(filter.data(), "bilin", 5) == 0;
}
void FreePicture::Handle(Client& client) { server->Free(picture); }

void CreateSolidFill::Handle(Client& client) {
  if (!client.CheckNewId(picture)) return;
  Picture& pic = server->Add<Picture>(picture, ResType::Picture, client);
  pic.is_solid = true;
  pic.solid_argb = Color16(color);
}
static void MakeGradient(Picture& pic, SkPoint pts[2], Span<const FIXED> stops,
                         Span<const COLOR> colors, bool radial, SkPoint center, float r0,
                         float r1) {
  Vec<SkColor4f> cols;
  Vec<float> pos;
  for (size_t i = 0; i < stops.size() && i < colors.size(); ++i) {
    cols.push_back(SkColor4f::FromColor(Color16(colors[i])));
    pos.push_back(stops[i] / 65536.0f);
  }
  if (cols.size() < 2) return;
  SkGradient grad{
      SkGradient::Colors{SkSpan<const SkColor4f>(cols.data(), cols.size()),
                         SkSpan<const float>(pos.data(), pos.size()), SkTileMode::kClamp},
      {}};
  if (radial)
    pic.gradient = SkShaders::TwoPointConicalGradient(pts[0], r0, center, r1, grad);
  else
    pic.gradient = SkShaders::LinearGradient(pts, grad);
}
void CreateLinearGradient::Handle(Client& client) {
  if (!client.CheckNewId(picture)) return;
  Picture& pic = server->Add<Picture>(picture, ResType::Picture, client);
  pic.is_solid = false;
  SkPoint pts[2] = {{p1.x / 65536.0f, p1.y / 65536.0f}, {p2.x / 65536.0f, p2.y / 65536.0f}};
  MakeGradient(pic, pts, stops, colors, false, {}, 0, 0);
}
void CreateRadialGradient::Handle(Client& client) {
  if (!client.CheckNewId(picture)) return;
  Picture& pic = server->Add<Picture>(picture, ResType::Picture, client);
  SkPoint pts[2] = {{inner.x / 65536.0f, inner.y / 65536.0f},
                    {outer.x / 65536.0f, outer.y / 65536.0f}};
  MakeGradient(pic, pts, stops, colors, true, {outer.x / 65536.0f, outer.y / 65536.0f},
               inner_radius / 65536.0f, outer_radius / 65536.0f);
}
void CreateConicalGradient::Handle(Client& client) {
  if (client.CheckNewId(picture)) server->Add<Picture>(picture, ResType::Picture, client);
}

// Builds a Skia shader that samples a source picture (solid, gradient, or drawable).
static sk_sp<SkShader> PictureShader(Picture& pic) {
  if (pic.is_solid) return SkShaders::Color(pic.solid_argb);
  if (pic.gradient) return pic.gradient;
  Drawable* d = PictureDrawable(&pic);
  if (d && d->surface) {
    SkTileMode tile = pic.repeat == RepeatNormal ? SkTileMode::kRepeat
                      : pic.repeat == RepeatPad  ? SkTileMode::kClamp
                                                 : SkTileMode::kDecal;
    SkSamplingOptions samp(pic.filter_bilinear ? SkFilterMode::kLinear : SkFilterMode::kNearest);
    SkMatrix m = SkMatrix::I();
    if (pic.has_transform) {
      m.setAll(pic.transform[0], pic.transform[1], pic.transform[2], pic.transform[3],
               pic.transform[4], pic.transform[5], pic.transform[6], pic.transform[7],
               pic.transform[8]);
      SkMatrix inv;
      if (m.invert(&inv)) m = inv;  // RENDER transforms map dst->src
    }
    return d->surface->makeImageSnapshot()->makeShader(tile, tile, samp, &m);
  }
  return nullptr;
}

void Composite::Handle(Client& client) {
  Picture* srcp = server->Get<Picture>(src, ResType::Picture);
  Picture* dstp = server->Get<Picture>(dst, ResType::Picture);
  if (!srcp || !dstp) return;
  Drawable* dd = PictureDrawable(dstp);
  if (!dd || !dd->Canvas()) return;
  SkCanvas& canvas = *dd->Canvas();
  canvas.save();
  if (!dstp->clip_rects.empty()) {
    SkRegion region;
    for (auto& r : dstp->clip_rects) region.op(r, SkRegion::kUnion_Op);
    canvas.clipRegion(region);
  }
  SkPaint paint;
  paint.setBlendMode(PictOpToBlend(op));
  paint.setAntiAlias(true);
  Picture* maskp = server->Get<Picture>(mask, ResType::Picture);
  SkRect dstrect = SkRect::MakeXYWH(dst_x, dst_y, width, height);
  if (srcp->is_solid && !maskp) {
    paint.setColor(srcp->solid_argb);
    canvas.drawRect(dstrect, paint);
  } else if (auto shader = PictureShader(*srcp)) {
    canvas.save();
    canvas.translate(dst_x - src_x, dst_y - src_y);
    paint.setShader(shader);
    canvas.drawRect(SkRect::MakeXYWH(src_x, src_y, width, height), paint);
    canvas.restore();
  }
  canvas.restore();
  if (dd->type == ResType::Window) MarkDirty(static_cast<Window*>(dd));
}

void FillRectangles::Handle(Client& client) {
  Picture* dstp = server->Get<Picture>(dst, ResType::Picture);
  if (!dstp) return;
  Drawable* dd = PictureDrawable(dstp);
  if (!dd || !dd->Canvas()) return;
  SkPaint paint;
  paint.setColor(Color16(color));
  paint.setBlendMode(PictOpToBlend(op));
  for (auto& r : rects)
    dd->Canvas()->drawRect(SkRect::MakeXYWH(r.x, r.y, r.width, r.height), paint);
  if (dd->type == ResType::Window) MarkDirty(static_cast<Window*>(dd));
}

void CreateGlyphSet::Handle(Client& client) {
  if (!client.CheckNewId(gsid)) return;
  GlyphSet& gs = server->Add<GlyphSet>(gsid, ResType::GlyphSet, client);
  gs.argb = format == 0x30;  // ARGB32 glyphset carries color glyphs
}
void ReferenceGlyphSet::Handle(Client& client) {
  if (client.CheckNewId(gsid)) server->Add<GlyphSet>(gsid, ResType::GlyphSet, client);
}
void FreeGlyphSet::Handle(Client& client) { server->Free(glyphset); }
void AddGlyphs::Handle(Client& client) {
  GlyphSet* gs = server->Get<GlyphSet>(glyphset, ResType::GlyphSet);
  if (!gs) return;
  size_t off = 0;
  for (size_t i = 0; i < glyphids.size() && i < glyphs.size(); ++i) {
    const GLYPHINFO& gi = glyphs[i];
    Glyph g;
    g.width = gi.width;
    g.height = gi.height;
    g.x = gi.x;
    g.y = gi.y;
    g.dx = gi.x_off;
    g.dy = gi.y_off;
    size_t stride = gs->argb ? (size_t)gi.width * 4 : (((size_t)gi.width + 3) & ~size_t(3));
    size_t bytes = stride * gi.height;
    if (off + bytes <= data.size() && gi.width > 0 && gi.height > 0) {
      SkImageInfo info =
          gs->argb
              ? SkImageInfo::Make(gi.width, gi.height, kBGRA_8888_SkColorType, kPremul_SkAlphaType)
              : SkImageInfo::Make(gi.width, gi.height, kAlpha_8_SkColorType, kPremul_SkAlphaType);
      SkPixmap pm(info, data.data() + off, stride);
      g.image = SkImages::RasterFromPixmapCopy(pm);
    }
    off += bytes;
    gs->glyphs[glyphids[i]] = std::move(g);
  }
}
void FreeGlyphs::Handle(Client& client) {
  GlyphSet* gs = server->Get<GlyphSet>(glyphset, ResType::GlyphSet);
  if (gs)
    for (U32 id : glyphs) gs->glyphs.erase(id);
}

// Draws one glyph run into `dst` with `src` as the ink color (mask glyphs) or directly
// (color glyphs). The command stream is the RENDER glyph elt encoding.
static void CompositeGlyphRun(Client& client, U8 op, U32 src, U32 dst, U32 glyphset_id, I16 src_x,
                              I16 src_y, Span<const U8> cmds, int idsize) {
  Picture* srcp = server->Get<Picture>(src, ResType::Picture);
  Picture* dstp = server->Get<Picture>(dst, ResType::Picture);
  GlyphSet* gs = server->Get<GlyphSet>(glyphset_id, ResType::GlyphSet);
  if (!srcp || !dstp || !gs) return;
  Drawable* dd = PictureDrawable(dstp);
  if (!dd || !dd->Canvas()) return;
  SkCanvas& canvas = *dd->Canvas();
  SkColor ink = srcp->is_solid ? srcp->solid_argb : SK_ColorBLACK;
  SkPaint paint;
  paint.setColor(ink);
  paint.setBlendMode(PictOpToBlend(op));
  paint.setAntiAlias(true);
  const U8* p = cmds.data();
  const U8* end = p + cmds.size();
  int x = 0, y = 0;
  while (p + 8 <= end) {
    U8 count = p[0];
    if (count == 255) {  // GLYPHABLE: a jump to new x,y follows (8 bytes)
      I32 dx, dy;
      std::memcpy(&dx, p + 4, 4);
      std::memcpy(&dy, p + 8 <= end ? p + 4 : p, 4);
      p += 8;
      continue;
    }
    I16 dx, dy;
    std::memcpy(&dx, p + 4, 2);
    std::memcpy(&dy, p + 6, 2);
    x += dx;
    y += dy;
    p += 8;
    for (int i = 0; i < count && p + idsize <= end; ++i) {
      U32 gid = 0;
      std::memcpy(&gid, p, idsize);
      p += idsize;
      auto it = gs->glyphs.find(gid);
      if (it != gs->glyphs.end()) {
        Glyph& g = it->second;
        if (g.image) {
          SkRect dstrect = SkRect::MakeXYWH(x - g.x, y - g.y, g.width, g.height);
          if (gs->argb)
            canvas.drawImageRect(g.image, dstrect, SkSamplingOptions(), &paint);
          else {
            SkPaint mask = paint;
            mask.setShader(
                g.image->makeShader(SkSamplingOptions(), SkMatrix::Translate(x - g.x, y - g.y)));
            // Alpha-8 image modulates the ink color.
            mask.setColorFilter(SkColorFilters::Blend(ink, SkBlendMode::kSrcIn));
            canvas.drawImageRect(g.image, dstrect, SkSamplingOptions(), &paint);
          }
        }
        x += g.dx;
        y += g.dy;
      }
    }
  }
  if (dd->type == ResType::Window) MarkDirty(static_cast<Window*>(dd));
}
void CompositeGlyphs8::Handle(Client& client) {
  CompositeGlyphRun(client, op, src, dst, glyphset, src_x, src_y, glyphcmds, 1);
}
void CompositeGlyphs16::Handle(Client& client) {
  CompositeGlyphRun(client, op, src, dst, glyphset, src_x, src_y, glyphcmds, 2);
}
void CompositeGlyphs32::Handle(Client& client) {
  CompositeGlyphRun(client, op, src, dst, glyphset, src_x, src_y, glyphcmds, 4);
}

static float Fixed16(FIXED v) { return v / 65536.0f; }
void Trapezoids::Handle(Client& client) {
  Picture* srcp = server->Get<Picture>(src, ResType::Picture);
  Picture* dstp = server->Get<Picture>(dst, ResType::Picture);
  if (!srcp || !dstp) return;
  Drawable* dd = PictureDrawable(dstp);
  if (!dd || !dd->Canvas()) return;
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setBlendMode(PictOpToBlend(op));
  if (srcp->is_solid)
    paint.setColor(srcp->solid_argb);
  else if (auto sh = PictureShader(*srcp)) {
    paint.setShader(sh);
    paint.setColor(SK_ColorBLACK);
  }
  auto edge_x = [](const LINEFIX& l, float y) {
    float y1 = l.p1.y / 65536.0f, y2 = l.p2.y / 65536.0f;
    float x1 = l.p1.x / 65536.0f, x2 = l.p2.x / 65536.0f;
    if (y2 == y1) return x1;
    return x1 + (x2 - x1) * (y - y1) / (y2 - y1);
  };
  for (auto& t : traps) {
    float top = Fixed16(t.top), bottom = Fixed16(t.bottom);
    SkPathBuilder path;
    path.moveTo(edge_x(t.left, top), top);
    path.lineTo(edge_x(t.right, top), top);
    path.lineTo(edge_x(t.right, bottom), bottom);
    path.lineTo(edge_x(t.left, bottom), bottom);
    path.close();
    dd->Canvas()->drawPath(path.detach(), paint);
  }
  if (dd->type == ResType::Window) MarkDirty(static_cast<Window*>(dd));
}
void Triangles::Handle(Client& client) {}
void TriStrip::Handle(Client& client) {}
void TriFan::Handle(Client& client) {}
void AddTraps::Handle(Client& client) {}
void CreateCursor::Handle(Client& client) {
  if (client.CheckNewId(cid)) server->Add<Cursor>(cid, ResType::Cursor, client);
}
void CreateAnimCursor::Handle(Client& client) {
  if (client.CheckNewId(cid)) server->Add<Cursor>(cid, ResType::Cursor, client);
}
}  // namespace render

// ============================================================================
//  DRI3
// ============================================================================

namespace dri3 {
void QueryVersion::Handle(Client& client) {
  QueryVersionReply reply{};
  reply.major_version = 1;
  reply.minor_version = 2;
  Reply(client, std::move(reply));
}
void Open::Handle(Client& client) {
  OpenReply reply{};
#if !defined(_WIN32)
  reply.nfd = 1;
  int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  reply.device_fd = FD(fd >= 0 ? fd : open("/dev/null", O_RDONLY | O_CLOEXEC));
#endif
  Reply(client, std::move(reply));
}
#if defined(__linux__)
static void ImportDmabufPixmap(Client& client, U32 pixmap, int width, int height, U8 depth,
                               U32 format, U64 modifier, int plane_count, FD* fds, U32* strides,
                               U32* offsets) {
  if (!client.CheckNewId(pixmap)) return;
  Pixmap& p = server->Add<Pixmap>(pixmap, ResType::Pixmap, client);
  p.width = width;
  p.height = height;
  p.depth = depth;
  DmabufImage desc;
  desc.width = width;
  desc.height = height;
  desc.drm_format = format;
  desc.modifier = modifier;
  desc.opaque = depth < 32;
  desc.plane_count = plane_count;
  bool ok = true;
  for (int i = 0; i < plane_count; ++i) {
    desc.fds[i] = fcntl(fds[i].fd, F_DUPFD_CLOEXEC, 0);
    desc.strides[i] = strides[i];
    desc.offsets[i] = offsets[i];
    if (desc.fds[i] < 0) ok = false;
  }
  if (ok) p.imported = vk::ImportDmabuf(std::move(desc));
}
#endif
void PixmapFromBuffer::Handle(Client& client) {
#if defined(__linux__)
  FD fds[1];
  fds[0] = std::move(pixmap_fd);
  U32 strides[1] = {stride}, offsets[1] = {0};
  ImportDmabufPixmap(client, pixmap, width, height, depth, 0x34325258 /* XR24 */,
                     0 /* DRM_FORMAT_MOD_LINEAR */, 1, fds, strides, offsets);
#endif
}
void PixmapFromBuffers::Handle(Client& client) {
#if defined(__linux__)
  U32 strides[4] = {stride0, stride1, stride2, stride3};
  U32 offsets[4] = {offset0, offset1, offset2, offset3};
  FD fds[4];
  for (int i = 0; i < num_buffers && i < (int)buffers.size(); ++i) fds[i] = std::move(buffers[i]);
  U32 fourcc = depth >= 32 ? 0x34325241 /* AR24 */ : 0x34325258 /* XR24 */;
  ImportDmabufPixmap(client, pixmap, width, height, depth, fourcc, modifier, num_buffers, fds,
                     strides, offsets);
#endif
}
void BufferFromPixmap::Handle(Client& client) {
  BufferFromPixmapReply reply{};
  Reply(client, std::move(reply));  // export unsupported; empty reply
}
void BuffersFromPixmap::Handle(Client& client) {
  BuffersFromPixmapReply reply{};
  Reply(client, std::move(reply));  // export unsupported; empty reply
}
void GetSupportedModifiers::Handle(Client& client) {
  GetSupportedModifiersReply reply{};
  Reply(client, std::move(reply));
}
void FDFromFence::Handle(Client& client) {
  FDFromFenceReply reply{};
#if !defined(_WIN32)
  reply.fence_fd = FD(open("/dev/null", O_RDONLY | O_CLOEXEC));
#endif
  Reply(client, std::move(reply));
}
void FenceFromFD::Handle(Client& client) {}
void SetDRMDeviceInUse::Handle(Client& client) {}
void ImportSyncobj::Handle(Client& client) {}
void FreeSyncobj::Handle(Client& client) {}
}  // namespace dri3

// ============================================================================
//  Present
// ============================================================================

namespace present {
void QueryVersion::Handle(Client& client) {
  QueryVersionReply reply{};
  reply.major_version = 1;
  reply.minor_version = 4;
  Reply(client, std::move(reply));
}
void QueryCapabilities::Handle(Client& client) {
  QueryCapabilitiesReply reply{};
  reply.capabilities = 0;  // no async/fence flips
  Reply(client, std::move(reply));
}
void SelectInput::Handle(Client& client) {}
void NotifyMSC::Handle(Client& client) {}
void PixmapSynced::Handle(Client& client) {}
void Pixmap::Handle(Client& client) {
  x11::Window* w = server->Get<x11::Window>(window, ResType::Window);
  x11::Pixmap* p = server->Get<x11::Pixmap>(pixmap, ResType::Pixmap);
  if (!w || !p) return;
  x11::Window* top = x11::ToplevelOf(w);
  if (Ptr<library::X11Window> obj_ptr = top ? top->object.Lock() : nullptr) {
    library::X11Window& obj = *obj_ptr;
    if (p->imported) {
      {
        auto lock = std::lock_guard(obj.mutex);
        obj.image = p->imported;
        obj.content_size = {p->width, p->height};
        obj.input_region = SkPath::Rect(SkRect::MakeIWH(p->width, p->height));
      }
      obj.WakeToys();
      vm.WakeToys();
    } else if (p->surface && w->surface) {
      w->surface->getCanvas()->drawImage(p->surface->makeImageSnapshot(), 0, 0);
      MarkDirty(w);
    }
  }
  // Signal the flip completed and the pixmap is idle again.
  CompleteNotify done{};
  done.kind = 0;
  done.window = window;
  done.serial = serial;
  done.Send(client);
  IdleNotify idle{};
  idle.window = window;
  idle.serial = serial;
  idle.pixmap = pixmap;
  idle.Send(client);
}
}  // namespace present

// ============================================================================
//  Input event senders (posted from the toys onto the epoll thread)
// ============================================================================

static x11::Window* HandleWindow(library::X11Window& obj) {
  return (x11::Window*)obj.window_handle.load();
}

void Server::SendMotion(library::X11Window& obj, int x, int y, U32 state) {
  epoll.Post([this, weak = obj.AcquireWeakPtr(), x, y, state] {
    auto o = weak.Lock();
    if (!o) return;
    x11::Window* w = HandleWindow(*o);
    if (!w) return;
    g_last_x = x;
    g_last_y = y;
    EventToOwner(*w, EventMaskPointerMotion, [&](Client& c) {
      MotionNotify ev{};
      ev.detail = 0;
      ev.root = kRoot;
      ev.event = w->xid;
      ev.child = 0;
      ev.root_x = x;
      ev.root_y = y;
      ev.event_x = x;
      ev.event_y = y;
      ev.state = state;
      ev.same_screen = 1;
      ev.Send(c);
    });
    FlushAll();
  });
}
void Server::SendButton(library::X11Window& obj, U32 button, bool pressed, int x, int y,
                        U32 state) {
  epoll.Post([this, weak = obj.AcquireWeakPtr(), button, pressed, x, y, state] {
    auto o = weak.Lock();
    if (!o) return;
    x11::Window* w = HandleWindow(*o);
    if (!w) return;
    U32 mask = pressed ? EventMaskButtonPress : EventMaskButtonRelease;
    EventToOwner(*w, mask, [&](Client& c) {
      if (pressed) {
        ButtonPress ev{};
        ev.detail = button;
        ev.root = kRoot;
        ev.event = w->xid;
        ev.root_x = x;
        ev.root_y = y;
        ev.event_x = x;
        ev.event_y = y;
        ev.state = state;
        ev.same_screen = 1;
        ev.Send(c);
      } else {
        ButtonRelease ev{};
        ev.detail = button;
        ev.root = kRoot;
        ev.event = w->xid;
        ev.root_x = x;
        ev.root_y = y;
        ev.event_x = x;
        ev.event_y = y;
        ev.state = state;
        ev.same_screen = 1;
        ev.Send(c);
      }
    });
    FlushAll();
  });
}
void Server::SendKey(library::X11Window& obj, U32 keycode, bool pressed, U32 state) {
  epoll.Post([this, weak = obj.AcquireWeakPtr(), keycode, pressed, state] {
    auto o = weak.Lock();
    if (!o) return;
    x11::Window* w = HandleWindow(*o);
    if (!w) return;
    EventToOwner(*w, pressed ? EventMaskKeyPress : EventMaskKeyRelease, [&](Client& c) {
      if (pressed) {
        KeyPress ev{};
        ev.detail = keycode;
        ev.root = kRoot;
        ev.event = w->xid;
        ev.root_x = g_last_x;
        ev.root_y = g_last_y;
        ev.event_x = g_last_x;
        ev.event_y = g_last_y;
        ev.state = state;
        ev.same_screen = 1;
        ev.Send(c);
      } else {
        KeyRelease ev{};
        ev.detail = keycode;
        ev.root = kRoot;
        ev.event = w->xid;
        ev.state = state;
        ev.same_screen = 1;
        ev.Send(c);
      }
    });
    FlushAll();
  });
}
void Server::SendCrossing(library::X11Window& obj, bool enter, int x, int y) {
  epoll.Post([this, weak = obj.AcquireWeakPtr(), enter, x, y] {
    auto o = weak.Lock();
    if (!o) return;
    x11::Window* w = HandleWindow(*o);
    if (!w) return;
    EventToOwner(*w, enter ? EventMaskEnterWindow : EventMaskLeaveWindow, [&](Client& c) {
      if (enter) {
        EnterNotify ev{};
        ev.root = kRoot;
        ev.event = w->xid;
        ev.root_x = x;
        ev.root_y = y;
        ev.event_x = x;
        ev.event_y = y;
        ev.same_screen_focus = 1;
        ev.Send(c);
      } else {
        LeaveNotify ev{};
        ev.root = kRoot;
        ev.event = w->xid;
        ev.root_x = x;
        ev.root_y = y;
        ev.event_x = x;
        ev.event_y = y;
        ev.same_screen_focus = 1;
        ev.Send(c);
      }
    });
    FlushAll();
  });
}
void Server::SendFocus(library::X11Window& obj, bool in) {
  epoll.Post([this, weak = obj.AcquireWeakPtr(), in] {
    auto o = weak.Lock();
    if (!o) return;
    x11::Window* w = HandleWindow(*o);
    if (!w) return;
    keyboard_focus = in ? WeakPtr<library::X11Window>(o) : WeakPtr<library::X11Window>{};
    EventToOwner(*w, 0, [&](Client& c) {
      if (in) {
        FocusIn ev{};
        ev.event = w->xid;
        ev.Send(c);
      } else {
        FocusOut ev{};
        ev.event = w->xid;
        ev.Send(c);
      }
    });
    FlushAll();
  });
}
void Server::CloseWindow(void* handle) {
  epoll.Post([this, handle] {
    // Ask the client to close if it selected WM_DELETE_WINDOW; else destroy the window.
    for (auto& [xid, res] : resources) {
      if (res.get() != handle || res->type != ResType::Window) continue;
      x11::Window& w = *static_cast<x11::Window*>(res.get());
      U32 wm_protocols = InternAtom("WM_PROTOCOLS", true);
      U32 wm_delete = InternAtom("WM_DELETE_WINDOW", true);
      auto it = w.props.find(wm_protocols);
      bool supports_delete = false;
      if (it != w.props.end())
        for (size_t i = 0; i + 4 <= it->second.data.size(); i += 4) {
          U32 a;
          std::memcpy(&a, it->second.data.data() + i, 4);
          if (a == wm_delete) supports_delete = true;
        }
      if (supports_delete && w.owner) {
        Client* saved = g_current_client;
        g_current_client = w.owner;
        ClientMessage ev{};
        ev.format = 32;
        ev.window = w.xid;
        ev.type = wm_protocols;
        std::memcpy(ev.data.data8, &wm_delete, 4);
        U32 t = 0;
        std::memcpy(ev.data.data8 + 4, &t, 4);
        ev.Send(*w.owner);
        w.owner->Flush();
        g_current_client = saved;
      } else if (w.owner) {
        // No graceful path: signal the client to exit. The board object may already be
        // gone (this runs from its destructor), so use the pid saved on the window.
        if (w.client_pid > 0) {
#if defined(_WIN32)
          if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)w.client_pid)) {
            TerminateProcess(h, 1);
            CloseHandle(h);
          }
#else
          kill((pid_t)w.client_pid, SIGTERM);
#endif
        }
      }
      break;
    }
  });
}

// ============================================================================
//  Board integration
// ============================================================================

void UnmapAndForgetToplevel(x11::Window& w) {
  if (!w.is_toplevel) return;
  Ptr<library::X11Window> obj = w.object.Lock();
  w.object = {};
  if (!obj) return;  // the board object was already deleted; nothing to remove
  obj->window_handle.store(nullptr);
  {
    auto lock = std::lock_guard(obj->mutex);
    obj->client_gone = true;
  }
  obj->WakeToys();
  {
    auto lock = std::lock_guard(server->ui_mutex);
    server->ui_disappeared.push_back(obj);
  }
  vm.WakeToys();
}

}  // namespace automat::x11

// ============================================================================
//  Board object + toy (library::X11Window)
// ============================================================================

namespace automat::library {

namespace {
constexpr float kPx = x11::kPx;
constexpr float kTitleH = ui::WindowFrame::kTitleH;
constexpr float kFrame = ui::WindowFrame::kFrame;
constexpr float kMinContent = x11::kMinContent;

U32 ModifierState(const ui::Key& key) {
  // The effective modifier mask (low 8 bits, same order as the X core state) plus the active
  // group in bits 13-14, which is what a client reads with XkbGroupForCoreState.
  return (U32)xkb::ModifierMask(key) | (((U32)key.layout & 0x3u) << 13);
}
}  // namespace

// A mapped X11 top-level shown on the board: the shared window chrome (window_frame.hpp)
// around the window's composited snapshot. Mirrors WaylandWindowToy: caret-gated keyboard,
// pointer pass-through. Override-redirect windows (menus, tooltips) stay bare.
struct X11WindowToy : ui::beta::ObjectToy, ui::PointerMoveCallback {
  Str title_;
  bool client_gone_ = false;
  bool override_redirect_ = false;
  bool client_decorated_ = false;
  DecoratedWindow::DecorationPreference pref_ = DecoratedWindow::DecorationPreference::Auto;
  sk_sp<SkImage> image_;
  SkISize content_size_ = {};
  SkPath input_shape_;
  ui::Caret* caret_ = nullptr;

  X11WindowToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) { PullState(); }
  ~X11WindowToy() override {
    if (caret_) caret_->Release();
  }

  Ptr<X11Window> LockWindow() const { return LockObject<X11Window>(); }

  void PullState() {
    auto win = LockWindow();
    if (!win) return;
    auto lock = std::lock_guard(win->mutex);
    title_ = win->title.empty() ? win->app_id : win->title;
    client_gone_ = win->client_gone;
    override_redirect_ = win->override_redirect;
    client_decorated_ = win->client_decorated;
    pref_ = win->decoration_preference.load(std::memory_order_relaxed);
    image_ = win->image;
    content_size_ = win->content_size;
    input_shape_ = win->input_region.makeTransform(SkMatrix::Scale(kPx, -kPx));
  }

  bool Decorated() const {
    using P = DecoratedWindow::DecorationPreference;
    if (override_redirect_) return false;
    if (client_gone_) return true;
    if (pref_ != P::Auto) return pref_ == P::ServerSide;
    return !client_decorated_;
  }

  Vec2 ContentSize() const {
    return {content_size_.width() > 0 ? content_size_.width() * kPx : kMinContent,
            content_size_.height() > 0 ? content_size_.height() * kPx : kMinContent};
  }
  Rect ContentRect() const { return Rect::MakeAtZero(ContentSize()); }
  Vec2 TopLeft() const { return ContentRect().TopLeftCorner(); }
  ui::WindowFrame Chrome() const { return {ContentSize(), title_}; }

  SkPath FocusCaretShape() const { return Decorated() ? Chrome().FocusCaretShape() : SkPath(); }

  Tock Tick(time::Timer& timer) override {
    if (!LockWindow()) {
      MarkDead(timer.now);
      return {};
    }
    Str prev_title = title_;
    SkISize prev_size = content_size_;
    SkPath prev_input = input_shape_;
    bool prev_decorated = Decorated();
    PullState();
    if (caret_) caret_->shape = FocusCaretShape();
    Tock tock = Tock::Draw;
    if (title_ != prev_title || content_size_ != prev_size || input_shape_ != prev_input ||
        Decorated() != prev_decorated)
      tock |= Tock::Shape;
    return tock;
  }

  bool CenteredAtZero() const override { return true; }

  SkPath Shape() const override {
    if (!Decorated()) return SkPath::Rect(ContentRect());
    return Chrome().Shape();
  }

  // ---- pointer pass-through ----
  Vec2 ToSurfacePx(Vec2 l) const {
    Vec2 sz = ContentSize();
    l += Vec2(sz.x / 2, sz.y / 2);  // from center to top-left origin
    l.y = sz.y - l.y;               // flip to y-down
    return l / kPx;
  }

  void PointerMove(ui::Pointer& p, Vec2) override {
    Vec2 px = ToSurfacePx(p.PositionWithin(*this));
    if (auto win = LockWindow()) x11::server->SendMotion(*win, (int)px.x, (int)px.y, 0);
  }
  void PointerEnter(ui::Pointer& p) override {
    Vec2 px = ToSurfacePx(p.PositionWithin(*this));
    if (auto win = LockWindow()) x11::server->SendCrossing(*win, true, (int)px.x, (int)px.y);
    StartWatching(p);
  }
  void PointerLeave(ui::Pointer& p) override {
    if (auto win = LockWindow()) x11::server->SendCrossing(*win, false, 0, 0);
    StopWatching(p);
  }

  void FocusClient(ui::Pointer& p) {
    if (caret_ || !p.keyboard) return;
    auto [w, h] = ContentSize().xy;
    caret_ = &p.keyboard->RequestCaret(*this, Vec2(-w / 2, h / 2));
    caret_->shape = FocusCaretShape();
    if (auto win = LockWindow()) x11::server->SendFocus(*win, true);
    WakeAnimation();
  }
  void ReleaseCaret(ui::Caret&) override {
    caret_ = nullptr;
    if (auto win = LockWindow()) x11::server->SendFocus(*win, false);
    WakeAnimation();
  }
  void ForwardKey(ui::Key key, bool pressed) {
#if defined(__linux__)
    U32 keycode = (U32)automat::x11::KeyToX11KeyCode(key.physical);
    if (keycode <= 8) return;
    if (auto win = LockWindow()) x11::server->SendKey(*win, keycode, pressed, ModifierState(key));
#endif
  }
  void KeyDown(ui::Caret&, ui::Key key) override { ForwardKey(key, true); }
  void KeyUp(ui::Caret&, ui::Key key) override { ForwardKey(key, false); }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

  void VisitOptions(const OptionsVisitor& visitor) const override {
    ObjectToy::VisitOptions(visitor);
    VisitDecorationOptions(owner, visitor);
  }

  void Draw(SkCanvas& canvas) const override {
    Vec2 sz = ContentSize();
    Rect content = ContentRect();
    if (image_) {
      SkPaint paint;
      if (image_->isOpaque())
        paint.setColorFilter(SkColorFilters::Blend(SK_ColorBLACK, SkBlendMode::kDstOver));
      canvas.save();
      canvas.translate(content.left, content.top);
      canvas.scale(1, -1);
      canvas.drawImageRect(image_, SkRect::MakeWH(sz.x, sz.y),
                           SkSamplingOptions(SkFilterMode::kLinear), &paint);
      canvas.restore();
    } else {
      SkPaint bg;
      bg.setColor("#202020"_color);
      canvas.drawRect(content, bg);
    }
    if (Decorated()) Chrome().Draw(canvas);
  }
};

// Held while a button initiated over the window is down: routes the press/release to the
// client and focuses it, instead of dragging the object.
struct X11InputAction : ClientInputActionBase {
  X11WindowToy& toy;
  U32 button;
  X11InputAction(ui::Pointer& p, X11WindowToy& toy, U32 button)
      : ClientInputActionBase(p), toy(toy), button(button) {
    toy.FocusClient(p);
    Vec2 px = toy.ToSurfacePx(p.PositionWithin(toy));
    if (auto win = toy.LockWindow()) {
      LinkWindow(*win);
      x11::server->SendButton(*win, button, true, (int)px.x, (int)px.y, 0);
    }
  }
  void Update() override {}
  ui::Widget& InitiatingWidget() override { return toy; }
  ~X11InputAction() override {
    Vec2 px = toy.ToSurfacePx(pointer.PositionWithin(toy));
    if (auto win = toy.LockWindow())
      x11::server->SendButton(*win, button, false, (int)px.x, (int)px.y, 0);
  }
};

static U32 X11ButtonCode(ui::ActionTrigger btn) {
  using ui::PointerButton;
  switch ((PointerButton)btn) {
    case PointerButton::Left:
      return 1;
    case PointerButton::Middle:
      return 2;
    case PointerButton::Right:
      return 3;
    default:
      return 0;
  }
}

std::unique_ptr<Action> X11WindowToy::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  bool in_content = ContentRect().Contains(p.PositionWithin(*this));
  if (in_content)
    if (U32 code = X11ButtonCode(btn)) return std::make_unique<X11InputAction>(p, *this, code);
  return ObjectToy::FindAction(p, btn);
}

std::unique_ptr<ObjectToy> X11Window::MakeToy(ui::Widget* parent) {
  return std::make_unique<X11WindowToy>(parent, *this);
}

X11Window::~X11Window() {
#if defined(__linux__)
  if (x11::server) x11::server->CloseWindow(window_handle.load());
#endif
}

Ptr<Object> X11Window::Clone() const {
  auto win = MAKE_PTR(X11Window, *this);
  LaunchClone(*this, *win);
  return win;
}

void X11Window::DecorationPreferenceChanged() { WakeToys(); }

}  // namespace automat::library

// ============================================================================
//  Server lifecycle + UIFrame
// ============================================================================

namespace automat::x11 {

using library::X11Window;

// Flatten Automat's shared keymap into the core protocol's view: two levels per group per
// key for GetKeyboardMapping, and, for GetModifierMapping, the keys that activate each real
// modifier (found by pressing each key on a scratch state and seeing which modifiers set).
void LoadKeymap(Server& s) {
  s.keymap_min = 8;
  s.keymap_max = 255;
  s.keysyms_per_code = 2;
  s.keysyms.clear();
  std::memset(s.mod_map, 0, sizeof(s.mod_map));
  std::memset(s.key_mods, 0, sizeof(s.key_mods));

  xkb_keymap* km = keymap ? keymap->xkb : nullptr;
  if (km) {
    // Core protocol keycodes are CARD8: keys past 255 (the compiled default keymap extends
    // to 708) are unreachable over X11 and are dropped.
    s.keymap_min = std::clamp<xkb_keycode_t>(xkb_keymap_min_keycode(km), 8, 255);
    s.keymap_max = std::clamp<xkb_keycode_t>(xkb_keymap_max_keycode(km), 8, 255);
    xkb_layout_index_t groups = std::max<xkb_layout_index_t>(xkb_keymap_num_layouts(km), 1);
    s.keysyms_per_code = 2 * (int)groups;
    for (xkb_keycode_t kc = s.keymap_min; kc <= s.keymap_max; ++kc) {
      xkb_layout_index_t key_groups = xkb_keymap_num_layouts_for_key(km, kc);
      for (xkb_layout_index_t g = 0; g < groups; ++g)
        for (xkb_level_index_t level = 0; level < 2; ++level) {
          const xkb_keysym_t* syms = nullptr;
          int n = g < key_groups ? xkb_keymap_key_get_syms_by_level(km, kc, g, level, &syms) : 0;
          s.keysyms.push_back(n > 0 ? syms[0] : 0 /* NoSymbol */);
        }
    }
    for (xkb_keycode_t kc = s.keymap_min; kc <= s.keymap_max; ++kc) {
      xkb_state* st = xkb_state_new(km);
      if (!st) break;
      xkb_state_update_key(st, kc, XKB_KEY_DOWN);
      xkb_mod_mask_t mods = xkb_state_serialize_mods(st, XKB_STATE_MODS_EFFECTIVE);
      xkb_state_unref(st);
      s.key_mods[kc] = (U8)(mods & 0xff);
      for (int m = 0; m < 8; ++m) {
        if (!(mods & (1u << m))) continue;
        for (int k = 0; k < 4; ++k)
          if (s.mod_map[m][k] == 0) {
            s.mod_map[m][k] = (U8)kc;
            break;
          }
      }
    }
  }
  if (s.keysyms.empty()) {
    // Fallback: an identity mapping so keycodes at least reach clients.
    s.keysyms_per_code = 1;
    for (int kc = s.keymap_min; kc <= s.keymap_max; ++kc) s.keysyms.push_back(kc);
  }
}

Server::~Server() {
  if (socket_path) {
    Status ignore;
    socket_path.Unlink(ignore, true);
    socket_path.WithExt("lock").Unlink(ignore, true);
  }
}

void Server::NotifyRead(Status&) {
  for (;;) {
#if defined(_WIN32)
    SOCKET accepted = accept((SOCKET)fd.fd, nullptr, nullptr);
    if (accepted == INVALID_SOCKET) break;
    u_long non_blocking = 1;
    ioctlsocket(accepted, FIONBIO, &non_blocking);
    int client_fd = (int)accepted;
#else
    int client_fd = accept4(fd.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
#endif
    Client& client = *Client::colony.emplace(*this);
    client.fd = FD(client_fd);
    client.index = next_client_index++;
    Status status;
    epoll.Add(&client, status);
  }
}

void Server::FlushAll() {
  for (Client& client : Client::colony) client.Flush();
}

void Start(mux::Epoll& epoll, Status& status) {
  int chosen = -1;
#if defined(_WIN32)
  // X11-over-TCP convention: display :n is TCP port 6000+n; stay on loopback.
  SOCKET listen_fd = INVALID_SOCKET;
  for (int n = 0; n <= 32; ++n) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) break;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)(6000 + n));
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
      closesocket(s);  // port taken (another X server?) - try the next display
      continue;
    }
    u_long non_blocking = 1;
    ioctlsocket(s, FIONBIO, &non_blocking);
    listen_fd = s;
    chosen = n;
    break;
  }
  if (chosen < 0) {
    AppendErrorMessage(status) += "no free X11 display port in 6000-6032";
    return;
  }
  if (listen(listen_fd, 16) != 0) {
    closesocket(listen_fd);
    AppendErrorMessage(status) += f("x11 listen(): error {}", WSAGetLastError());
    return;
  }
  LOG << f("X11 server listening on 127.0.0.1:{} (DISPLAY=127.0.0.1:{})", 6000 + chosen, chosen);

  server = std::make_unique<Server>(epoll);
  server->fd = FD((int)listen_fd);
  server->display_number = chosen;
#else
  int listen_fd = -1;
  FD lock_fd;
  Str sock_path;
  for (int n = 1; n <= 32; ++n) {
    Str lock = f("/tmp/.X{}-lock", n);
    int lf = open(lock.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0644);
    if (lf < 0) continue;
    if (flock(lf, LOCK_EX | LOCK_NB) != 0) {
      close(lf);
      continue;
    }
    Str path = f("/tmp/.X11-unix/X{}", n);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
      close(s);
      close(lf);
      continue;
    }
    unlink(path.c_str());
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
      close(s);
      close(lf);
      continue;
    }
    listen_fd = s;
    lock_fd = FD(lf);
    sock_path = path;
    chosen = n;
    break;
  }
  if (chosen < 0) {
    AppendErrorMessage(status) += "no free X11 display slot in /tmp/.X11-unix";
    return;
  }
  if (listen(listen_fd, 16) < 0) {
    close(listen_fd);
    AppendErrorMessage(status) += f("x11 listen(): {}", strerror(errno));
    return;
  }
  LOG << f("X11 server listening on {} (DISPLAY=:{})", sock_path, chosen);

  server = std::make_unique<Server>(epoll);
  server->fd = FD(listen_fd);
  server->socket_path = sock_path;
  server->lock_fd = std::move(lock_fd);
  server->display_number = chosen;
#endif

  // Seed predefined atoms 1..68.
  server->atom_names.push_back("");  // atom 0 = None
  for (const char* name : kPredefinedAtoms) server->InternAtom(name, false);

  // The root window: owned by no client, never destroyed until shutdown.
  auto root = std::make_unique<Window>();
  root->xid = kRoot;
  root->width = kScreenW;
  root->height = kScreenH;
  root->depth = 24;
  root->mapped = true;
  server->root = root.get();
  server->resources[kRoot] = std::move(root);

  // A minimal EWMH window-manager identity. Without it GTK/Qt toolkits treat the display
  // as unmanaged and stall their first paint waiting for a window manager to appear.
  {
    auto check = std::make_unique<Window>();
    check->xid = kWmCheck;
    check->parent = server->root;
    check->depth = 24;
    server->root->children.push_back(check.get());
    Window& c = *check;
    server->resources[kWmCheck] = std::move(check);
    U32 utf8 = server->InternAtom("UTF8_STRING", false);
    U32 win_atom = server->InternAtom("WINDOW", false);
    U32 check_id = kWmCheck;
    SetProp(c, "_NET_SUPPORTING_WM_CHECK", win_atom, 32, &check_id, 4);
    SetProp(*server->root, "_NET_SUPPORTING_WM_CHECK", win_atom, 32, &check_id, 4);
    const char* wm_name = "Automat";
    SetProp(c, "_NET_WM_NAME", utf8, 8, wm_name, strlen(wm_name));
    U32 supported[] = {
        server->InternAtom("_NET_SUPPORTING_WM_CHECK", false),
        server->InternAtom("_NET_WM_NAME", false),
        server->InternAtom("_NET_WM_STATE", false),
        server->InternAtom("_NET_ACTIVE_WINDOW", false),
        server->InternAtom("_NET_FRAME_EXTENTS", false),
        server->InternAtom("_NET_WM_STATE_FOCUSED", false),
        server->InternAtom("_NET_WM_MOVERESIZE", false),
    };
    SetProp(*server->root, "_NET_SUPPORTED", server->InternAtom("ATOM", false), 32, supported,
            sizeof(supported));
  }

  LoadKeymap(*server);

  epoll.Post([s = server.get()] {
    Status status;
    s->epoll.Add(s, status);
    if (!OK(status)) ERROR << "x11: failed to watch the listening socket: " << status.ToStr();
  });
}

void Stop() { server.reset(); }
Str SocketName() {
  if (!server) return {};
#if defined(_WIN32)
  // Host form: Xlib/xcb parse "host:display" and dial TCP port 6000+display.
  return f("127.0.0.1:{}", server->display_number);
#else
  return f(":{}", server->display_number);
#endif
}
void Tick() {
  if (!server) return;
  Server& s = *server;
  Vec<std::pair<Ptr<X11Window>, Ptr<Launch>>> appeared;
  Vec<Ptr<X11Window>> disappeared;
  Vec<WeakPtr<X11Window>> move_requests;
  {
    auto lock = std::lock_guard(s.ui_mutex);
    appeared.swap(s.ui_appeared);
    disappeared.swap(s.ui_disappeared);
    move_requests.swap(s.ui_move_requests);
  }
  for (auto& weak : move_requests) {
    if (auto win = weak.Lock()) StartClientMove(*win);
  }
  static int spawn_count = 0;
  for (auto& [w, launch] : appeared) {
    auto& win = *w;
    Ptr<Object> source = launch ? launch->source.Lock() : nullptr;
    Location* source_location = source ? source->MyLocation() : nullptr;
    if (source && !win.launcher->IsConnected()) {
      win.launcher->Connect(Interface(source.get(), nullptr));
    }
    Board* board = source_location ? source_location->LockBoard().get() : nullptr;
    if (!board) board = &DefaultBoard();
    auto& loc = board->CreateEmpty();
    if (source_location) {
      loc.placement = Location::PlaceBeside{source_location->AcquireWeakPtr()};
    } else {
      int n = spawn_count++;
      loc.placement = Location::Direct{Vec2(0.01f * (n % 3), -0.02f * (n % 5))};
    }
    loc.InsertHere(std::move(w));
    board->WakeToys();
    vm.WakeToys();
  }
  for (auto& w : disappeared) {
    auto vm_lock = std::lock_guard(vm.mutex);
    for (auto& board : vm.boards) {
      if (auto* here = board->LocationOrNull(*w)) {
        board->Extract(*here);
        board->WakeToys();
      }
    }
    vm.WakeToys();
  }
}

}  // namespace automat::x11
