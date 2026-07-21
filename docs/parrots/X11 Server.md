# X11 Server

## What it is

Automat is an X11 display server as well as a Wayland compositor. Processes started by
the Command object connect to Automat's X socket, and every top-level window they map
becomes an "X11 Window" object on the board, draggable and deletable like any other
object. Programs that speak X11 rather than Wayland — which includes most applications
that need more access than Wayland grants — are thereby composed on the board like
Automat's own objects. The design deliberately mirrors the Wayland compositor so the two share the
board integration, the input bridge, and the GPU import path; read `Wayland Compositor.md`
first, because this document only describes where X11 differs.

Code map: `src/x11.hpp` (the public surface: the board window object and the four server
lifecycle calls), `src/x11_protocol.hpp` (the wire runtime and the resource tree, included
only by the implementation and the generated dispatcher), `src/x11.cpp` (the protocol
handlers, the drawing, the board object and its toy), `src/x11.py` (protocol code
generation), `src/x11/*.xml` (the vendored xcb-proto definitions),
`build/generated/x11_generated.{hpp,cpp}` (generated structs and wire code). The
keycode tables shared with the host-X client code are in `src/x11_keys.{hpp,cpp}`.

## Protocol stack

The server speaks the X11 wire protocol directly; there is no Xlib and no server-side
library. Every request, reply and event is one plain C++ struct generated from the
xcb-proto XML by `src/x11.py`. The generator is the counterpart of `src/wayland.py` with
one deliberate difference: all generated text goes to `build/generated/x11_generated.hpp`
rather than into validated blocks in a hand-written header. Wayland interfaces mix
generated declarations with hand-written per-object state, so their structs must be
editable in place; X11 messages are pure data, because server state is stored on the resource
structs in `src/x11_protocol.hpp` instead, so nothing hand-written needs to sit next to
the generated message. Adding an extension is dropping its XML in `src/x11/` and
implementing the new `Handle` methods — a missing one is a link error, which is the intended
way to discover what a new protocol file still needs.

Each request struct carries its wire fields plus a `Handle(Client&)` the server
implements; a reply is a sibling struct the request's `Reply` method encodes; an event is
a struct with a `Send` method. A request's value-list (the X "mask + list" idiom) becomes
`Optional<>` members, and variable-length lists become spans into the read buffer, so a
handler reads its arguments as ordinary fields. The codec is little-endian only;
a connection that announces big-endian byte order is refused at the setup handshake,
because every client that reaches this host is little-endian and supporting both doubles
the codec for no benefit.

Extensions are declared in nested namespaces (`x11::shm`, `x11::render`, `x11::dri3`,
`x11::present`, `x11::bigreq`, `x11::xc_misc`). The generator assigns each a major opcode
and a first-event and first-error base and emits them as constants, so the dispatcher and
`QueryExtension` agree without a hand-maintained table. XKEYBOARD (`x11::xkb`,
`src/x11_xkb.cpp`) is the one extension that is not generated from XML: its replies are
synthesized from the shared keymap and the handful of request fields it reads are fixed
offsets, so generated decode structs would add nothing (see Keyboard layout below).

## Threading and resources

The server runs on Automat's `mux::Epoll` thread, the same thread the Wayland compositor
uses. The listening socket and every client socket are `mux::Epoll::Listener`s. A client's
requests are read, dispatched and the replies flushed in one pass; board structure is
mutated only on the UI thread in `x11::Tick()`, exactly as for Wayland.

On Linux the server listens on a filesystem UNIX socket. On Windows it listens on TCP
loopback, display number n mapping to port 6000+n, because Windows AF_UNIX sockets cannot
carry file descriptors. TCP provides no SO_PEERCRED, so the client's process id — the launch-matching
hint — is recovered from the system TCP table by locating the
mirrored connection. TCP also carries no file descriptors, so the SHM and DRI3 display
paths never activate on Windows and clients paint through plain protocol requests.

X resources — windows, pixmaps, graphics contexts, colormaps, cursors, fonts, and the
RENDER pictures and glyph sets — are stored in one server-global table keyed by XID, because X
resources are shareable across clients and a client may name another's resource. Each
resource records its owning client so a disconnect frees exactly that client's resources.
Freeing a window frees its children; the recursion snapshots the child ids and erases the
window by key afterwards, because the recursive frees rehash the table and mutate the
child list, which would invalidate an iterator or index held across them.

