# Command Launcher

## The problem this object solves

Automat needs a way to start operating-system programs: directly useful on
its own, and the source of clients for Automat's Wayland compositor. The
error-prone part of every launcher is the shell between the user and the
kernel: quoting rules, expansions, and the resulting gap between what was
typed and what the program receives. The Command object removes the shell
entirely and makes the one structure that matters — the argv array —
directly visible.

Implementation: `src/library_command.*`. The object follows Automat's
device-noun naming (Timer, Key Presser): the object is named after the
command it holds, and the RUN button performs it.

## The model is the argv vector

The object stores `Vec<Str> argv` — not a command-line string. One element is
one argument, by construction, and that is also the serialized form
(`"argv": ["notify-send", "Hello world"]`). The flat string exists only as a
derived view: `GetText` joins with single spaces, and `SetText` splits on
spaces and is lossy by design, because flat text cannot express that a space
is part of an argument — exactly the ambiguity the object exists to remove.

An element may legitimately contain spaces. Where such elements come from is
governed by provenance: the space key always commits a tile, and no quoting
syntax weakens that rule, while atomic insertions keep their spaces. The
atomic sources are deserialized states and values pushed through links;
clipboard paste and file drops join them when the text-field layer gains a
paste event. Inside a tile, a literal space is drawn as a gray midline dot:
the content stays ink, and the invisible byte receives a visible mark that
distinguishes it from a tile boundary.

## Tiles are argv

The panel's central element is a row of tiles, one per element. The first
tile is the program, the following tiles its arguments. Typing a space
commits the current tile and begins the next one; Backspace at a tile's
start joins it with the previous one (deleting the gap — literal spaces are
never created by edits). `*`, `$HOME` and quotes stay literal because
nothing interprets them. The gray `posix_spawnp()` credit near the title
states the contract the way Leptonica objects credit their library function.

The editor reuses `ui::TextFieldBase` for caret plumbing, clicks and
selection, with caret positions expressed as flat byte offsets into the
canonical single-separator join of the vector — a coordinate that stays
well-defined and bijective with (tile, offset) pairs even when elements
contain spaces. Key handling operates directly on the vector
(`ArgvField::KeyDown` in `src/library_command.cpp`). When the row outgrows
the plate it scales down uniformly rather than hiding anything.

Layout follows the DRAKON grammar used across Automat: title and credit on
top, the data row under them, readouts in the lower-left corner. The run
control is the shared `ui::beta::RunButton` — a hand-drawn disc with a play
triangle (a stop square on red while the child runs), placed at the plate's
lower center and dipping slightly past the border, where the "next"
connector leaves the object. The disc splits the bottom row: status readouts
are placed on its left, and the right side stays free. Small gray captions —
"program", "arguments…" — under the tiles name the parts.

## States

Every state change is shown on two channels (the Beta rule):

- The program tile is marked with a red outline and squiggle while its text
  does not resolve to an executable (`$PATH` search, same rules as
  `execvp`), and RUN is gray and hatched. When it resolves, RUN turns green.
- While the child runs, the button becomes a red STOP, the lower-left corner
  shows the spinner and the live `pid` readout.
- After exit, a chip reports the result: green `exit 0`, red `exit N`, or
  the signal name if the child died by signal. STOP means SIGTERM.

Pressing Enter inside the tiles is equivalent to pressing RUN.

## Interfaces and composition

The object exposes the standard `Runnable` (spawn) and `LongRunning`
(STOP/cancel, `Done` on reap) interfaces plus `Next` chaining, so
Timers, Gears, chains and the bubble menu drive it without special cases.
The child is watched through a pidfd registered on the shared epoll thread
(`mux::WatchProcess`), which reaps it and reports the exit status.

Children are launched with `WAYLAND_DISPLAY` pointing at Automat's own
compositor and `DISPLAY` pointing at Automat's own X11 server; `GDK_BACKEND`
is removed so a toolkit picks its preferred backend (`library::SpawnArgv`,
errors through `Status`). A window mapped by the child is placed next to the
Command's plate, on the Command's board, and keeps a `Launcher` connection
back to it — the visible cable that also makes the relationship survive saves
(see `Wayland Compositor.md` and `Wayland Client Persistence.md`).

## stdio streams

The Command exposes two stream ports (`src/stream.hpp`): `stdout` leaves at
the plate's lower left and `stdin` accepts connections at the top edge.
Connecting one Command's stdout to another's stdin is recipe data, like the
argv tiles: nothing happens until start. Starting a Command starts the
stages downstream of it, the way a shell starts every stage of `a | b | c`
together, because an anonymous pipe needs both ends at spawn. The pipe is
created with pipe2 and installed through posix_spawn_file_actions; Automat
keeps no end of it, because a held read end would suppress the writer's
SIGPIPE and a held write end would suppress the reader's EOF. Stopping has
no counterpart rule: when one stage exits, the kernel propagates EOF and
SIGPIPE and the exit chips report it (a `SIGPIPE` chip is the normal way a
pipeline ends early).

A stream port may also connect to a File object (`src/library_file.*`)
instead of another Command: the head's stdin reads it and the tail's stdout
writes it, which is shell redirection made visible on the board. At start
the Command resolves the file object to a concrete descriptor through the
`FdProvider` interface (`src/fd_provider.hpp`) — a fresh open per run, so a
rerun rebuilds the file the way `>` does, and the file object's append flag
turns that into `>>`. A failed open aborts the whole launch, the way a shell
refuses the command when its redirection fails, and the error is reported on
the file object. The file object's face shows the file as it is on disk —
size and the tail of its content, polled while visible — so output arriving
in the file is watchable in real time. A file also bounds the start-together
rule: a chain start stops at a file, because unlike a pipe a file needs no
second process, and `a > f` followed by `b < f` are separate runs in a shell
too.

The stream connection renders as a pipe and prints its meters on a chip
beside its middle. The chip carries only what the kernel attributes to the
pipe itself: the format ("bytes" — byte streams have no formats to
negotiate), the pipe's fill against its capacity as a bar, and, once it
persists, the blocked side ("backpressured producer" or "starved
consumer"). Fill and capacity come from FIONREAD and F_GETPIPE_SZ through
a transient pidfd_getfd dup of the child's own write end, closed
immediately because a held end would distort the pipeline's EOF and
SIGPIPE semantics; the blocked side comes from /proc/pid/syscall of the
two children (write on fd 1, read on fd 0 — the fd argument ties the
blocked state to this pipe), smoothed with hysteresis because a producer
under flow is momentarily blocked on many samples.

Throughput is deliberately NOT on the pipe. The kernel keeps no
per-descriptor byte counters; /proc/pid/io wchar counts the whole
process's writes, so a rate derived from it is a process property and a
pipe label would misattribute the process's other traffic to this
connection. Write totals and rates therefore belong on the Command object
itself, beside its pid and exit readouts. Everything is sampled on every
UI tick, so the meters move at the same rate as the rest of the
interface.

Deliberately absent: environment editing, working-directory control,
stderr and extra-descriptor ports, terminal (pty) bindings for unconnected
stdio, and starting stages upstream of the started one. The stdio design
these grow into is recorded in `Pipeline Language.md`.
