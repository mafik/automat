# Pipeline Language

This document is the design for representing pipeline libraries inside
Automat: GStreamer, PipeWire, FFmpeg (libav), GEGL, TensorFlow, and UNIX
pipes. The design has three primary use cases, in priority order: a user drops a single block on the
board and learns what it does by playing with it; a user browses the
available blocks and can tell at a glance what each one is for; a user
builds a working pipeline one step at a time. Secondary goals: blocks from
different libraries combine without hidden performance loss; running
pipelines show their data and their throughput; the three drive models
(a library running its own threads, a library driven call-by-call, and
Automat's explicit control flow) mix freely.

## Design rules

**Every object works from the moment it is dropped.** A block that does
nothing until a complete pipeline exists cannot be learned by interaction.
Each library has a cheap immediate mode, and every object uses it the
moment it lands on the board. The per-library mechanisms are listed under
"Dropping a single block" below.

**Data is shown as measurements, not as moving chunks.** Real rates make
per-buffer animation impossible: PipeWire audio moves about a thousand
buffers per second, and no API can track one buffer across a thread or
process boundary anyway. What the APIs do expose is aggregate: counts,
rates, fill levels, and progress. The visual language therefore shows
numbers, level bars, and content previews, and it animates only things
that are computed from counters.

**Streams flow downward.** Control flow in Automat already runs top-down
(DRAKON style), and a push pipeline is the same shape: each stage hands
its output to the stage below. Stream inputs are ports on a block's top
edge; stream outputs are ports on its bottom edge. Scalar parameters keep
their existing place on the left edge. A rule this simple can still be
broken where a better layout exists.

**Formats are printed in each library's own notation.** The text at a
connection is `video/x-raw(memory:DMABuf) format=NV12 1280x720`,
`R'G'B'A u8`, `f32[?,224,224,3]`, or `S16LE 2ch 48000Hz` — never a
paraphrase. The native notation is precise, it teaches the library, and
for GStreamer the caps features already state the memory type, which is
the whole zero-copy story in the library's own words.

**Nothing converts silently.** Every conversion or copy that a library
would insert implicitly becomes a visible adapter object on the board.
The user can only remove a cost they can see.

**There are no pipeline containers.** Blocks stand directly on the board
and mix freely; no box groups them. Where a library requires a container
object (GStreamer requires a GstPipeline), Automat maintains it invisibly,
one per connected component. Errors and state changes land on the
individual block that caused them, using the existing beta state grammar
(red outline plus exclamation chip for errors; state words like PLAYING
printed on the block).

**Existing vocabulary is reused.** Parameters are instruments, shared with
Leptonica (GStreamer GParamSpec, FFmpeg AVOption, PipeWire SPA Props, and
GEGL param specs all describe name, type, range, and default, so one
instrument set serves all of them). Prototypes present as clone piles.
Shelves group blocks. The radar points to nearby objects where a
connection may auto-connect, exactly as it does today. The run button
keeps its one meaning.

## Concepts

**Stream connection.** A connection that carries a sequence of buffers
between two blocks. It is drawn like the existing cables — a straight
line with circular 90-degree turns — but thicker, as two parallel walls
with a visible interior, so control cables and stream connections can
never be confused. While data flows, a dashed pattern inside the
connection moves at a speed proportional to the measured buffer rate,
and a small label near the connection prints the rate ("30 f/s ·
18 MB/s"). The pattern speed comes from a counter, not from tracking
buffers; the counters are listed under "Meters".

**Port.** The point on a block where a stream connection attaches.
GStreamer calls these pads, PipeWire and GEGL call them ports; the board
uses one word. Ports that exist only sometimes (a demuxer grows one pad
per stream in the file, reported by the pad-added signal) are drawn as
faint sockets until they appear. Request pads (GStreamer tee, muxers)
are drawn as a socket with a plus sign; connecting to it requests a new
pad (gst_element_request_pad_simple).

**Preview.** Every block that carries data shows its current data on its
own face. There is nothing to attach and nothing to discover: if data is
present, the block displays it. A preview is a sampled measurement, not
the stream itself — it reads the data at a bounded rate and never blocks
the pipeline. Each domain has a standard preview:

- Video: the current frame, updated at a preview rate (a few frames per
  second when the block is small, full rate when the user enlarges it).
- Audio: a level meter, with an optional oscilloscope view.
- Text and bytes: the scrolling tail of the most recent data, like a
  terminal, plus byte counters. The face is user-resizable, the same as
  Automat's window objects, so "how large must it be" is the user's
  choice; the minimum useful size shows one line of text or one level
  bar.
- Images: the Leptonica paper, unchanged.
- Tensors: dtype, shape, device, and min/mean/max, with a small heat
  strip for two-dimensional slices.
- Compressed media: stream description (codec, resolution, bit rate) and
  position, since showing encoded bytes as pixels would be a lie.

**Meter.** A numeric or bar readout of a measured quantity. Every meter
maps to a specific API: GStreamer queue fill from the queue element's
current-level-buffers, current-level-bytes, and current-level-time
properties; buffer and byte rates from a pad probe that only increments
counters; PipeWire per-node busy time, wakeup delay, and xrun count from
the profiler module (the numbers pw-top shows); the driver's quantum and
rate from the graph clock; UNIX pipe fill and capacity from
ioctl(FIONREAD) and fcntl(F_GETPIPE_SZ), sampled through a transient
pidfd_getfd; the blocked side of a pipe from /proc/pid/syscall and
/proc/pid/wchan; per-Command byte totals from /proc/pid/io (rchar and
wchar), shown on the Command itself because they are process
properties; FFmpeg progress from packet pts against the stream duration;
GEGL progress from GeglProcessor's progress value; tf.data throughput
from element counts.

**Tap.** An action on any stream connection or port that copies one
buffer out and turns it into an ordinary board object: a video frame
becomes an image on a paper, audio becomes a clip, a tensor becomes a
tensor object, text becomes a text object. The copy uses the library's
sanctioned mechanism: an appsink pulling one sample in GStreamer, a
short pw_stream capture in PipeWire, the frame already in hand in libav,
gegl_node_blit in GEGL, Dataset.take(1) in tf.data, tee(2) on a byte
pipe. Taps connect streaming to the rest of Automat: a tapped frame can
feed Threshold like any other image.

**Adapter.** A block that converts between formats or memory types:
videoconvert, audioresample, hwdownload, a babl conversion, a
tf.cast. When two ports cannot agree on a format, Automat proposes the
adapters that would make the link work, ranked the way the library ranks
them (GstElementFactory rank; the converters libavfilter would
auto-insert). Accepting the proposal places the adapter as a real block.
Adapters that copy across memory domains print their throughput, because
those copies are the main avoidable cost when mixing libraries.

## Dropping a single block

The first use case: the user takes one block from a shelf, drops it, and
learns it by playing with it. What "it works immediately" means depends
on the library; each library has a real mechanism for it.

- **A GStreamer source** (videotestsrc, a camera, a microphone) starts
  producing when dropped: Automat puts it in a hidden pipeline, sets it
  PLAYING, and connects its unlinked source pad to an internal preview
  sink (a real appsink; video negotiates to a small RGBA frame, audio to
  a peak meter). The face shows the live picture or level immediately.
  Turning an instrument (pattern, brightness, freq) changes the output
  on the spot, subject to the property's mutability flag
  (GST_PARAM_MUTABLE_PLAYING allows live changes; others require a
  brief state drop, which the block shows as its state word changing).
- **A GStreamer filter or decoder** alone has nothing to process. Its
  face shows what it is (klass line, description from the factory
  metadata) and its ports show what it accepts and produces (pad
  template caps, printed). It becomes live the moment it is dropped near
  data: while the user drags it, the radar points at nearby blocks whose
  ports can link (the pad template caps intersect, checked with
  gst_element_factory_get_static_pad_templates and
  gst_caps_can_intersect - see GStreamerElement::CanFeedStream), and
  dropping it in radius auto-connects it, exactly like dropping a
  Leptonica tool near the paper.
- **A GEGL operation** alone renders its own demonstration: the operation
  applied to a standard generated input (gegl:checkerboard), rendered as
  the block's face. Turning an instrument re-renders it live. GEGL's
  reference compositions (gegl_operation_get_key(op,
  "reference-composition")) reference data files from the GEGL source
  tree that installed systems do not have, so the generated input is the
  demonstration. Near a paper, the radar offers the connection, and the
  preview switches from the demonstration to the real image.
- **An FFmpeg media file object** opens the file the moment its path is
  set (avformat_open_input, avformat_find_stream_info) and shows the
  container, duration, and one row per stream with codec, resolution or
  sample rate, and bit rate — each row is an output port ("#0", "#1")
  producing that stream's packets, and the "video" port doubles the best
  video stream (av_find_best_stream) so a decoder connects without
  knowing indices. Reading is shared: a decoder pulling its stream parks
  packets that belong to other connected streams in their bounded
  queues, and each queue's fill is its port's meter, so one consumer
  outpacing another is visible on the cable. No pipeline is needed for
  any of this. An FFmpeg codec object decodes whichever stream feeds it
  — a video stream becomes the held image, an audio stream shows the
  decoded sample format — and lights up when dragged near a matching
  packet stream.
- **A PipeWire node object is already live**, because it is a proxy for
  a node that exists in the daemon's graph. Its face shows node name,
  media.class, state (suspended, idle, running), the negotiated format,
  and a level meter. The meter's capture stream exists only while the
  proxy stands on a board: a capture stream is itself a node in the
  graph, so a shelf that attached one per entry would list its own
  streams and grow without end. Volume and mute are instruments backed
  by SPA Props. Automat runs as root and talks to the daemon directly;
  no portals are involved.
- **A TensorFlow operation or layer** computes eagerly. Pointed at a
  tensor object, it produces its output tensor immediately and keeps it
  current when the input changes. Alone, it shows its signature
  (input/output dtypes and shapes, parameter counts).
- **A UNIX command** is the existing Command object with three new
  ports: stdin on top, stdout at the bottom, stderr as a smaller
  separate output. While stdin is unconnected, the face offers a text
  input line; while stdout is unconnected, the face shows the scrolling
  output tail. So dropping `grep --line-buffered error` alone gives a
  working playground: type a line, see whether it passes. This is the
  discovery story for a library that has no machine-readable
  introspection at all.

## Browsing the shelves

The second use case: see what exists and what each block is for. Each
library is a shelf, and the shelf uses the library's own taxonomy,
because that grouping is the library authors' curation: GStreamer klass
strings ("Source/Video", "Codec/Decoder/Video", "Filter/Effect/Audio"),
GEGL categories, PipeWire media.class values, FFmpeg's
codec/filter/muxer registries, Keras layer families.

The GStreamer shelf exists (src/library_gstreamer.cpp, GStreamerShelfToy):
every factory compiled into the static build registers as a prototype, and
the shelf groups them by the first segment of their klass strings, ordered
Source, Filter, Sink, Generic — the direction data flows on the board. The
PipeWire shelf exists too (src/library_pipewire.cpp, PipeWireShelfToy),
with its groups ordered the same producer-to-consumer way, and so does the
GEGL shelf (src/library_gegl.cpp, GeglShelfToy), grouped by the categories
key with render sources first and programming plumbing last. All are
reached through the beta stamp's bubble menu, like the Leptonica shelf,
and all shelves place their entries with the shared ui::ShelfButton.

A shelf entry answers "what is this" before it is touched:

- The name is the library's real name for the thing: `videoconvert`,
  `gegl:gaussian-blur`, `avdec_h264`. A muted credit line names the
  library and category. This is the existing rule of exposing the
  primitives Automat is built on.
- The shelf entry is the block's own face at reduced scale; an idle face
  shows the one-line description from the library's metadata (factory
  description, AVFilter.description, operation "description" key).
- The PipeWire shelf is different in kind: it lists the live nodes of
  the running system, grouped by media.class, with their current states —
  because PipeWire objects are not prototypes to instantiate but
  existing things to use. Each entry is the node proxy's own face, so it
  carries the state word and format the node has right now; dragging one
  out places its proxy on the board. The shelf follows the daemon: nodes
  appearing and disappearing rebuild it. Several nodes sharing one
  node.name are shown as one entry, because a proxy addresses its node
  by name.

Shelf entries present as clone piles, unchanged.

## Building a pipeline step by step

The third use case: composition, one block at a time, with the pipeline
working at every intermediate step.

Connecting is the existing gesture: drag a block near another, the radar
points at the ports that can accept it, dropping in radius connects,
or the user drags a connection between ports explicitly. Compatibility
is computed with the library's own negotiation primitives (caps
intersection in GStreamer, EnumFormat pod intersection in PipeWire,
query_formats in libavfilter, dtype and shape rules in TensorFlow; babl
converts any image format, and byte pipes accept anything).

