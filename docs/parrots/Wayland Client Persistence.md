# Wayland Client Persistence

## The problem

Automat objects survive restarts: the board is serialized into
`automat_state.json` and recreated on launch. A Wayland window object wraps a
live operating-system process, which does not survive anything — its memory,
file descriptors and compositor connection die with it. "Saving a window"
therefore cannot mean one thing; it means choosing a point on a spectrum
between saving the *intent* that produced the window and saving the *complete
runtime state* of the process behind it. The same question decides what
cloning a window means.

This document lays out the options that were considered, their entailments,
and which one Automat implements today. The implementation lives in
`src/library_wayland_window.{hh,cc}` (recipe storage, respawn flag) and
`src/wayland_compositor.cc` (respawn and adoption in `UIFrame`).

## Option 1: Recipe — save the argv, re-run it (implemented)

Save the argv that produced the window (as a JSON array — one element is one
argument, spaces and all), plus where the window sits on the board.
Deserializing re-runs it against Automat's compositor; cloning runs a second
instance. Runtime state — terminal scrollback, an unsaved document, a
half-played video — is lost, and nothing pretends otherwise.

A subtlety worth keeping: when the re-run client maps its first window, it
must become *the same object* on the board, not a second one. Automat does
this with an adoption table: the respawn records the new child's pid, and the
compositor routes that client's first toplevel into the existing object,
preserving its position and connections. Without adoption, every save/load
cycle would duplicate windows.

The window also keeps a real `Launcher` connection to the Command whose
child mapped it — a visible cable, serialized through the ordinary links
mechanism. On restore, the respawn goes *through* that Command
(`Command::AdoptiveLaunch`): the Command spawns its own argv, synthesizes
the running state the way Timer resumes from a save, and therefore keeps the
pid readout and STOP control over the restored child. The window's own
recipe is the fallback for windows whose Command is gone or already busy
(a cloned window whose original still runs, for example — the clone
self-spawns a second instance while the Command keeps owning the first).

How the association is captured matters too. The window asks "which Command
has a child with my client's pid?" at mapping time. This only works for
directly spawned clients; a process that forks before connecting (a browser
launcher, a daemonizing app) breaks the pid match and the window saves
without a recipe or link, loading as a dead ghost. The protocol-level
hardening for this is xdg-activation: the launcher obtains a token from the
compositor, passes it through `XDG_ACTIVATION_TOKEN`, and the client
presents it on its first window — an association that survives forking
because the token, not the pid, carries the identity. Worth implementing
when fork-happy clients start to matter.

Why this option won: it is fully implementable with what a compositor
legitimately knows, it never lies about what survived, it composes with the
Command object (the recipe *is* the Command's argv), and its failure mode is
visible (a ghost) rather than mysterious (a half-restored process).

## Option 2: Ghost snapshot — recipe plus the last frame

Identical to the recipe, plus the last committed buffer saved as an image.
The deserialized window shows the frozen picture until the user re-runs it —
the board "looks right" immediately after loading.

The risk is that a screenshot of a terminal looks exactly like a live
terminal. The ghost must be visibly dead (desaturated, hatched, or marked
with a "frozen" sign) or users will type into a window that no longer
exists. Automat currently draws
recipe-less ghosts with a gray hatch instead of content; adding the frozen
frame would be a small extension (serialize `pixels` as a sidecar image, the
way `LeptonicaImage` persists its bitmap).

## Option 3: CRIU — checkpoint the real process

CRIU (Checkpoint/Restore In Userspace) can freeze a process tree to disk
images and restore it later, preserving memory, threads, open files and
sockets. This is the only option that genuinely saves the half-written
document.

The hard parts are exactly the resources that cross Automat's boundary:

- **The compositor socket.** A restored client holds a unix socket fd whose
  other end died with the old Automat. CRIU supports "external" fds, but the
  protocol state machine (bound globals, object ids, buffer handshakes) lives
  in both peers; Automat would need to checkpoint its per-client protocol
  state alongside the process and rebuild resource tables on restore, or
  interpose a proxy process that can reconnect transparently (the approach
  used by waypipe). Either is a research project, not a feature.
- **Pids, ptys, GPU fds.** Terminals own pseudo-terminals with shells whose
  children form a tree; GPU clients own DRM fds. Each adds restore
  constraints; some (GPU contexts) are effectively non-restorable today.
- **Clone.** CRIU restore into two copies requires rewriting pids and every
  resource both copies would share. Possible for self-contained processes,
  undefined for anything holding sockets.

CRIU fits best as an *optional* deep-save for cooperative, self-contained
processes, layered on top of the recipe (if restore fails, fall back to
re-running). The fit with Automat's expose-the-low-level
philosophy is good; the engineering cost is the reason it is not built.

## Option 4: Virtual machine snapshot — change the unit of persistence

Run clients inside a lightweight VM (Firecracker, QEMU microVM, or a
gVisor-style sandbox) whose display is forwarded to Automat. Snapshotting and
cloning become first-class: the hypervisor serializes the whole guest, and a
restored or cloned guest reconnects its display channel the way any VM
console does.

This is the only option where *clone of a running process* is well-defined —
fork the VM. The costs: a guest kernel per client (memory), display
forwarding latency, a much larger moving part to maintain, and the loss of
direct kernel-object composition (the client's pid is no longer a host pid a
Command can wait on). It turns "Automat as compositor" into "Automat as
hypervisor console" — a different product. Worth revisiting if Automat ever
wants untrusted-code sandboxing anyway; the persistence then comes free.

## Option 5: Protocol replay — record the Wayland session

Record every request the client ever sent (the way `WAYLAND_DEBUG` prints
them) and "restore" by replaying. This works for stateless toys and fails
for everything real: replay re-executes outputs, not inputs — the client's
decisions depended on timing, file contents and randomness that replay does
not reproduce, and the client process itself still has to exist to continue
past the recording. Replay is the right tool for *testing* compositors (a
recorded session is a reproducible test case) and the wrong tool for
persistence. Noted here mainly so future readers do not rediscover it.

## Option 6: Zygote / fork-adoption — clone without restore

For clone specifically (not save): keep a paused, pre-connection copy of the
process (a zygote) and fork it on demand, so the clone shares the recipe's
startup cost but diverges from a warm state. Wayland makes the naive version
illegal — a forked client would share one socket with its parent and corrupt
the protocol stream — so the zygote must pause *before* connecting, which
limits how warm it can be. Browsers use this internally; a compositor doing
it generically would need client cooperation. Filed as an idea that only
pays off for slow-starting applications.

## Where this leaves Automat

The recipe with adoption is implemented and is the right default: simple,
cheap, composable. The natural next steps, in order of value per effort:

1. **Ghost snapshot** (option 2) — small, makes loaded boards legible.
2. **CRIU for cooperative processes** (option 3) — large, real value for
   terminals and editors, needs a proxy-or-reconnect story for the socket.
3. **VM-backed clients** (option 4) — a strategic decision, not a feature.

The window's serialized form deliberately stays small and readable —
`{"recipe": "foot", "title": "foot"}` — so that whichever deeper option
arrives later can extend it without migration pain.
