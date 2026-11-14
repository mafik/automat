# Mem Meld

`MemMeld` (`src/mem_meld.hpp`, `src/mem_meld.cpp`) keeps a live copy of another
Linux process's memory inside Automat so that later code reads and writes the target with plain
loads and stores rather than a system call per access. It is a utility layer below Automat's object
layer; the Process object that will present a remote process to the user is built on top of it.

## Why a mirror instead of per-access system calls

The intended consumer uploads the whole copy to the GPU for visualization and follows pointers
through it. `process_vm_readv` per access is too slow for that, and a copy that lags is acceptable
only if reads and writes still reach the target's current bytes. The mirror therefore shares the
target's physical pages: the target's private memory lives in `MAP_SHARED` mappings of one memfd,
and Automat maps the same memfd, so both processes see the same pages. File mappings are shown by
mapping the same file.

## The local mirror

`Local()` returns the local mirror: one contiguous local range with no holes, because the GPU
upload wants a single buffer. Remote pages live at unrelated local addresses and addresses are never
shared between the processes, so a stored reference is a remote address plus an offset that is
translated again before each use. Translation in both directions is one lookup in one of two
translation tables, which are range maps (`MapleTree`, see `docs/parrots/Maple Tree.md`) with one
segment per region. The local mirror is a reservation of 256 GiB, made when the mirror is
constructed; a region that does not fit is not mirrored.

A region (`MemMeld::Region`) is one mirrored range of the target. It records the target
address of its first byte, its local bytes inside the local mirror, its protection in the target,
its path and its file offset. The path is the file of a file region and the name of anonymous
memory that has one, such as `[heap]` or `[stack]`; it is empty for other anonymous memory.
`ToLocal(remote, size)` returns the local bytes of `[remote, remote + size)` when the first and the
last byte of that range lie in one region, and an empty span otherwise. `ToRemote(local)` returns
the target address of a byte of the local mirror, or 0 when the byte lies outside every live region.
`Find(remote)` returns the live region that contains the address, or null.

`Local`, `ToLocal` and `ToRemote` may be called from any thread without a lock, so that objects on
other threads can read and write the target's memory directly. Only the tracer thread writes the
translation tables, and each update of a table becomes visible to readers at one instant. An
operation that needs several updates, such as the split of a region or a move, becomes visible step
by step, so a reader may find no translation for an address whose mapping did not change. The table
keyed by remote addresses holds a segment that covers the remote range of each region, and the table
keyed by local addresses holds a segment that covers its local range. The value of a segment is the
page distance from its range to the other range, modulo 2^35 pages, together with the index of the
region. A reader therefore translates with the value alone and never reads the region records,
which the tracer thread changes. `ToLocal` looks up the first byte and requires that the last byte
lies in the same segment. The extent behind `Local()` is an atomic, which grows only after the new
local range is mapped. `Find` reads the region records, which the tracer thread changes and reuses,
so it may be called only from the callbacks, and the region that it returns is valid until the
callback returns.

A translation may be outdated by the time it is used, because the target keeps running. Reading
through an outdated translation never faults: the local bytes of a region that left are mapped again
as read-only zero pages, and the bytes of a later region may replace them. Every byte of the local
mirror is backed and readable, except in the pages of a file region that lie wholly past the end of
its file, which raise `SIGBUS` as in the target. Writes fault on read-only pages, as in the target.
Because the zero pages replace released ranges, Automat also keeps no mapping of memfd offsets that
the target released.

Memfd regions are mapped read-write locally whatever their protection in the target, so Automat can
write to memory that the target only reads, as a debugger can. A private file mapping that is shown
from its file is mapped read-only. A shared file mapping is mapped read-write when its descriptor
allows writing. The bytes of the last partial page past the end of a file read as zeros. Reading a
memfd page that was never written allocates it, unlike anonymous memory, which maps the zero page;
readers of large sparse regions should skip holes with `SEEK_DATA`.

## Tracing at the system-call level