When a link is made, negotiation runs and the fixed format appears as
the format label at the connection. When a link cannot work, the label
position shows the two irreconcilable formats and the proposed adapters;
nothing fails silently and nothing is inserted invisibly.

Editing a running pipeline follows each library's real rules:

- GStreamer: inserting or removing a block in a flowing stream uses a
  blocking pad probe upstream of the edit point (the sanctioned
  dynamic-relink procedure), then releases it. Elements added to a
  running pipeline are state-synced (gst_element_sync_state_with_parent).
  Demuxer pads appear when the stream is identified (pad-added), and
  the faint sockets become real ones.
- PipeWire: links are created and destroyed freely at any time
  (pw_link objects); this is the daemon's normal operation.
- FFmpeg: a filter graph negotiates once (avfilter_graph_config), so a
  structural edit rebuilds the graph. The affected blocks show a brief
  "reconfiguring" state — the cost is real, so it is shown, not hidden.
  Properties that libav marks runtime-settable (AV_OPT_FLAG_RUNTIME_PARAM,
  filters with process_command) stay live instruments.
- GEGL: connections and property changes are legal at any time; the
  graph is lazy. Changing anything invalidates downstream regions
  ("invalidated" signal) and the previews repaint as regions recompute
  ("computed" signal) — the repaint sweeping across the paper is real
  progress, not decoration.
