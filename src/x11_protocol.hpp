#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <ankerl/unordered_dense.h>
#include <include/core/SkImage.h>
#include <include/core/SkPath.h>
#include <include/core/SkPoint.h>
#include <include/core/SkSurface.h>

#include <bit>
#include <cstring>
#include <deque>
#include <memory>
#include <string>

#include "../build/generated/x11_generated.hpp"  // IWYU pragma: export
#include "colony.hpp"
#include "fd.hpp"
#include "int.hpp"
#include "mortal.hpp"
#include "mux.hpp"
#include "path.hpp"
#include "ptr.hpp"
#include "span.hpp"
#include "str.hpp"
#include "string_multimap.hpp"
#include "vec.hpp"
#include "x11.hpp"

// Automat's built-in X11 server. The wire codec is little-endian only. Each request,
// reply and event is a generated struct (build/generated/x11_generated.hpp); this header
// adds the runtime the generated code and the handlers in x11.cpp build on:
//
//   Reader / Writer - little-endian wire cursors over a client's in / out buffers.
//   Client          - one connection: an mux::Epoll::Listener with the id bookkeeping.
//   Server          - the listening socket, the resource table, atoms, input focus.
//   Resource tree   - Window / Pixmap (Drawable), GContext, Colormap, Cursor, Font,
//                     Picture, GlyphSet, ShmSeg. Drawables carry a raster SkSurface, so
//                     core and RENDER drawing is Skia drawing and a window's board
//                     snapshot is a makeImageSnapshot of its composited tree.

namespace automat::x11 {

struct Client;
struct Server;

// A little-endian cursor that reads one request body. `ok` goes false on truncation; the
// dispatcher turns that into a Length error instead of reading past the buffer.
struct Reader {
  const char* p;
  const char* end;
  U8 data_byte = 0;  // request header byte 1 (the first small field of core messages)
  bool ok = true;

  template <class T>
  T Fixed() {
    T v{};
    if (p + sizeof(T) > end) {
      ok = false;
      return v;
    }
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
  }
  void Skip(size_t n) {
    if (p + n > end) {
      ok = false;
      p = end;
    } else {
      p += n;
    }
  }
  void Bytes(void* dst, size_t n) {
    if (p + n > end) {
      ok = false;
      std::memset(dst, 0, n);
      return;
    }
    std::memcpy(dst, p, n);
    p += n;
  }
  template <class T>
  Span<const T> List(size_t count) {
    size_t bytes = count * sizeof(T);
    if (p + bytes > end) {
      ok = false;
      return {};
    }
    Span<const T> out((const T*)p, count);
    p += bytes;
    return out;
  }
  StrView Str(size_t count) {
    if (p + count > end) {
      ok = false;
      return {};
    }
    StrView out(p, count);
    p += count;
    return out;
  }
  template <class T>
  Span<const T> Rest() {
    size_t count = (size_t)(end - p) / sizeof(T);
    Span<const T> out((const T*)p, count);
    p = end;
    return out;
  }
  StrView RestStr() {
    StrView out(p, (size_t)(end - p));
    p = end;
    return out;
  }
  FD TakeFd(Client&);
};

// A little-endian cursor that appends one reply or event to a client's out buffer. The
// buffer is grown by `total` up front and zero-filled, so any bytes a message leaves
// unwritten (the reply's unused tail) are already zero.
struct Writer {
  std::string& out;
  size_t pos;