The memory map of a process changes only through system calls, so the mirror follows those: the
tracer thread seizes every thread of the target with ptrace and resumes them with `PTRACE_SYSCALL`,
and the kernel stops each thread at the entry and the exit of every system call. The tracer rewrites
the arguments of the few calls that change the address space and resumes all others untouched.
Nothing runs inside the target and no library function of the target is called, so programs that
bypass libc (Go, static binaries, raw `syscall`) are mirrored like any other, and no lock of the
target matters. Seccomp filtering, which could limit the stops to the interesting calls, is not
used: a filter can never be removed, and once no tracer is attached the filtered calls fail.

System calls are injected into a stopped thread by setting its registers to the arguments, pointing
its instruction pointer at a `syscall` instruction in `[vdso]` and resuming it to the entry and the
exit of that call, with every signal blocked through `PTRACE_SETSIGMASK`. Single-stepping is not
used, because the kernel resets the disposition of `SIGTRAP` when it forces the step trap on a task
that blocks or ignores it. A thread that stands at the entry of a system call first has that call
cancelled; it runs again after the registers are restored.

## The tracer thread

Every ptrace request must come from the thread that seized the tracee, so the thread that calls
`Attach` is the tracer thread: `Attach` seizes the target, handles the stops of every task of the
target and returns when the session ends. The mirror starts no thread; the consumer chooses the
thread that calls `Attach` and keeps it for the whole session. The tracer thread waits with
`__WNOTHREAD`, so that it sees only the stops of its own tracees and never those of another mirror's
tracer thread or the children of other Automat threads. It does see the children of its own thread,
and its wait collects their exits, so the tracer thread should not be the parent of other
processes. Mirrors share no state, so they need no lock. The tracer thread changes all state of its
mirror: the memfd, the offset allocator, the local mirror and the translation tables. At each
system-call stop it applies the change to the local mirror and the tables, calls the callbacks, and
only then resumes the target thread, so a callback sees the target's memory map as the call left
it. A consumer with expensive work forwards it to another thread itself.

`Attach` may be called once. It duplicates the pidfd of the target, so the caller keeps its own
descriptor. When `Attach` returns, its `Status` reports the outcome of the session: OK when `Detach`
ended it or the target exited with status 0, otherwise an error that says how the target exited or
why the attach or the session failed. The errors found before the session starts are reported
through the same `Status`: a second call, an invalid pidfd, a pidfd whose process id cannot be read
and a failed reservation of the local mirror. No callback reports the end of the session; a consumer
that reacts to it does so after `Attach` returns. While the session runs, the wake signal is
unblocked in the tracer thread, and the signal mask of the thread is restored when `Attach` returns.

`Detach` is the only function that another thread calls to control the session (`Local`,
`ToLocal` and `ToRemote` need no lock). It sets a flag of the mirror, which the tracer thread reads
before each wait, and interrupts the tracer's `waitid` with a signal whose handler does nothing. A
signal that arrives just before the wait is lost, so `Detach` sends it again every millisecond until
the session has ended. When `Detach` returns while `Attach` runs, no callback runs afterwards. A
callback runs on the tracer thread and may call `Detach`, which then only sets the flag, and the
session ends when the callback returns. The flag stays set when `Detach` is called before `Attach`,
so that `Attach` detaches again as soon as it has attached.

A mirror must not be destroyed while `Attach` runs, which a debug assertion reports. Because no
callback can run once `Attach` has returned, the state that the callbacks use needs no protection
against the destruction. A mirror has a single owner and is not reference-counted. The owner of the
thread that runs `Attach` calls `Detach`, joins the thread and then destroys the mirror. Lifetimes
shared with other threads belong to the layer above: other objects hold a `WeakPtr` to the Automat
object that owns the mirror, and they lock that object before they read or write the local mirror
directly.

## Callbacks

A consumer assigns the callbacks of `MemMeld` before it calls `Attach` and does not change
them afterwards, because the tracer thread reads them without a lock. A callback that is not
assigned is skipped. The tracer thread calls the callbacks while the target thread whose stop caused
them waits:

- `on_mapped` reports a region that appeared.
- `on_changed` reports one region that moved, grew, shrank or changed its protection; `before` and
  `after` describe that region before and after the change.
- `on_unmapped` reports a region that stopped being mirrored; its local bytes are still readable
  during the call.