- TensorFlow: eager objects recompute on change. A traced function
  (tf.function) shows a "tracing" state on first run and after a shape
  change, because retracing is a real cost.

The hidden GStreamer container is managed under this activity: linking
two blocks that were in different hidden pipelines merges them into one
(gst_bin_add, state resync); cutting a pipeline in two splits them.
Automat inserts nothing else without showing it; where GStreamer
requires a queue for an extra thread (each branch of a tee needs one),
the queue is proposed like any adapter and placed as a visible block.

## Watching a running pipeline

Previews on every block, meters on connections and queue blocks, and
taps for pulling single buffers out cover the fifth goal. The cost model
is stated plainly: previews sample at a bounded rate through the
library's cheap path (pad probe and appsink in GStreamer, monitor-port
capture in PipeWire, the frame already in hand in libav, the node cache
in GEGL); meters read counters that the libraries maintain anyway; a
tap costs one buffer copy when the user asks for it. Because Automat is
itself a Wayland compositor and an X11 server, a video sink is not a
special case: waylandsink or ximagesink content arrives through the
existing surface path (dmabuf where negotiated) and the sink block's
face is the actual video output.

Queue blocks (GStreamer queue, appsrc/appsink boundaries, tf.data
prefetch, the kernel buffer of a UNIX pipe) all show the same three
facts: fill level against capacity, input rate, output rate. A full
queue with a stalled consumer and a starved queue with a slow producer
are the two failure shapes of every streaming system, and they are
readable at a glance on any queue block regardless of library. A kernel
pipe even names the blocked side directly: /proc/pid/syscall shows
which fd a process is blocked on.