  Writer(Client&, size_t total);
  template <class T>
  void Fixed(T v) {
    std::memcpy(out.data() + pos, &v, sizeof(T));
    pos += sizeof(T);
  }
  void Skip(size_t n) { pos += n; }
  void Bytes(const void* src, size_t n) {
    std::memcpy(out.data() + pos, src, n);
    pos += n;
  }
  template <class T>
  void List(Span<const T> s) {
    Bytes(s.data(), s.size_bytes());
  }
  void List(StrView s) { Bytes(s.data(), s.size()); }
  void StrList(Span<const StrView> s) {
    for (StrView v : s) {
      Fixed<U8>((U8)v.size());
      Bytes(v.data(), v.size());
    }
  }
  void Pad4() { pos = (pos + 3) & ~size_t(3); }
  void TakeFd(FD&&);
};

inline size_t Pad4(size_t n) { return (n + 3) & ~size_t(3); }
inline size_t StrListBytes(Span<const StrView> s) {
  size_t n = 0;
  for (StrView v : s) n += 1 + v.size();
  return n;
}

U16 ClientSequence(Client&);
bool DecodeFailed(Client&, U16 sequence);  // emits a Length error, returns true

// ---- resources ---------------------------------------------------------------------

enum class ResType : U8 {
  Window,
  Pixmap,
  GContext,
  Colormap,
  Cursor,
  Font,
  Picture,
  GlyphSet,
  ShmSeg,
};

struct Resource {
  U32 xid = 0;
  ResType type;
  Client* owner = nullptr;
  Resource(ResType t) : type(t) {}
  virtual ~Resource() = default;
};

// A Window or a Pixmap: anything with pixels drawing targets. The raster surface is the
// backing store; a window composites its mapped children into it for its board snapshot.
struct Drawable : Resource {
  int width = 0, height = 0;
  U8 depth = 32;
  sk_sp<SkSurface> surface;  // raster, kBGRA_8888; null until sized
  sk_sp<SkImage> imported;   // a DRI3 dmabuf import, displayed zero-copy on Present
  Drawable(ResType t) : Resource(t) {}
  SkCanvas* Canvas();
  void Ensure(int w, int h, U8 depth);
};

struct GContext;

struct Window : Drawable {
  Window* parent = nullptr;
  Vec<Window*> children;  // bottom-to-top stacking order
  int x = 0, y = 0;       // relative to parent's origin
  int border_width = 0;
  U16 win_class = 1;  // InputOutput
  bool mapped = false;
  bool override_redirect = false;
  U32 event_mask = 0;  // the owning client's event selection on this window
  U32 do_not_propagate = 0;
  U32 background_pixel = 0;
  bool has_background_pixel = false;
  struct Property {
    U32 type = 0;
    U8 format = 8;
    Vec<U8> data;
  };
  ankerl::unordered_dense::map<U32, Property> props;  // atom -> value

  // The board object mirroring a top-level window. Weak: the Location is the sole strong
  // owner, so deleting it destroys the object; a strong ref here would dangle its freed toy.
  WeakPtr<library::X11Window> object;
  I64 client_pid = 0;  // for CloseWindow after the object is already gone
  bool is_toplevel = false;

  Window() : Drawable(ResType::Window) {}
};

struct Pixmap : Drawable {
  Pixmap() : Drawable(ResType::Pixmap) {}
};

// Only the graphics-context state Automat's drawing reads is kept; a client may set the
// rest of the GC value list (tiles, dashes, functions) and it is accepted and ignored.
struct GContext : Resource {
  U32 foreground = 0;
  int line_width = 0;
  U32 clip_x_origin = 0, clip_y_origin = 0;
  Vec<SkIRect> clip_rects;  // empty = unclipped
  GContext() : Resource(ResType::GContext) {}
};

struct Colormap : Resource {
  Colormap() : Resource(ResType::Colormap) {}
};
struct Cursor : Resource {
  Cursor() : Resource(ResType::Cursor) {}
};
struct Font : Resource {
  Font() : Resource(ResType::Font) {}
};

// ---- RENDER resources --------------------------------------------------------------

struct Picture : Resource {
  U32 drawable = 0;       // the Drawable this picture draws into / samples from
  U32 format = 0;         // PICTFORMAT id (selects alpha handling)
  bool has_alpha = true;  // ARGB vs RGB format
  U32 repeat = 0;         // RepeatNone/Normal/Pad/Reflect
  bool component_alpha = false;
  Vec<SkIRect> clip_rects;
  bool has_transform = false;
  float transform[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major, 16.16 decoded
  U32 filter_bilinear = false;

  // A solid-fill or gradient source picture has no drawable; it samples a shader.
  bool is_solid = false;
  U32 solid_argb = 0;
  sk_sp<SkShader> gradient;
  Picture() : Resource(ResType::Picture) {}
};

struct Glyph {
  int width = 0, height = 0, x = 0, y = 0;  // x,y = origin offset (xcb GLYPHINFO)
  int dx = 0, dy = 0;                       // advance
  sk_sp<SkImage> image;                     // A8 or BGRA
};
struct GlyphSet : Resource {
  ankerl::unordered_dense::map<U32, Glyph> glyphs;
  bool argb = false;  // format has color (else A8 mask)
  GlyphSet() : Resource(ResType::GlyphSet) {}
};

struct ShmSeg : Resource {
  FD fd;
  void* addr = nullptr;
  size_t size = 0;
  bool sysv = false;  // addr came from shmat (detach with shmdt) rather than mmap
  ShmSeg() : Resource(ResType::ShmSeg) {}
  ~ShmSeg() override;
};

// ---- client ------------------------------------------------------------------------

struct Client : mux::Epoll::Listener {
  Server& server;
  std::string in;
  std::string out;
  std::deque<FD> recv_fds;
  Vec<FD> out_fds;