- `on_attached` reports that every region of a new address space, at the attach or after an `exec`,
  has arrived through `on_mapped`, so the mirror is live.
- `on_forked` reports a fork child that the consumer may follow (see fork, clone, threads and exec).

A new address space is reported through `on_unmapped` for every region of the old one, then through
`on_mapped` for each new region in ascending address order, and then through `on_attached`. A region
that a call splits is reported through `on_unmapped` for the whole region and then through
`on_mapped` for each part in ascending address order. The order of the reports of an `mremap` that
moves memory is described in Memory after the attach.

## Attach

The tracer seizes every thread, repeating until no new thread appears, stops them, and confirms that
the pidfd still names the seized process. A target that another mirror or a debugger traces cannot
be seized, because a task has one tracer, so the attach fails. A target that runs under a seccomp
filter is refused, because the filter would also judge the injected and rewritten calls; Chromium's
filter answers them with `SIGSYS`. The tracer creates the memfd inside the target, places it at
descriptor 900 when the descriptor limit allows it, makes it 2^60 bytes long and obtains it with
`pidfd_getfd`.

Then it converts the memory present now, with every thread stopped: each private anonymous mapping,
whatever its protection, is copied into memfd pages that are mapped over it. Only populated pages
are copied, found through `/proc/<pid>/pagemap` and read through `/proc/<pid>/mem`, which reads
pages whatever their protection, so the bytes of an inaccessible page survive and a large
reservation allocates nothing. A private file mapping is converted the same way when it is writable
or holds private pages (relocated RELRO pages, for example); otherwise it is shown by mapping its
file. Shared file mappings are shown by mapping their file. The pause lasts about 0.15 s for 256 MiB
of resident memory and grows with it.

Some mappings are neither converted nor mirrored, because converting them would change how the
target behaves: wipe-on-fork and don't-fork mappings (glibc keeps its getrandom state in one), whose
fork semantics belong to private anonymous memory; device and special mappings (`[vdso]`, `[vvar]`,
`[vsyscall]`, hugetlb, `VM_IO`, `VM_PFNMAP`); grow-down mappings other than the main stack; and
memory registered with userfaultfd, which a new mapping would drop.

The main stack cannot stay a grow-down mapping, because a memfd mapping does not grow, so it becomes
one memfd mapping as large as the stack limit, with an inaccessible guard below it where there is
room. Its top page from the first argument string up stays private anonymous memory and is not
mirrored: `/proc/<pid>/cmdline` and `environ` read only anonymous memory, and tools such as `ps`
read them.

## Memory after the attach

Only accessible memory is mirrored: the local mirror maps a memfd range when it becomes accessible
and drops it when it becomes inaccessible. The tracer acts on these calls:

- `mmap` of private anonymous memory, whatever its protection, is rewritten at its entry into a
  `MAP_SHARED` mapping of fresh memfd offsets, so new memory never needs a later conversion;
  `MAP_DROPPABLE` mappings and mappings with `MAP_GROWSDOWN` or `MAP_HUGETLB` stay private. A
  private file mapping is converted at the exit when it is writable; a read-only one and a shared
  one are shown from the file that `pidfd_getfd` takes from the descriptor the call used.
- `mprotect` runs unchanged; a private file mapping that becomes writable is converted, and every
  other accessible file mapping in the range is shown from its file.
- `munmap`, a `MAP_FIXED` mapping over mirrored memory, an `MREMAP_FIXED` destination, a shrinking
  `mremap` and a shrinking `brk` release the memfd offsets they remove.
- `mremap` growth whose new offsets would reach pages that another mapping uses first moves the
  range to fresh offsets (the call is cancelled, the bytes are copied with `copy_file_range`, and
  the call runs again); when that fails it fails with `ENOMEM`, as without memory. Only a range of
  one VMA is moved: the kernel refuses to grow or move a range of several VMAs, so such a call runs
  unchanged and fails as it would without the tracer. A growth to more than one slot (see Memfd
  offsets) fails with `ENOMEM` instead of moving, because its fresh offsets would again cross the
  end of a slot. `MREMAP_DONTUNMAP` and the duplication of a mapping with an old size of zero fail
  with `EINVAL`, as for the private anonymous memory that the target asked for. The region keeps its
  identity through the move to fresh offsets: its local bytes are mapped onto the fresh offsets in
  place, so the move itself is not reported, and the growth is reported through `on_changed` as
  without it.