## Mixing libraries

A connection between blocks of different libraries is a bridge. Automat
picks the cheapest real mechanism and shows which one it picked:

1. Same process, same memory: reference passing (GStreamer appsink
   handing buffers to a GEGL buffer, DLPack between tensor libraries).
   An ordinary connection.
2. Different process or device, exportable memory: file-descriptor
   passing (dmabuf, memfd; DRM_PRIME frames mapping to VASurface via
   av_hwframe_map; splice between byte pipes). Also an ordinary
   connection; the format label's memory annotation
   ((memory:DMABuf), AV_PIX_FMT_DRM_PRIME) states it.
3. Otherwise: a conversion adapter with a visible throughput meter
   (hwdownload, videoconvert, av_hwframe_transfer_data, host-device
   tensor copies).

The user's optimization loop is: see the adapter, read the two format
labels that made it necessary, and re-plumb (pick the decoder that
outputs dmabuf, keep the filter on the GPU) until the adapter
disappears. Format agreement is checked when the link is attempted, so
an impossible bridge is visible before any data flows.

## Mixing drive models

Who moves the data is a fact about each library, and the board shows it
instead of hiding it:

- **Self-running**: GStreamer pipelines run their own streaming threads;
  PipeWire graphs are driven by their driver node's timer. Such blocks
  carry a state word (PLAYING, running) and their sources have the run
  button: run starts the flow, stop ends it. The PipeWire driver block
  prints its quantum and rate ("1024/48000"); all nodes it drives show
  their busy time against that quantum, which is exactly what pw-top
  reports and is the honest way to see who is close to an xrun.
