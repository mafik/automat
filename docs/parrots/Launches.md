# Launches

## The problem

Automat starts operating-system programs from several places: the Command
object runs its argv, restoring a saved board revives the applications that
were running, and copying a live window starts another instance of its
program. Each of these produces a child process whose windows must land in
the right place: next to the Command that started it, or inside the exact
window object that was saved or copied. Nothing in POSIX connects a spawned
process to the windows it produces — and single-instance applications break
even the obvious process-id association, because the spawned process forwards
its launch over the session D-Bus to an already-running primary process and
exits, after which the window maps on a connection whose process id never
matches the spawned one.

The launches system is the one mechanism behind all of these flows. It is
implemented in `src/launcher.hpp` and `src/launcher.cpp`.

## The object model

Three kinds of objects divide the work:

- **Command** (`src/library_command.*`) is the specification and the
  controls: the argv, RUN and STOP, the stdio ports, the exit chip. A
  pending launch is a Command.
- **Launch** (`src/launcher.hpp`) is one running instance: the POSIX child
  (pid, exit status, stream captures) combined with one XDG activation
  sequence (the token). A Launch is created only by actually spawning; an
  unlaunched Launch does not exist. It is an `Object`, so it is
  reference-counted, thread-safe to point at, and has a widget.
- **WaylandWindow and X11Window** hold what the display servers report.
  Their shared launch-related part — title, app_id, recipe, client pid, the
  Launcher cable, copy and serialization behavior — is the `ClientWindow`
  base in `src/launcher.hpp`.

A fourth object is anticipated but not implemented: an Application object
built from a .desktop entry, acting as another launch source with an icon and
D-Bus activation. `Launch::source` is a generic `WeakPtr<Object>` so that
source does not have to be a Command.

## The Launch record and matching

`Launch::Spawn` is the only spawn path. It filters empty argv words, injects
the environment (`WAYLAND_DISPLAY` and `DISPLAY` point at Automat's own
servers, `GDK_BACKEND` is dropped so toolkits pick their preferred backend),
mints a random 32-hex-character activation token
(`MintActivationToken`: `RandomBytesSecure` + `BytesToHex`), exports it as both
`XDG_ACTIVATION_TOKEN` (the name the protocol specifies, read by GTK 4) and
`DESKTOP_STARTUP_ID` (the X11-era name — GLib forwards it through the
GApplication handoff under the `desktop-startup-id` key, which is the only
key GTK 3 reads, and GTK 3 on Wayland passes that value to
`xdg_activation_v1.activate`), spawns via posix_spawnp, and registers the
Launch in a process-wide index.

The index is a `Vec<WeakPtr<Launch>>` behind one mutex in launcher.cpp — an
index of objects, not a data store. `Launch::Find(pid, token)` answers the
one question the display servers ask when a new client window appears: which
launch does it belong to? A token match wins over a pid match, because the
token survives the single-instance handoff and pid does not; the pid is a
hint that works for directly spawned clients. A launch with no matching
window stays in the index; being a WeakPtr, its entry dies with the
object and is swept during later spawns.

A launch either targets an existing window or produces new ones:

- `Launch::restoring` names a `ClientWindow` waiting to be filled — a ghost
  restored from a save or a fresh copy. When the matching client maps, the
  server fills that object in place, preserving its Location, cables and
  every reference other objects hold to it.
