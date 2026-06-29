# Wayland Compositor

## What it is

Automat is itself a Wayland compositor: processes started by the Command
object connect to Automat's socket, and every window they map becomes a
"Wayland Window" object on the board — draggable, deletable, persistable
like any other object. This turns external programs into board citizens
instead of things that live outside the canvas.

Code map: `src/wayland.hpp` (the public surface: the board window object and the four compositor
lifecycle calls), `src/wayland_protocol.hpp` (the protocol interface definitions, `Server` and
`Client`, included only by the implementation and the generated dispatcher),
`src/wayland.cpp` (the protocol runtime, the compositor proper, and the board object with its toy),
`src/wayland.py` (protocol code generation and validation),
`build/generated/wayland_generated.{hpp,cpp}` (generated forward declarations and wire code).

## Protocol stack

The compositor speaks the Wayland wire protocol directly; there is no libwayland and no
third-party scanner. Each Wayland interface is one plain `struct Interface : Common` in the
hand-written `src/wayland_protocol.hpp`.

`src/wayland.py` owns the half of each struct that follows mechanically from the protocol XML -
the request and event declarations, the enums, the accounting metadata - and emits it as a block
fenced by `clang-format off/on: generated <Interface> from <xml>` markers. On build it
re-derives that text and compares it to `src/wayland_protocol.hpp` exactly; a mismatch fails the build and
writes `src/wayland_protocol.hpp.fixed` (the generated blocks corrected and any missing interface added in
dependency order, every hand-written field kept), which the developer diffs in. It also writes
`build/generated/wayland_generated.{hpp,cpp}`: the interface forward declarations and the wire
code - event encoders and the dispatch that reads a request, validates its new-id arguments,
creates the argument objects, and calls the handler (a destructor request also destroys its
object).

The runtime is small and entirely in `src/wayland.cpp`: the listening socket, the per-client
read-dispatch-flush loop, the `wl_display` and `wl_registry` globals, object-id bookkeeping, and
the compositor proper - every request handler, the surface-to-board scene mirror, the input
senders, and the per-frame reconcile.

The socket is claimed the way libwayland claims it: an exclusive `flock` on `wayland-N.lock`
guards each display number, so a live server's number is skipped while a socket a crashed or
interrupted run left behind is reclaimed (the dead process released its lock) and rebound. A clean
shutdown additionally unlinks both the socket and its lock file in `~Server`. Without this the
sockets would pile up.

Per-interface storage is the point of the bespoke stack. Below its generated block each
interface struct in `src/wayland_protocol.hpp` carries its own hand-written fields, so a surface's role
chain, a buffer's planes, or a params object's accumulated plane fds live on the object itself. Cross-object links are typed
pointers, each nulled in the peer's destructor; object lookup goes through the client's
id table or the per-type colony that owns every live object of a type. This avoids the
resource-to-object maps and linear scans a libwayland compositor needs.

## Threading

The compositor runs on Automat's `mux::Epoll` thread and every protocol object lives in a
per-type colony owned on that thread. One epoll loop multiplexes every descriptor the
subsystem cares about, each wrapped as a `mux::Epoll::Listener`:

- the listening socket, and one socket per connected client
- cross-thread work posted onto the thread (`Epoll::Post`)
- the frame-callback timer
- one close-escalation timer per toplevel asked to close