- **Automat-driven**: FFmpeg codecs and filter graphs, GEGL processors,
  TensorFlow steps, and tf.data iterators do nothing until called. These
  blocks are driven by Automat's existing control flow: Run performs one
  call (one send/receive round, one gegl_processor_work chunk, one
  get_next), and Next chains, Timers, and loops schedule it. A media
  player built from FFmpeg blocks is a Timer running a decode step at
  the frame rate — ordinary Automat, no new mechanism: a Timer in a
  loop, with its Running property synced to the driven block's Step
  property, fires the step repeatedly at a fixed period. The FFmpeg codec
  block prints its last return value literally (OK, EAGAIN, EOF),
  because EAGAIN is the library's own word for "call the other
  function first".
- **Boundaries**: where a self-running stream meets an Automat-driven
  block, the libraries themselves require a queue with an API on both
  sides — appsrc/appsink in GStreamer, pw_stream in PipeWire,
  buffersrc/buffersink at a libav graph edge. These are the boundary
  blocks; each is a queue block showing fill, capacity, and both rates,
  plus the overflow policy as an instrument (appsink max-buffers and
  drop; appsrc block; leaky modes). Backpressure and frame dropping
  happen at the boundary block and are readable on it.
- **Stepping**: a paused GStreamer video sink accepts single-frame steps
  (gst_event_new_step), exposed as a step signal on the sink while
  paused. Automat-driven blocks step by definition. Live PipeWire
  devices cannot be stepped, so no step control appears on them.
  Automat's control flow pokes blocks through Signal interfaces
  (src/base.hpp). Each signal declares what happens when it arrives
  while the block's LongRunning is active: the starting signal
  (Runnable) is inhibited, because a running thing is not started
  twice; a step signal is delivered, because it performs one bounded
  unit of work regardless of the stream running. This is what lets a
  Timer or a Next chain drive a boundary block's Pull or Push during
  playback. A step on a stopped boundary block starts its chain first,
  so stepping works from the first press.

## Library notes