- With no `restoring`, each window the client maps becomes a new window
  object. It records `launch.argv` as its recipe (the argv actually spawned,
  not the Command's possibly edited-since text), keeps `Ptr<Launch>
  launched_by`, is inserted into the source's Board with a
  `Location::PlaceBeside` request naming `launch.source`'s Location —
  resolved UI-side from the actual widget shapes, the servers computing no
  coordinates — and receives a Launcher cable to it. This replaced the
  earlier board scan for a Command whose child pid matched; the launch
  carries the association directly, and it keeps working after the child
  exits or forwards, which the pid scan could not do.

`launched_by` is also what keeps a launch alive: it stays findable for as
long as the Command holds it or any window it produced exists, so an
application opening another window hours later still associates correctly.

## Command and the launch

A Command holds a single `Ptr<Launch> launch` — the current run, or the last
one after it exits. Re-running replaces it, which is also what bounds the
capture buffers: one run, one set of buffers, overwritten by the next run.
The exit chip and the stdout meters read through this launch.

The launch is displayed as an icon: a gear with the pid under it, rotating
and saturated while the process runs, still and desaturated after it exits.
One widget draws it everywhere (`LaunchWidget`, `src/launcher.hpp`): the
Command plate embeds it small, and an extracted launch shows it standalone. The desaturation deliberately changes nothing beyond the
display: matching does not depend on the launch's state, so a window
arriving after the process ended — the nemo forward, a slow application —
still finds its launch and lands correctly. There is no way to know at exit
time whether a cleanly-exited process was `ls` being done or a process
whose forwarded startup continues elsewhere; the design makes the
distinction unnecessary instead of guessing.

The icon can be dragged off the plate. Dragging extracts the launch
(`Command::ExtractLaunch`): the Command's slot empties, its running state
ends without cancelling the child and without scheduling `next`, and the
launch becomes a board object under the pointer. This is how one Command
runs several instances at once — extract the running one, run again — while
the Command itself keeps strict one-instance semantics (RUN while running is
refused; a Timer firing into a busy Command is skipped, which is the safe
scheduling default). Deleting a launch that sits on a Board SIGTERMs a
child that has not exited and has produced no window; a child with a window is terminated
through the window's own close path instead, and deleting the launch of an
already-exited run deletes only the record.

Launches on a Board do not survive serialization: a saved Launch
deserializes as a dead record and is removed by the post-load pass, because the process it
represented is gone and the windows it produced revive through their own
recipes. `persistence.cpp` creates the object for the "Launch" type directly
instead of going through the prototype registry, which keeps Launch out of
the toolbar.

## Stream captures and pipes

Standard error is not routed anywhere by the stdio cables, and standard
output is only routed when a cable or file is connected. The default for
both is capture: `Launch::Spawn` gives the child a dedicated pipe per
captured stream, holds the read end, and drains it on the epoll thread into
a bounded ring buffer on the Launch (256 KiB per stream). This is
deliberately different from inter-command pipes, where Automat holds no
ends to preserve EOF and SIGPIPE semantics — that rule exists for pipes
between processes and does not apply when Automat is the consumer. The
Command's toy shows the tail of the last launch's captures on a paper strip
under the plate — stdout in ink, stderr in red — so a program's output and
errors are visible where it ran. Buffers live exactly as long as
their launch: kept after exit, replaced by the next run, session-bound
(they are not serialized).

Pipes between pipeline stages are recorded as `Pipe` objects
(`ReferenceCounted`, not board objects): the writing launch holds the record
and the record names the reading launch. The pipe's runtime meters — fill and capacity via
a transient pidfd_getfd of the child's own descriptor, the blocked side via
/proc — moved from Command onto `Launch::StdoutStats`, since they are
per-instance truths. The stream cable remains the pipe's visualization.

## The flows

- **RUN on a Command**: `Command::Run` collects the downstream chain,
  creates the inter-stage pipes and their Pipe records, and calls
  `SpawnStage` per stage, which goes through `Launch::Spawn` with
  `source = the command`.
- **Restore on load**: `LaunchRestoredWindows` (called at the end of
  `LoadState`) walks the boards once and creates a launch for every
  `ClientWindow` that has a recipe and no client — through the linked
  Command when one is connected and idle (`Command::RunFor`, so the Command
  owns the child and STOP works), else directly. There is no pending state
  and no per-frame pass: launches are created at load, at copy, and at RUN,
  and nowhere else.
- **Copy of a live window**: `Clone()` returns a new window of the same
  type and `LaunchClone` immediately spawns a launch aimed at it. The clone
  fills in where it is dropped. Copying a dead window yields a dead copy.
- **Window appears** (both servers, epoll thread): `Launch::Find` by token
  or pid, then fill `restoring` or create a new associated window. The
  Wayland-specific activation orderings — `activate` before the map stashes
  the launch on the surface, `activate` after the map may only claim a
  window still waiting in the handoff queue — are described in
  `Wayland Compositor.md`.

## Threading

Spawning happens on UI and VM threads; matching and window filling on the
epoll thread; toys read on the UI thread. The registry mutex is a leaf —
nothing else is acquired under it, and posix_spawnp runs outside it. Each
Launch has its own mutex following the standard object pattern, with
`wake_counter` notifying toys. Capture listeners live on the epoll thread
and remove themselves on EOF or when their launch dies. The established
lock order `vm.mutex` → window/command mutex → launch mutex → registry mutex
is never reversed.