Pixels flow out as a tree of surface objects that mirrors the Wayland surface
tree, so Automat's own widget compositing lays a window out. Every `wl_surface`
becomes a `WaylandSurface` object
holding its own imported `sk_sp<SkImage>`; a surface owns its subsurfaces and
popups as `Ptr<WaylandSurface>` children, and the toplevel surface is the board's
`WaylandWindow`, which derives from `WaylandSurface`. Each committed `wl_shm`
buffer is row-copied once into a pooled allocation (the pool entry is reclaimed
when the previous frame's image lets go) and the raster image wraps it without
further copies; a dmabuf becomes a GPU-backed image instead. Both imports happen
on the compositor thread (see GPU buffer passing below), which writes each
object's texture and child list under that object's mutex.

Board structure is only mutated on the UI thread, in
`UIFrame()` — called once per frame from `RootWidget::Tick` — which inserts
newly mapped windows (seated next to the Command that spawned them) and
removes windows whose client went away; the child surface objects below a window
are created and dropped by the compositor, off the board.

One renderer fact matters to anyone extending this: an idle Automat does not
tick the root widget. Every path that needs `UIFrame` to run (window
appeared, window disappeared, the compositor itself starting while restored
windows wait to respawn) must call `vm.WakeToys()`, which `PackFrame`
watches.

## Protocol surface

Implemented: `wl_compositor`, `wl_subcompositor` (subsurfaces are composited
into the window's surface tree, see below), `wl_shm`,
`wl_output`, `wl_seat` (pointer + keyboard), `wl_data_device_manager` (clipboard
selection, see below), `xdg_wm_base` with toplevels and popups (menus, see
below), `zxdg_decoration_manager_v1` for per-window decoration negotiation, `zwp_linux_dmabuf_v1` for GPU buffer passing
(see below), `wp_viewporter` for surface cropping and scaling (see below), and
`wp_cursor_shape_v1` for named cursors.
foot requires the subcompositor and data device manager to even start; kitty
requires the decoration ordering rule below; Firefox additionally needs popups,
input regions, scroll and clipboard (see below).

Two protocol rules earned through debugging:

- The initial commit handshake must send `xdg_toplevel.configure` first and
  `xdg_surface.configure` second, and *nothing else* may send an
  `xdg_surface.configure` before that bundle. The decoration object
  confirming a mode pre-map must send only its own configure event; GLFW
  clients (kitty) tear down silently otherwise. A popup's first commit sends the
  same kind of bundle for its own xdg_surface: `xdg_popup.configure` (the
  resolved geometry) then `xdg_surface.configure`.

## GPU buffer passing

Clients hand the compositor GPU buffers through `zwp_linux_dmabuf_v1` rather
than copying pixels through `wl_shm`. The global is advertised at version 4
because the version 4 feedback events are the only way a Mesa client learns
which DRM device to allocate its buffers on; without that information (and
without the obsolete `wl_drm` global) Mesa falls back to `wl_shm`. The feedback
names the render node as the main device and offers one tranche of `AR24`/`XR24`
with the linear and implicit modifiers; version 1-3 clients receive the legacy
format and modifier events instead. Both formats import as
`VK_FORMAT_B8G8R8A8_UNORM`.

A committed dmabuf becomes the same `sk_sp<SkImage>` the shm path produces,
imported on the compositor thread at commit so a dmabuf frame travels the same
field as an shm frame. Import takes one of two paths: a zero-copy Vulkan
external-memory import when the driver can import the buffer, and a mapped-upload
fallback when it cannot. The zero-copy path builds a GPU texture, which needs a
Graphite recorder; the compositor uses its own recorder, separate from the
renderer's - not a second Graphite context, because Graphite images are
context-local, so an image built on a second context could not be drawn by the
renderer, whereas a second recorder on the shared context yields one that can.
The two recorders are each confined to their own thread and never used at once,
so no lock is needed. A dmabuf buffer is held until the next frame replaces it
before being released, because the producer must not reuse it while the GPU is
still sampling it; this is the zero-copy lifetime the shm copy-release avoids.

The two import paths exist because of what a host without a real GPU can do.
With llvmpipe and lavapipe on a virtio-gpu that has no 3D acceleration, Mesa's
WSI gives ordinary clients (terminals, Chromium) only `wl_shm`; they cannot
emit dmabuf at all. A client that allocates its own buffers through GBM on the
primary node, such as the weston dmabuf demo, does emit dmabuf, but lavapipe
cannot import that buffer zero-copy and so takes the mapped-upload path. A host
with a real GPU imports every client's dmabuf zero-copy.

The protocol and feedback are in `src/wayland.cpp` (`SendDmabufFeedback`, the
`LinuxBufferParamsV1` handlers, `ImportBufferDmabuf`), the import in `src/vk.cpp`
(`ImportDmabuf`), and the plane description it consumes in `src/dmabuf.hpp`.

## Surface cropping and scaling

`wp_viewporter` separates a surface's displayed size from its buffer's pixel
size: a source rectangle selects part of the buffer and a destination size sets
the surface size that rectangle is scaled to. Clients use it to render at a
fractional scale, or to hand over one oversized buffer and present a sub-region
of it. Automat needs it because the displayed size, not the buffer size, is what
a window object occupies on the board and what pointer input is measured
against.

The crop and scale state lives on the `wp_viewport` object rather than the
surface. Destroying the viewport then removes the crop and scale at the
surface's next commit, which is what the protocol requires, and the surface's
teardown only has to orphan the viewport. The state is resolved at commit, once
the just-attached buffer's size is known: resolution yields the surface size -
the destination, or else the source size, or else the buffer size - and the
buffer rectangle to sample. A surface with no viewport resolves to its buffer's
size sampled whole, so it carries no special handling. The two violations the
protocol defers to commit, a non-integer crop with no destination and a source
rectangle reaching outside the buffer, raise the protocol error there; the
cheaper checks on negative or zero values raise it when the request arrives.

The resolved surface size becomes the window object's size and the source
rectangle travels with the frame to the toy, which samples it into the content
area. Because both the window's board size and its surface-local input mapping
derive from the surface size, a cropped or scaled window drags, seats and routes
pointer events correctly with no further change. Resolution and the protocol are
in `src/wayland.cpp` (`ResolveGeometry`, the `Viewport`/`Viewporter` handlers);
the sampling is in `src/wayland.cpp`.

A commit that changes only the viewport while attaching no new buffer does not
resize the window until the next buffer arrives, because resolution runs on the
buffer path. Clients that change their crop or scale attach a buffer in the same
commit, so this is not observed in practice.

## Subsurface compositing

A client may split a window across several surfaces: `wl_subcompositor` turns a
wl_surface into a subsurface of a parent, positioned relative to it and stacked
in the parent's tree. Toolkits that render through a GPU compositor use this for
their main content - Firefox and Chromium put their whole page in a child surface
- so a compositor that ignores the subtree shows only the parent, which for those
clients is an empty decoration frame. This is why the feature exists: it is what
separates "maps the window" from "shows the page".

Each surface keeps its own committed image on a `WaylandSurface` object, and a
parent surface owns its subsurfaces as `Ptr<WaylandSurface>` children; at every
commit that changes the visible tree the compositor updates this object tree to
match the Wayland tree (`UpdateSurfaceNode`). Each surface's toy draws only its own
texture and hosts a child toy per child surface, so Automat's renderer composites
the subtree the way it composites any widget tree, each surface a node with its
own cached texture. A parent with a transparent centre (a client that draws its
own decorations) shows its child's content through it. The window extent and board
size come from the toplevel surface; subsurfaces are placed within it, and each can
be stacked above the parent's own content or below it - a surface object keeps a
child list on each side, drawn around its own texture, so a subsurface placed below
an opaque parent is hidden, as the protocol requires. The offset of a child within
its parent lives on the parent's child list, not on the child, so the toy reads a
surface's geometry and its children as one consistent snapshot under one mutex.

Subsurface state is double-buffered against the parent. `set_position` and, in
sync mode (the default), the child's committed buffer apply at the parent
surface's commit; a desync child - what browsers use, so content updates are not
gated on a parent repaint - applies its commits immediately. The compositor holds
a sync child's committed state in a cache until the parent commits, then applies
it recursively. A surface is rejected as its own ancestor at `get_subsurface`, so
the tree walks stay finite.

Pointer input reaches the topmost surface at the cursor that accepts input, which
need not be the toplevel and need not be the surface visually on top. Toys
forward enter, leave, motion, button and axis to their surface's clients in
surface-local coordinates (see Input pass-through). Keyboard input goes to the
toplevel surface instead; see Input pass-through for why a subsurface must not hold
keyboard focus.

The tree, the sync/desync rules and stacking are in `src/wayland.cpp`
(`DoCommit`, `ApplyChildren`, `UpdateSurfaceNode`, `SyncWindowTree`, the
`Subcompositor`/`Subsurface` handlers); the per-surface objects, their toys, the
texture drawing and the per-surface input routing are in `src/wayland.cpp`.

## Popups (menus)

A client opens a menu, dropdown, autocomplete list or context menu as an
`xdg_popup`: a transient child surface placed by an `xdg_positioner` relative to a
parent `xdg_surface` (a toplevel or another popup). A popup becomes a
`WaylandSurface` child of its parent surface object, in the above-content list. The
compositor does not pin a final position: it encodes the positioner onto the edge -
the anchor/gravity/offset position, the alternative mirrored about the anchor, and
the client's flip and slide permissions - and the popup's toy resolves the
placement against the on-screen viewport, keeping the popup visible while letting it
extend past the window edge. That decision belongs to the UI layer because the
window is a movable, zoomable board object whose on-screen position only the UI
layer knows. Hit-testing checks popups before the toplevel tree because they draw on
top.

Dismissal does not rely on the popup grab. Firefox shows context menus on button
release, by which point the implicit grab serial is gone, so it never calls
`xdg_popup.grab` (Weston behaves identically). The compositor therefore dismisses
the topmost popup whenever a press lands on a surface that is not itself a popup
(`popup_done`); a popup that *does* grab additionally takes
keyboard focus so the menu can be driven from the keyboard.

The positioner is computed and encoded in `src/wayland.cpp`
(`ResolvePopup`, `UpdateSurfaceNode`, `XdgSurface::OnGetPopup`) and resolved against the
viewport in `src/wayland.cpp` (`PlacePopup`), which flips then slides a popup
that would fall off screen, honoring the client's permitted adjustments so a menu
opened near a screen edge stays usable.

## Clipboard

Copy and paste use the `wl_data_device` selection. A client copies by offering a
`wl_data_source` (with its mime types) and setting it as the selection; the
compositor stores it and hands every data device a data offer advertising those
types, so any client can paste. A paste's `wl_data_offer.receive` is brokered to
the owning source's `wl_data_source.send`, relaying the destination's fd for the
source to fill. Copy and paste within one client - a URL copied from a page and
pasted into the address bar - round-trips through this. The host X11 clipboard is
not bridged, so copy between Firefox and an X11 application does not cross. The
broker is in `src/wayland.cpp` (`SetSelection`, `OfferSelectionTo`, the
`DataDevice`/`DataSource`/`DataOffer` handlers); the server-allocated data offer it
hands out is the one place the compositor creates an object the client did not.

## Input pass-through

A press becomes a `wl_pointer` button (an `Action` lives for the duration of the
press, so drags select text in a terminal instead of moving the object); hover
motion with no button held is forwarded too (the toy watches the pointer via
`PointerMoveCallback`), so links and menu items highlight; the scroll wheel
becomes `wl_pointer.axis` over the content and falls through to canvas zoom over
the chrome. The toy hands the compositor the surface plus surface-local
coordinates; the compositor resolves the surface handle and relays the event.
The title bar and frame fall through to the standard object behaviors (drag, menu).

A surface's reactive shape is its `set_input_region`, not its texture, so a
surface with an empty input region is transparent to the pointer and the hit falls
through to whatever is behind it. This is load-bearing for GTK clients: Firefox
renders its page into a content subsurface but marks that subsurface
input-transparent and takes all pointer input on the toplevel surface, so routing
by visual stacking would deliver clicks, scroll and motion to a surface the client
silently discards them on.

Keyboard focus is the toplevel surface, never a subsurface under the cursor.
Toolkits route keyboard input through the window's main (xdg_toplevel) surface
and ignore a `wl_keyboard.enter` delivered to a content subsurface, so a browser
whose page lives in a subsurface would drop every keystroke if focus followed the
pointer. Focus is still gated by Automat's caret system: clicking the content
requests a caret owned by the window toy; the familiar blinking I-beam then morphs
into an underline inside the title band — carets draw black, so the focus shape
lives on guaranteed-light chrome, never on client pixels, which are arbitrary and
often dark. While the caret exists, `KeyDown`/`KeyUp` pairs are forwarded to the
toplevel surface (or to a popup that holds a grab); focus moves the way carets
always move — clicking any text field steals it (a keyboard leave is sent), and
Escape releases it. Escape therefore never reaches the client today. Candidate
future treatments, in rough order of appeal: a compositor-reserved chord
(the compositor reserves Super for itself, following Hyprland, so
Super+Esc could mean "send a literal Escape"), holding focus through the *first*
Escape and forwarding it while a quick second press defocuses, or a
per-window passthrough toggle on the title bar.

Keycodes pass through Automat verbatim: an X11 keycode becomes an `AnsiKey`
and back through the fixed tables in `src/x11.cpp`, then minus 8 to evdev.
Because of that, the keymap advertised to clients must be the host X
server's actual keymap, fetched with `xkbcommon-x11` — building a default
"us" keymap garbles every non-qwerty layout (the development host runs
Programmer Dvorak, which is how this rule was learned). Modifier masks are
taken from the same keymap and sent before every key event.

Not yet routed: the relative-pointer / pointer-constraints used for pointer lock
(Firefox binds them but works without).

## Window objects and lifetime

The compositor holds only a `WeakPtr` to the window object; the board
Location owns it. This is load-bearing: dragging an object *extracts* its
Location from the board, so any "is it still on the board" check
false-positives mid-drag, and killing the client on that false positive would
destroy a window the user is only moving. The object's destructor is the true
deletion signal — black hole and bubble-menu delete both end there — and it asks
the client to close (`xdg_toplevel.close`), with SIGTERM two seconds later if the
client ignores it. The client pid for the escalation comes from the socket's
`SO_PEERCRED`.

In the other direction, a client unmapping or disconnecting queues the
window for removal; `UIFrame` erases its Location.

Persistence (recipes, respawn, adoption of the respawned client into the
existing object) is documented in `Wayland Client Persistence.md`.