**GStreamer.** Elements are blocks; the hidden per-component GstPipeline
carries clocking and the bus. Any factory in the registry wraps
generically: the face's instruments come from GParamSpec introspection
(a GEnum is a chip that cycles its nicks, a boolean is a checkbox, a
bounded number is a slider; unbounded and exotic properties stay
recipe-only), and the block's ports come from the factory's pad
templates. A request template mints its pad when the chain builds; a
sometimes template's port waits as a faint socket until pad-added
delivers the pad, at which point the deferred link completes on the
streaming thread. Chains are graphs, not lines: starting any element
starts the whole linked component, and every member shows the run's
state. Every unconsumed always-source pad ends in an internal preview
branch (queue, videoconvert, videoscale, appsink) when it can carry raw
video, or a queue and fakesink otherwise; the leading queue gives each
branch its own streaming thread, so a tee's fan-out prerolls without
one branch starving the others. The branch takes part in caps
negotiation like any sink, so a chain with no real sink negotiates
toward the preview's format. A sometimes pad that appears with no
consumer waiting gets the same terminal branch, so a demuxer's
unclaimed streams keep flowing and show themselves. Editing the
topology of a running chain stops it and starts the new arrangement —
the state word shows the transition. Bus messages route to blocks: ERROR marks the
failing element with the existing error state and keeps the message
text on it; EOS appears on the sink block and is an event output usable
by control flow ("when playback ends, run this"); STATE_CHANGED updates
the printed state word. The appsink boundary block queues frames without
dropping, so a full queue backpressures the chain — the honest boundary
behavior — and each Pull takes one frame out as an image the rest of
Automat can consume; the appsrc boundary block's Push reads its
connected image and sends it in as one buffer. Sources with a duration show a position bar
(gst_element_query_position); dragging it performs a flushing seek.
Sinks that sync to the clock show the pipeline latency
(GST_QUERY_LATENCY).

**PipeWire.** The board mirrors the daemon's graph through the registry:
nodes, ports, and links, with each node's state, its SPA Props (volume
and mute), and its negotiated format following the daemon's update
events. The volume instrument shows and moves the cubic-scaled value
that wpctl prints; the daemon stores channel volumes linear. A stream
connection between two node proxies stands for the whole set of daemon
links between that pair of nodes; making the connection creates the
links (output ports paired to input ports by audio channel, with
object.linger so they outlive Automat) and breaking it destroys them. A
connection is board intent until the daemon carries it: while one of the
named nodes is missing from the graph, the connection keeps trying, so a
saved board recreates its links when its nodes return. Once realized,
the connection follows reality in both directions — when another tool
destroys the links, the board connection disconnects, and links created
behind the scenes (WirePlumber policy) appear as connections wherever
both ends have proxies on the same board. Deleting a node proxy only
removes it from the board, because the node belongs to the daemon.

**FFmpeg.** The media file, codec, filter, and muxer blocks are
Automat-driven. Packets and frames are refcounted (av_packet_ref,
av_frame_ref), so passing them between blocks copies nothing. Hardware
decode keeps frames on the GPU (hw_frames_ctx); the format label then
shows the hardware pixel format, and pulling such a frame to a preview
goes through av_hwframe_transfer_data at preview rate only. Timestamps
stay in each stream's own time_base and are rescaled exactly at block
boundaries (av_rescale_q); the format label includes the time base so a
mismatch is visible rather than mysterious.