- An `mremap` that moves memory is reported through `on_changed`: each region keeps its local bytes,
  its path and its file offset at the new address, because Automat objects hold addresses relative
  to their region. A shrink of a move is reported before it and a growth after it, each through
  another `on_changed`, and what the local mirror showed at the destination goes first.
- `brk` runs unchanged; at its exit memfd pages that continue the heap's offsets are mapped over the
  new pages, so that the kernel merges them with the heap. A query with the argument 0 is not
  followed, because its result, which may be older than the result of a concurrent growth, would
  otherwise read as a shrink.
- `madvise` with `MADV_DONTNEED`, `MADV_DONTNEED_LOCKED` or `MADV_FREE` over anonymous memfd memory
  runs as `MADV_REMOVE`, with which the kernel itself frees the memfd pages, so that they read as
  zero in both processes, as anonymous memory does. `MADV_WIPEONFORK` over anonymous memfd memory
  first makes the range private anonymous memory with the same bytes, as a detach does, and the
  local mirror drops it, because wipe-on-fork memory is never mirrored; the call then runs on
  private memory and succeeds, as without the tracer.
- `madvise` with `MADV_GUARD_INSTALL` or `MADV_GUARD_REMOVE` runs unchanged, and its exit stop reads
  the range again (see Guard regions).

## Guard regions

A guard page is a page that `madvise(MADV_GUARD_INSTALL)` marks so that every access to it faults.
The marker has no mapping of its own: the memory map still shows the surrounding mapping as
accessible, and only `/proc/<pid>/pagemap` tells the guard pages apart, with bit 58 together with
bit 62. glibc places the guard page of a thread stack this way. A guard page is inaccessible in the
target, so it is not mirrored and it stays in the target as it is.

Every read of a range of the memory map therefore reads the guard pages of the range as well:
`PAGEMAP_SCAN` lists the pages that are swapped, which includes the guard markers, and the pagemap
entries of those pages say which of them are guard pages. The regions of a range are the accessible
pages of its mappings without the guard pages, so a mapping with a guard page inside it is mirrored
as several regions. The tracer follows `MADV_GUARD_INSTALL` and `MADV_GUARD_REMOVE` like any other
call that changes a range: at the exit of the call it reads the range again, so installed guard
pages leave the mirror and removed ones return. The memfd offsets under a guard page stay in use
until the target unmaps the mapping, because the guard page is still mapped in the target.

The attach does not convert guard pages. It converts the parts of a mapping on both sides of a guard
page and leaves the guard page as private anonymous memory with its marker, because a mapping over
it would drop the marker. A detach makes memfd mappings private anonymous memory again and installs
the guard markers again in the copy.

## Concurrent calls

Automat does not serialize anything that the target does not serialize: the threads of the target
run their address-space calls concurrently, and their exit stops reach the tracer in any order. The
bookkeeping is therefore independent of that order. The entry stop of a call records the pieces of
memory it may remove, each identified by its address and its memfd offset (or, for a file, its file
offset), and the exit stop makes the local mirror show what the target maps in the call's ranges at
that moment: regions that the target no longer maps there go, and what it maps now is added or keeps
its region. A removal applies only to a region that still shows the same memory, so a region that
another thread mapped at the same address survives. A file region shows the same memory only as long
as its file has the same device and inode, so a different file mapped at the same address and file
offset replaces it. Because every change of a range ends with an exit stop that reads the range
afterwards, the local mirror converges to the target's maps.