  bool setup_done = false;
  bool big_requests = false;
  U16 sequence = 0;  // last request's sequence number (echoed in replies/events)
  U32 id_base = 0;   // resource-id-base handed to this client
  U32 id_mask = 0x1fffff;
  int index = 0;  // slot used to derive id_base

  static Colony<Client> colony;

  Client(Server& s) : server(s) {}
  ~Client() override;

  void NotifyRead(Status&) override;
  StrView Name() const override { return "X11Client"sv; }

  void Flush();
  void Disconnect();

  // Emits an error reply for the request currently being processed.
  void Error(U8 code, U32 bad_value, U16 minor = 0, U8 major = 0);
  bool CheckNewId(U32 xid);  // validates a client-allocated XID, errors if bad
};

// ---- server ------------------------------------------------------------------------

struct Server : mux::Epoll::Listener {
  mux::Epoll& epoll;
  Path socket_path;  // the AF_UNIX path (@/tmp/.X11-unix/X<n>); unlinked on shutdown
  FD lock_fd;
  int display_number = -1;

  U32 next_client_index = 1;

  // The single global resource table (X resources are server-global and shareable).
  ankerl::unordered_dense::map<U32, std::unique_ptr<Resource>> resources;
  Window* root = nullptr;

  // Atoms: interned strings. Predefined atoms 1..68 seeded at construction. The reverse map
  // owns its keys: a view into atom_names would dangle when the Vec reallocates.
  Vec<Str> atom_names;  // index = atom id; [0] unused
  ankerl::unordered_dense::map<Str, U32, string_hash, string_equal> atom_ids;

  // Selections (clipboard): atom -> owning window xid.
  ankerl::unordered_dense::map<U32, U32> selection_owner;

  // Input. The core-protocol view of the shared keymap (keymap.hpp), flattened by
  // LoadKeymap() in x11.cpp.
  WeakPtr<library::X11Window> keyboard_focus;
  int keymap_min = 8, keymap_max = 255;
  int keysyms_per_code = 0;
  Vec<U32> keysyms;       // keyboard mapping, min..max
  U8 mod_map[8][8] = {};  // modifier mapping (per real modifier -> keycodes)
  U8 key_mods[256] = {};  // per keycode -> the real modifier mask it activates (for XKB)

  // Board handoff, mirroring the wayland server.
  std::mutex ui_mutex;
  Vec<Ptr<library::X11Window>> ui_appeared;
  Vec<Ptr<library::X11Window>> ui_disappeared;
  std::mutex adoption_mutex;
  Vec<std::pair<I64, WeakPtr<library::X11Window>>> adoptions;
  WeakPtr<ui::PointerObject> pointer_object;

  std::unique_ptr<mux::Timer> frame_timer;

  Server(mux::Epoll& e) : epoll(e) {}
  ~Server();

  StrView Name() const override { return "X11Server"sv; }
  void NotifyRead(Status&) override;  // accepts connections

  void FlushAll();

  U32 InternAtom(StrView name, bool only_if_exists);
  StrView AtomName(U32 atom);

  template <class T>
  T* Get(U32 xid, ResType type) {
    auto it = resources.find(xid);
    if (it == resources.end() || it->second->type != type) return nullptr;
    return static_cast<T*>(it->second.get());
  }
  Drawable* GetDrawable(U32 xid) {
    auto it = resources.find(xid);
    if (it == resources.end()) return nullptr;
    if (it->second->type != ResType::Window && it->second->type != ResType::Pixmap) return nullptr;
    return static_cast<Drawable*>(it->second.get());
  }
  template <class T>
  T& Add(U32 xid, ResType type, Client& owner) {
    auto res = std::make_unique<T>();
    res->xid = xid;
    res->owner = &owner;
    T* ptr = res.get();
    resources[xid] = std::move(res);
    return *ptr;
  }
  void Free(U32 xid);
  void DestroyClientResources(Client&);

  // Cross-thread entry points (posted onto the epoll thread), used by the toys.
  void SendKey(library::X11Window&, U32 x11_keycode, bool pressed, U32 state);
  void SendButton(library::X11Window&, U32 button, bool pressed, int x, int y, U32 state);
  void SendMotion(library::X11Window&, int x, int y, U32 state);
  void SendCrossing(library::X11Window&, bool enter, int x, int y);
  void SendFocus(library::X11Window&, bool in);
  void CloseWindow(void* window_handle);

  // Recomposites `w`'s tree into a snapshot on its board object and wakes its toy.
  void PublishWindow(Window& w);
};

extern std::unique_ptr<Server> server;

}  // namespace automat::x11