**GEGL.** Every registered operation wraps generically
(src/library_gegl.cpp): the face's instruments come from GParamSpec
introspection using the same grammar as the GStreamer wrapper (a GEnum is
a chip that cycles its nicks, a boolean is a checkbox, a bounded number
is a slider over the spec's ui range), and the GEGL shelf groups the
operations by the library's own categories key, the way the GStreamer
shelf groups by klass. Blits and exports run through GeglProcessor so
progress is a number, and the "computed"/"invalidated" signals drive the
previews' progressive repaint. The paper is the data object at the
chain's edges.

**TensorFlow.** Tensors are data objects with dtype, shape, and device
printed; operations and layers compute eagerly against them. tf.data
pipelines preview their first element (Dataset.take(1)) so every stage
shows real data immediately; prefetch stages are queue blocks. Device
placement is part of the tensor's printed facts, and a host-device copy
is an adapter, the same as any other memory-domain crossing.

**UNIX.** The Command object gains stdio ports. A Command has two
states: offline (no process) and online (running). The ports describe
the offline state — they are recipe data, like the argv tiles. Start
resolves each port's binding to a concrete file descriptor, dup2s it
onto the child's fd 0/1/2, and execs. Because an anonymous pipe needs
both ends at creation, starting a Command starts the stages downstream
of it — starting the first stage starts the pipeline, the same way a
shell starts every stage of `a | b | c` before any of them runs.
Stopping needs no such rule: killing one stage lets the kernel
propagate the consequences — readers see EOF, writers get SIGPIPE —
and the exit chips show it, because SIGPIPE is the normal way UNIX
pipelines end early.

Each stdio port has a binding, and the binding is printed on the port:

- Terminal is the default while the port is unconnected. Automat opens
  a pty and the child gets the slave end, so the face is a real
  terminal: isatty() returns true, libc line-buffers, grep colorizes,
  the line discipline handles echo and ^C, and resizing the face sets
  the window size (TIOCSWINSZ plus SIGWINCH). Typing into the face
  writes to the pty master.
- Pipe is a kernel pipe to the connected peer. Automat is never in the
  data path.
- File is the connected file object. The file is opened fresh at start,
  matching shell `< file` semantics; sharing the file object's own
  descriptor is an explicit option, because a dup shares the file
  offset and a rerun would resume where the last run stopped.
- /dev/null is silence, and the clean choice for both directions,
  because reads from it return EOF and writes to it succeed.
- Closed is the sharp variant, offered explicitly and never as the
  default silence: the child gets EBADF and its next open() lands on a
  low descriptor, which is sometimes exactly the failure one wants to
  reproduce.

Because bindings are resolved at start, changing the binding of an
online Command takes effect by restarting it, which is what a shell
requires too. When stdout is connected into a pipeline, stderr keeps
its terminal binding, so every Command in a pipeline keeps its face
terminal, now showing errors and accepting typed input — the same
thing a shell window shows during `cmd | less`. Ports do not stop at
fd 2: a request port adds fd 3, 4, and so on, bound to a pipe or a
file, which covers `gpg --status-fd 3`, `ffmpeg -progress pipe:3`, and
LISTEN_FDS-style socket activation. The spec plate prints the
equivalent shell line, because port bindings are exactly shell
redirections.

A pipe between two running Commands belongs entirely to the kernel;
Automat only inspects it, and everything on the pipe's face comes from
these sources:

- Fill and capacity come from ioctl(FIONREAD) and fcntl(F_GETPIPE_SZ).
  Automat samples them through a transient pidfd_getfd — dup, query,
  close — because holding a persistent dup would break the pipeline's
  own semantics: a held read end suppresses the writer's SIGPIPE and a
  held write end suppresses the reader's EOF.
- Capacity is also a control: F_SETPIPE_SZ works on the same transient
  descriptor, so the capacity instrument on a pipe is writable while
  the pipeline runs, and a root Automat may exceed the 1 MiB
  pipe-max-size limit.
- The blocked side is read from /proc/pid/syscall, which names the
  blocked syscall together with its fd argument, and /proc/pid/wchan,
  which says pipe_read or pipe_write. The display therefore
  distinguishes a starved consumer (blocked in read) from a
  backpressured producer (blocked in write).
- Byte totals and rates are process properties, not pipe properties:
  /proc/pid/io rchar and wchar count the whole process's traffic across
  all descriptors, and the kernel keeps no per-descriptor counters. They
  are therefore shown on the Command object itself, beside its pid and
  exit readouts — never on the pipe, where the number would misattribute
  the process's other writes to this connection. The pipe's own display
  carries only what the kernel attributes to the pipe: fill, capacity,
  and the blocked side (the blocked syscall's fd argument ties it to
  this pipe specifically). Exact per-pipe rates would need an eBPF probe
  on pipe_read/pipe_write and are left as a possible later instrument.
- Topology comes from /proc/*/fd, where every pipe is named
  pipe:[inode]; matching inode numbers across processes reconstructs
  pipelines, including ones Automat did not start, so the board can
  show the machine's existing pipelines the same way it mirrors the
  PipeWire graph.

There is no format negotiation because byte streams have no formats;
the format label says bytes. Content is never visible on a direct
pipe — the kernel offers no way to peek without consuming — so content
appears only at endpoints Automat owns (the face terminal, a funnel, a
tap) or through an explicitly inserted tee Command.

## Known costs and open problems

- Preview sampling is bounded but not free; the preview rate must adapt
  to block size and zoom, and previews of hardware-memory video imply a
  download at preview rate unless the sink path is used.
- The pipeline libraries are vendored into third_party and linked
  statically (GStreamer with its plugins compiled in, FFmpeg, GEGL with
  its operations compiled in, TensorFlow with its op kernels compiled in,
  plus their static GLib stack), like every other Automat dependency;
  only Vulkan and PipeWire come from the host, because they are the
  official interfaces to host drivers and services.
- A library's compute runtime must not collide with Automat's statically
  linked LLVM inside software Vulkan drivers. Two rules keep that true:
  runtimes are excluded at build time (GEGL is built with OpenCL disabled),
  and the binary must not export its statically linked symbols into the
  dynamic symbol table, where a dlopen'd driver would bind against them —
  gmodule's -Wl,--export-dynamic is stripped from every link
  (src/glib.py StaticLibs). TensorFlow bundles its own LLVM, so when its
  archive is assembled, every symbol outside its public C++ API namespaces
  is localized (src/tensorflow.py); the bundled LLVM can neither clash
  with Automat's own LLVM nor leak into the dynamic symbol table.
- Two interfaces of one object must not share a display name: connections
  are saved as "object.interface" and load by name lookup, so a block
  with an "Image" input names its image output "Result".
- Merging or splitting the hidden GStreamer pipelines pauses and
  rewires them; the pause is shown as the state word changing.
- The FFmpeg filter-graph rebuild on structural edits interrupts that
  graph; runtime commands cover only some properties.
- Byte-stream previews of binary data need a hex view; the text tail is
  only right for text.
- Shelf faces should become demonstrations where the library can compute
  one: GEGL operations showing their reference composition, GStreamer and
  FFmpeg video filters showing a standard sample frame pushed once through
  the element offline, audio effects showing the processed waveform of a
  standard click. A face computed by the block itself cannot lie about
  what the block does. Today a shelf entry shows the block's idle face.
- A GStreamer block exposes at most four extra port slots and its face
  shows at most four property instruments; the further pads and
  properties exist only through saved recipes. A media file exposes at
  most six stream ports.
- A PipeWire node proxy's out connection names one peer node. When the
  daemon fans a node's links out to several nodes, the board shows the
  link to the first peer that has a proxy; the others exist without a
  connection on the board.
- The media file's position is a readout only; the live seek bar
  (av_seek_frame with a codec flush) is not built.
- Of the stdio bindings, Pipe and File exist (the File object resolves
  through the FdProvider interface); the pty terminal binding for
  unconnected stdio, /dev/null, Closed, the share-offset file option,
  stderr, and request ports beyond fd 2 are not built.
- The hidden GEGL graph is one global root rather than one per connected
  component, because a GEGL parent node carries no clock or bus that
  would need per-component scoping. Previews longer than 1024 pixels on
  an edge are sampled at reduced scale through the blit's scale factor.
  The negotiated babl format is not printed at GEGL edges: the node
  cache's format is not reachable through public GEGL API.
- The GEGL bundle folds in the common, core, transform and generated
  operation directories, and babl's extensions are compiled in because
  they register the format families operations look up by name
  (src/gegl.py, src/babl.py). Not compiled in: common-gpl3+ (GPL3
  operations in an MIT binary), common-cxx (each C++ operation carries
  its own module entry points, which collide in one library), and
  external (system library dependencies). gegl:bevel, gegl:layer and
  gegl:styles are excluded from the shelf and the prototype registry
  because their internal children live in those excluded sets; two
  startup log lines remain from a stray gegl:text reference and a
  buffer-property demonstration reading a null buffer.