Memfd offsets are punched only when a finished call released them and no mapping of the target uses
them. A piece that a call recorded and that is gone at its exit is released directly when no
`mremap` ran meanwhile and no offsets of its slot were released since the entry; otherwise the
tracer first reads the whole memory map and releases only the offsets that no mapping of the target
uses, that no call took at its entry without having reached its exit, and that the local mirror does
not show. A call that rewrites an `mmap` takes its offsets at the entry, but the target maps them
only when the call runs; and the region of memory that another thread unmapped stays in the local
mirror until that `munmap` reaches its exit stop. Memory that the local mirror showed and lost is
released through the piece of its region, which the tracer takes at the exit, instead of the piece
that the entry recorded for the same memory; because no release frees offsets that the local mirror
shows, no other release can have overtaken that piece, and without it the first release of the two
would make the second one read the whole memory map, so that every `munmap` would cost time in
proportion to the number of mappings.

While a fork-like call of the target runs, between its entry and its exit, the tracer releases
nothing: the child maps the memfd offsets that the parent had at the fork until the tracer makes it
private at the fork event, so a release that another thread caused meanwhile would let the child
copy zeros, or the bytes of a mapping that reused the offsets. The releases wait and run at the exit
of the fork-like call.

## Memfd offsets

The memfd is a sparse file of 2^60 bytes divided into slots of 64 GiB. A mapping takes a free slot,
or several fresh ones when it is larger than a slot, and grows only within the slot of its last page
while nothing above it in that slot is used, so no growth reaches pages that another mapping uses;
an `mremap` growth beyond that moves the range to the start of a fresh slot when the new size fits
in one slot, and fails with `ENOMEM` otherwise. Memory that continues a memfd mapping, such as the
next piece of the heap, continues its offsets, so the kernel merges the two mappings instead of
adding one for every step. Released pages are punched at once, after the local mirror stopped
showing them, so the offsets are free immediately and memory that later reuses them reads as zero,
as glibc's `calloc` assumes. A slot of a converted file mapping records the file, so that its pieces
keep their path, and so that `MADV_FREE` on it still fails with `EINVAL`, as on a file mapping.

## Shared file mappings

A shared mapping of a regular file (which includes memfd and shmem files, so also
`MAP_SHARED | MAP_ANONYMOUS` memory and System V shared memory) is shown by mapping the same file
`MAP_SHARED` into the local mirror at the same file offset, so Automat and the target see each
other's writes. The file comes from `pidfd_getfd` of the descriptor at the exit of the `mmap`,
otherwise from `/proc/<pid>/map_files`, which root may open, otherwise from the path after a device
and inode check; when none works the mapping stays unmirrored. These mappings are never punched,
copied or converted: a fork leaves them shared, and `munmap`, `mremap` and `exec` only change the
local mirror. The mirror's own memfd and mappings of devices are not shown.

## fork, clone, threads and exec

Threads and other tasks created with `CLONE_VM` share the address space and are traced as part of
the same mirror; they are recognized with `kcmp(KCMP_VM)`, not by the clone flags. A child with an
address space of its own shares the memfd mappings with its parent until it gets its own memory, so
the tracer clears `CLONE_CHILD_SETTID` and `CLONE_CHILD_CLEARTID` at the entry of a fork, `clone` or
`clone3` without `CLONE_VM`: the kernel would otherwise write the child's tid into the parent's
thread descriptor, which glibc's recursive mutexes compare with. It also clears `CLONE_UNTRACED` at
the entry of every `clone` and `clone3`, also with `CLONE_VM`, and restores the flags at the exit: a
child created with it reports no event, so a fork child would keep the parent's memfd memory and a
thread would run untraced. At the fork event, before the forking thread resumes, the child gets
private anonymous copies of the memfd mappings, closes its inherited memfd descriptor, gets its tid
stores through an injected `set_tid_address`, which returns the tid in its own pid namespace, and is
let go. Then `on_forked` tells the consumer about the child, with a pidfd that the tracer opened
while the child was still traced, so that it cannot name a reused pid. The pidfd is valid only
during the call, because the tracer closes it when `on_forked` returns. A consumer that follows the
child duplicates that pidfd during the call and calls `Attach` of a new mirror on a thread of its
own, because `Attach` blocks; the tracer keeps no pointer to the new mirror. The tracer thread of
the new mirror seizes the child and converts the child's memory into a memfd of its own, so the
memory of a followed child is copied twice. The child runs briefly before its follower seizes it,
and a fork that it makes meanwhile is not followed. A child that shares the descriptor table of its
parent, made with `CLONE_FILES` and without `CLONE_VM`, keeps the inherited descriptor, which is the
parent's, and is only let go: `on_forked` is not called for it, because the memfd descriptor of a
mirror that followed it would land in that shared table. The other threads of the parent keep
running, so the copy of memory they write meanwhile may be torn. A fork made with a raw `clone` or
`clone3` is handled the same way, because the stop is at the system call.

