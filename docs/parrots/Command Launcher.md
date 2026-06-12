# Command Launcher

## The problem this object solves

Automat needs a way to start operating-system programs: directly useful on
its own, and the source of clients for Automat's Wayland compositor. The
dangerous part of every launcher is the shell between the user and the
kernel — quoting rules, expansion surprises, the gap between what was typed
and what the program receives. The Command object removes the shell entirely
and makes the one structure that matters — the argv array — directly visible.

Implementation: `src/library_command.{hh,cc}`. The object follows Automat's
device-noun naming (Timer, Key Presser): the object *is* a command; its RUN
button supplies the verb.

## The model is the argv vector

The object stores `Vec<Str> argv` — not a command-line string. One element is
one argument, by construction, and that is also the serialized form
(`"argv": ["notify-send", "Hello world"]`). The flat string exists only as a
derived view: `GetText` joins with single spaces, `SetText` splits on spaces
and is lossy by design — flat text has no way to say "this space is part of
an argument", which is exactly the ambiguity the object exists to remove.

An element may legitimately contain spaces. Where such elements come from is
governed by provenance: the space *key* always commits a tile — that is the
teaching gesture, and no quoting syntax will ever weaken it — while *atomic*
insertions keep their spaces. Today the atomic sources are deserialized
states and values pushed through links; clipboard paste and file drops join
them when the text-field layer gains a paste event. Inside a tile, a literal
space renders as a gray midline dot: the content stays ink, the invisible
byte gets a visible, structural mark that says "NOT a tile boundary".

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
(`ArgvField::KeyDown` in `src/library_command.cc`). When the row outgrows
the plate it scales down uniformly rather than hiding anything.

Layout follows the DRAKON grammar used across Automat: title and credit on
top, the data row under them, readouts in the lower-left corner. The run
control is the shared `ui::slop::RunButton` — a hand-drawn disc with a play
triangle (a stop square on red while the child runs) that seats itself at
the plate's lower center, dipping slightly past the border, where the
"next" connector leaves the object. The disc splits the bottom row: status
readouts live on its left, the right side stays free. Micro gray captions —
"program", "arguments…" — sit under the tiles; nouns naming parts, the
entire glossary a novice needs.

## States

Every state change shows on two channels (the Slop rule):

- The program tile wears a red outline and squiggle while its text does not
  resolve to an executable (`$PATH` search, same rules as `execvp`), and RUN
  is gray and hatched. When it resolves, RUN turns green.
- While the child runs, the button becomes a red STOP, the lower-left corner
  shows the spinner and the live `pid` readout.
- After exit, a chip reports the child's truth: green `exit 0`, red
  `exit N`, or the signal name if the child died by signal. STOP sends
  SIGTERM; a child that handles it and exits cleanly therefore shows its own
  exit code, not the signal.

Pressing Enter inside the tiles is equivalent to pressing RUN.

## Interfaces and composition

The object exposes the standard `Runnable` (spawn) and `LongRunning`
(STOP/cancel = SIGTERM, `Done` on reap) interfaces plus `Next` chaining, so
Timers, Gears, chains and the bubble menu drive it without special cases. A
detached reaper thread waits on the child and reports the exit status.

Children are launched with `WAYLAND_DISPLAY` pointing at Automat's own
compositor and without `DISPLAY` (`library::SpawnArgv`, errors through
`Status`). A window mapped by the child appears seated next to the Command's
plate and keeps a `Launcher` connection back to it — the visible cable that
also makes the relationship survive saves (see `Wayland Compositor.md` and
`Wayland Client Persistence.md`).

Deliberately absent: environment editing, working-directory control and
stdio plumbing. The plate's left edge is kept free for those as future
connection points, per the data-on-the-left grammar.