A window or a pixmap is a drawable and owns a raster `SkSurface` as its backing store, so
core drawing and RENDER are both ordinary Skia drawing. A top-level window's board
snapshot is the composite of its own backing store and its mapped child windows, taken as
an `SkImage` and handed to the board object under its mutex — the same handover a Wayland
surface uses, so the renderer treats an X11 window and a Wayland window identically.

## Acting as a window manager

An X11 toolkit behaves differently depending on whether a window manager is present, and
it will not paint its first frame until it detects that it is managed. Automat therefore
presents a minimal window-manager identity: it owns a `_NET_SUPPORTING_WM_CHECK` window
and advertises `_NET_SUPPORTED`, and when a top-level maps it sets `WM_STATE` to Normal,
reports zero `_NET_FRAME_EXTENTS`, and synthesises the post-map `ConfigureNotify` that a
real manager would send. Without these a GTK client maps its window and then remains idle,
never running its draw cycle.

On the board, a mapped top-level is drawn with the shared window chrome
(`src/window_frame.hpp`) — the same frame the Wayland compositor draws for its
server-side-decorated toplevels — with the window title drawn on it. The title and `WM_CLASS` fallback are seeded
from the window's properties when the board object is created, because clients set them
before mapping; later `WM_NAME`/`_NET_WM_NAME` changes update it live. Override-redirect
windows (menus, tooltips) stay bare.

Whether a top-level is framed is negotiated the way the Wayland compositor negotiates
decoration modes, translated to X11 conventions: a client that draws its own frame says so
with `_MOTIF_WM_HINTS` (a decorations field of zero) or, for GTK client-side decorations,
by setting `_GTK_FRAME_EXTENTS`, and Automat then leaves it bare rather than double-framing
it. The user can override the negotiation from the window's right-click menu, whose
"Decoration..." submenu (Auto / Automat / App) is one implementation shared with the
Wayland compositor: both window objects implement `DecoratedWindow` (`src/window_frame.hpp`)
and persist the choice under the same `decoration` key. A window whose client is gone is
always framed, so a ghost waiting for its launch stays visible and draggable.

A bare window is moved by its own titlebar: the client turns the titlebar press into a
`_NET_WM_MOVERESIZE` ClientMessage sent to the root window, which is how EWMH clients ask
the window manager to take over a drag. `_NET_WM_MOVERESIZE` is advertised in
`_NET_SUPPORTED` because GTK checks the list before using it. The `SendEvent` handler
intercepts the message (the root is Automat itself, so nothing would deliver it), queues
the window and wakes the root widget, and the next `Tick` calls `StartClientMove`
(src/window_frame.hpp) — the same shared function the Wayland compositor uses for
`xdg_toplevel.move` — which replaces the action routing the pressed button to the client
with the standard board drag. The resize directions are ignored because embedded windows
are not resizable, and cancel is ignored because the move ends with the physical button
release.

Two wire rules that clients depend on. A client-supplied
event delivered by `SendEvent` must have its sequence-number field overwritten with the
receiving client's current sequence; the client fills that field with a meaningless value,
and xcb widens 16-bit event sequence numbers by comparing against the last one it saw, so a
stale value reads as an enormous jump and xcb declares the connection inconsistent and
disconnects. The keymap handed to clients through `GetKeyboardMapping` and
`GetModifierMapping` is flattened from the shared keymap rather than built from a default
layout, for the reason given in the Wayland document: keycodes pass through Automat
verbatim, so a synthesized layout garbles every non-QWERTY keyboard.

## Keyboard layout: XKB

The core keymap reply satisfies programs that read the keyboard through the core protocol,
but GTK and every client built on xkbcommon require the XKEYBOARD extension and will either
refuse to start or fall back to a compiled-in US layout without it.

The layout itself is stored outside the server, in the process-wide shared keymap
(`src/keymap.hpp`): an `xkb_keymap` built from whatever the platform offers — the host X
server's layout when Automat runs as an X client, the OS layout on Windows and macOS, a
compiled default otherwise. Both embedded servers express the same object, the Wayland
compositor by serializing it to text and the X11 server by encoding it onto the XKB wire
(`src/x11_xkb.cpp`), so keyboard behavior is identical whichever protocol a client speaks,
and neither server assumes any particular windowing system exists underneath Automat.