`vfork` children share the address space and are never attached. A `CLONE_VM` task outside the
target's thread group that calls `exec` leaves the address space and is let go. An `exec` of the
target replaces the address space and closes the `O_CLOEXEC` memfd descriptor; the session goes on:
every old region is reported through `on_unmapped`, a new memfd is created in the new program, and
its memory is converted and reported through `on_mapped` and `on_attached`, as at attach. The other
threads of the target end at their exit stops before the exec event arrives. When a thread other
than the main one calls `exec`, its tid becomes the process id, and the tracer gives the process id
the task of the former tid. `CLONE_VM` tasks of other thread groups keep the old address space and
are let go as they are, with memfd memory.

## Signals, stops and exits

The target notices nothing of the tracer apart from the cost of the stops. A signal is delivered at
its signal-delivery stop with its original information. During an injection every signal is
blocked; a signal at whose delivery the task stopped is queued again by resuming the task with it
while it is blocked, so it arrives later with its original sender and value. A system call that an
interrupt cut short goes through the kernel's signal code again on the way back after an injection,
which restarts it as the kernel would. Group-stops keep working: a thread that enters a group-stop
is resumed with `PTRACE_LISTEN`, which keeps it stopped until `SIGCONT`, and a thread that was in a
group-stop when it was injected into returns to it through an interrupt.

The target's exit reaches its parent. Every task stops at its `PTRACE_EVENT_EXIT` stop before it
becomes a zombie, also when it is killed by `SIGKILL`; the tracer reads its exit status there and
detaches it, so the task exits untraced and its parent collects it, whether that parent is Automat
or another process. The session ends when its last task is gone, with the status of that task's exit
stop, which is the status that the parent receives. The tracer's wait still reports exits, for a
task that exits without an exit stop, and collects them. A parent in Automat waits for the pidfd of
a traced child before `waitpid`, because until the child is dead, `waitpid` from the tracer's own
process also reports, and collects, its ptrace stops.

The tracer never resumes a task past an exit stop that it can see: the task would exit traced, the
tracer's wait would collect it, and a parent in Automat would get no exit status. A task that the
tracer keeps stopped, while it handles the task's stop, runs an injected call in it or converts the
memory at attach, moves to its exit stop when it is killed, and that stop is not reported yet. So
before it resumes a task, also between the steps of an injected call, the tracer takes such a stop
with a non-blocking wait, and an injected call that reaches the exit stop ends there. A kill in the
moment between that wait and the resume is the one case left (see Limitations).

## The memfd descriptor in the target

The target may close or replace descriptor 900, for example with `close_range`, or through io_uring
without a system-call stop. Before each mapping of the memfd that it makes the target do, the tracer
checks with `kcmp(KCMP_FILE)` that the descriptor still refers to the memfd; when it does not, the
session ends with an error. No call that closes or replaces descriptors is intercepted.

## Detach, exit and crash

`Detach`, or an error such as a closed memfd descriptor or the installation of a seccomp filter,
ends the session cleanly: the tracer stops every thread, replaces every memfd mapping with private
anonymous memory holding the same bytes (the routine that makes a fork child private) and installs
its guard markers again, closes the target's memfd descriptor and detaches, so that the target is an
ordinary process again. Converted file mappings become anonymous memory with the same bytes. When
the target exits, the session ends with the exit status: OK for status 0, an error otherwise.

When Automat crashes, the kernel detaches the target, which keeps running on memfd memory. A later
fork of the target then lets the kernel write the child's tid into the parent's thread descriptor,
and `MADV_DONTNEED` no longer zeroes memfd pages, which breaks allocators such as jemalloc that rely
on it. `PTRACE_O_EXITKILL` is not used, because it would kill the user's process.

## Cost

Every system call of the target stops it twice, and each stop costs a round trip between the target
and the tracer thread, so the slowdown follows the target's system-call rate, not its memory. The
measured costs are in the report of the stress test `SystemCallCost`; the non-blocking wait for an
exit stop before each resume adds about 5% to them. A compute-bound target is barely affected. The
tracer thread of a mirror serializes the stops of all threads of its target, as strace does. Once
mirrored, memory access costs nothing: a byte is a plain load. A translation is one lookup in a tree
with one segment per region. The lookup takes a read guard of two atomic operations, and its cost
and the memory of the translation tables follow the number of regions, not their size.

## Limitations

- Every system call costs two stops (see Cost).
- A setuid program executed by the target while it is traced runs without its privileges.
- An already traced target cannot be attached, and a debugger cannot attach to a mirrored target.
- Converted memory loses transparent huge pages and the VMA flags that `mlock` and `madvise` set.
- After an unclean exit of Automat the target keeps memfd memory (see Detach, exit and crash).
- `MADV_DONTNEED` and `MADV_DONTNEED_LOCKED` on a converted private file mapping, or over a range
  that also covers memory that is not anonymous memfd memory, keep the memfd pages; a converted file
  mapping no longer shows later writes to the file; `MADV_REMOVE` succeeds on anonymous memfd
  memory, where private anonymous memory refuses it.
- `MADV_WIPEONFORK` over a range that also covers memory that is not anonymous memfd memory fails
  with `EINVAL`, and the registration with userfaultfd without shared-memory support fails on
  anonymous memory mapped after the attach, because it is shared memory.
- `MADV_DONTNEED` that one thread calls while another forks frees memfd pages that the child, not
  yet private, still maps, so the child reads zeros there.
- A `brk` that fails while another thread grows the heap returns an older break, which the tracer
  takes as a shrink of the heap.
- An `mremap` growth of memfd memory to more than 64 GiB that cannot stay in its slot fails with
  `ENOMEM`.
- A child of `clone` with `CLONE_VFORK` and without `CLONE_VM` reports no event that the tracer
  follows, so it keeps the parent's memfd memory until its `exec` or exit, and its writes reach the
  parent.
- Some changes of the memory map are not followed and leave the region set stale until a later call
  touches the range: 32-bit system calls through `int 0x80`, io_uring's `IORING_OP_MADVISE`, the
  memory operations of a vfork child before its `exec`, and System V `shmat` and `shmdt`.
- A fork child gets no guard markers in the memfd mappings of its parent, because the kernel copies
  no page tables of a shared mapping, so its copy of a guard page is an ordinary page. A private
  file mapping that becomes writable and an `mremap` growth that moves memfd memory to fresh offsets
  map over their range, which drops the guard markers inside it. A kernel that does not report guard
  pages in bit 58 of `/proc/<pid>/pagemap` cannot tell them from swapped pages, and mirrors them.
- Callbacks run on the tracer thread while a target thread waits, so slow callbacks slow the target.
- The memory of a followed fork child is copied twice, the child runs briefly before its follower
  seizes it, and a fork that it makes meanwhile is not followed. A fork child that shares the
  descriptor table of its parent cannot be followed.
- A task that is killed in the fraction of a microsecond between the tracer's check for an exit stop
  and its resume is resumed past its exit stop, because ptrace has no resume that depends on the
  stop that the tracer saw; the task then exits traced, the tracer's wait collects it, and a parent
  in Automat gets no exit status.
- `Detach`, or an attach, while a thread of a multithreaded target runs `exec` can deadlock the
  tracer thread: the thread that runs `exec` waits until the other threads have exited, they wait
  for the tracer at their exit stops, and the tracer waits for the thread that runs `exec` to stop.
- A `CLONE_VM` task of another thread group that outlives an `exec` of the target keeps memfd
  memory, with the consequences of an unclean exit of Automat.
- On a kernel that reports no exit stop for a task killed by `SIGKILL`, the tracer's wait collects
  such a target, and a parent in Automat gets `ECHILD` from `waitpid`; the `Status` of `Attach`
  still reports the exit status.
- A translation from another thread may be outdated by the time it is used.