The wire encoding answers the requests a toolkit issues to build its keymap: `GetMap`,
`GetNames`, `GetControls`, `GetCompatMap`, `GetIndicatorMap`, `GetState`, `PerClientFlags`,
`GetDeviceInfo`, plus `GetGeometry` (answered "not found") and `GetKbdByName` (answered
"not loaded", which sends the client down the piecewise path it already handles). Key
types are deduced per key and group by asking xkbcommon which modifier combinations reach
each shift level, then deduplicated into a table shared by `GetMap` and `GetNames` so
structure and labels always agree. The keymap is expressed in real modifiers only: xkbcommon
resolves virtual modifiers (LevelThree, NumLock, ...) to the real masks the key events
already carry, so clients pick the correct shift level without any virtual-modifier
machinery. Two reply details are required: `GetMap`'s `present` and `GetNames`'
`which` must echo every component bit the client asked for, even the ones that are empty
here (actions, behaviors, virtual-modifier names), because xkbcommon-x11 refuses the whole
keymap otherwise; and the compat map carries a single catch-all interpretation ("any keysym,
any modifier: set the key's modmap modifiers"), because Xlib cannot parse an entirely empty
compat reply — it restates what the modifier map already says. The Set-family requests are
accepted and ignored: the synthetic keyboard has nothing to reconfigure, and an embedded
client must not affect the rest of the system.

The keymap can define several groups (layouts), and which one is active is not part of the
keymap but of the keyboard state. The active group is therefore carried in each forwarded
key event's modifier state — the effective modifier mask and the two group bits a client
reads with `XkbGroupForCoreState`, both taken verbatim from the originating platform input
event (`ui::Key`) — so typing follows the current layout and a runtime switch takes effect
on the next key. What is not sent is the XKB state-change event, so a client that tracks
the layout outside of keypresses does not update; this keeps the extension purely
request-driven.

## Displaying pixels: SHM and DRI3

Clients hand over rendered frames two ways. MIT-SHM is the common one: a client renders
into shared memory and the server reads it directly. Both segment styles are supported —
the classic System V segment attached by id (`shmat`), which is what libXext's
`XShmAttach` uses and therefore what GTK's cairo backend uses, and the file-descriptor
segment of the later protocol version. Without the System V path a GTK client's shared-memory
probe succeeds but its real frames never arrive, so the window stays blank.

DRI3 with Present is the GPU path, the X11 equivalent of `zwp_linux_dmabuf_v1`: a client
passes dmabuf file descriptors, the server imports them into a GPU-backed `SkImage` through
`vk::ImportDmabuf` — the same importer the Wayland compositor uses — and Present displays
the imported pixmap. As on the Wayland side, an all-software Mesa stack gives ordinary
clients only shared memory and never emits dmabuf, so this path serves clients that
allocate their own buffers or a host with a real GPU.

## Drawing

Core drawing requests map onto Skia: rectangles, lines, arcs and polygons become Skia
draws with the graphics context's colour and line width, `PutImage` blits client pixels
into the target's backing store, and `CopyArea` copies between drawables. The RENDER
extension, which is how a cairo-based toolkit draws, maps the same way: a picture
wraps a drawable or a solid or gradient source, `Composite` and `FillRectangles` become
Skia draws under the requested Porter-Duff blend mode, trapezoids become filled paths, and
glyph sets become cached images a glyph run draws with the source colour. This is enough
for GTK and cairo; the rarely-used parts of RENDER (transforms beyond the common cases, filters)
are present only where a client needs it.

## Window objects and lifetime

A mapped top-level becomes a `library::X11Window`, which the board Locations own and the
server holds only weakly — the same ownership rule as the Wayland window, required
for the same reason: a drag that leaves a board extracts the Location into the pointer, so
"is it still on a board" is not a safe liveness test. Deleting the object asks the client to close through
`WM_DELETE_WINDOW`, falling back to a signal to the client's process. A client that
disconnects or unmaps has its window removed by `Tick`. Persistence reuses the launches
system (`Launches.md`): the argv that mapped a window is saved, and a restored or copied
window gets a Launch whose client is matched back to it by process id.

The Command object sets both `WAYLAND_DISPLAY` and `DISPLAY` in the environment of the
programs it launches, so a child picks whichever protocol it prefers and an X11-only
program still finds Automat's X socket.
