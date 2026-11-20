# Mem Meld

`MemMeld` (`src/mem_meld.hpp`, `src/mem_meld.cpp`) keeps a live copy of another
Linux process's memory inside Automat so that later code reads and writes the target with plain
loads and stores rather than a system call per access. It is a utility layer below Automat's object
layer. The `library::MemMeld` object, described at the end of this document, attaches a mirror
to the process of a Process object and is shown as a spear.

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
memory that has one, such as `[heap]` or `[stack]`; it is empty for other anonymous memory. A
region owns its path, so a copy of a region owns a copy of the path and stays valid on its own.
`ToLocal(remote, size)` returns the local bytes of `[remote, remote + size)` when the first and the
last byte of that range lie in one region, and an empty span otherwise. `ToRemote(local)` returns
the target address of a byte of the local mirror, or 0 when the byte lies outside every live region.
A `MemMeld::ReadGuard` is the only way to look up regions, and it is valid on every thread,
including the tracer thread inside a callback: its `Find(remote)` returns the region that
contains the address, and its `FindLowerBound(remote)` returns that region or else the next region
above the address, so that a loop from address 0 visits every region in ascending order. The
regions that a guard returns stay valid and unchanged until the guard is destroyed, and all lookups
through one guard read the same version of the table.

`Local`, `ToLocal` and `ToRemote` may be called from any thread without a lock, so that objects on
other threads can read and write the target's memory directly. Only the tracer thread writes the
translation tables, and each update of a table becomes visible to readers at one instant. An
operation that needs several updates, such as the split of a region or a move, becomes visible step
by step, so a reader may find no translation for an address whose mapping did not change. The table
keyed by remote addresses holds a segment that covers the remote range of each region, and the table
keyed by local addresses holds a segment that covers its local range. The value of a segment is the
page distance from its range to the other range, modulo 2^35 pages, together with the index of the
region. A translation therefore uses the value alone and never reads the region records.
`ToLocal` looks up the first byte and requires that the last byte lies in the same segment. The
extent behind `Local()` is an atomic, which grows only after the new local range is mapped.

The region records live in one array in reserved address space, so that a record never moves and
the array is readable while it grows. A published record is never edited: a change of protection, a
shrink and a move publish a new record and retire the old one, because a reader may hold the old
record at any moment and must see consistent fields. The tracer thread orders the updates of an
operation so that the regions that a loop over one guard visits never overlap: a shrink removes the
tail from the table before it publishes the record of the head. A retired record is reused only
after the grace period of the table keyed by remote addresses (see `docs/parrots/Maple Tree.md`): a
`MemMeld::ReadGuard` holds a read guard of that table, so a record that left the table is reused
only after every guard that could have reached it has ended. Readers reach records only through
that table, so only its epochs decide. The guard costs the two atomic operations of the read guard
of the tree. It never blocks the tracer thread. A guard that lives for a long time delays the reuse
of records and the freeing of replaced tree nodes, so a consumer should not hold a guard across
frames. A callback that needs a lookup takes a guard for the duration of the lookup.

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
mirror: the memfd, the offset allocator, the local mirror, the translation tables and the region
records. At each system-call stop it applies the change to the local mirror and the tables, calls
the callbacks, and only then resumes the target thread, so a callback sees the target's memory map
as the call left it. A consumer with expensive work forwards it to another thread itself.

`Attach` may be called once. It duplicates the pidfd of the target, so the caller keeps its own
descriptor. When `Attach` returns, its `Status` reports the outcome of the session: OK when `Detach`
ended it or the target exited with status 0, otherwise an error that says how the target exited or
why the attach or the session failed. The errors found before the session starts are reported
through the same `Status`: a second call, an invalid pidfd, a pidfd whose process id cannot be read
and a failed reservation of the local mirror or of the region records. No callback reports the end
of the session; a consumer that reacts to it does so after `Attach` returns. While the session
runs, the wake signal is unblocked in the tracer thread, and the signal mask of the thread is
restored when `Attach` returns.

`Detach` is the only function that another thread calls to control the session (`Local`,
`ToLocal`, `ToRemote` and `MemMeld::ReadGuard` need no lock). It sets a flag of the mirror, which
the tracer thread reads before each wait, and interrupts the tracer's `waitid` with a signal whose
handler does nothing. A signal that arrives just before the wait is lost, so `Detach` sends it
again every millisecond until the session has ended. When `Detach` returns while `Attach` runs, no
callback runs afterwards. A callback runs on the tracer thread and may call `Detach`, which then
only sets the flag, and the session ends when the callback returns. The flag stays set when
`Detach` is called before `Attach`, so that `Attach` detaches again as soon as it has attached.

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
- `shmat` runs unchanged; at its exit the mapping at the returned address is shown from its file
  (see Shared file mappings), so the local mirror has the segment before the target thread
  continues. `shmdt` names only an address, so its entry records the mapping at that address, and
  its exit reads the range of that mapping again, which removes the region.
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
and inode check; when none works the mapping stays unmirrored. A System V segment has no descriptor
and no path that can be opened, so it is mirrored only when the tracer runs as root. These mappings
are never punched, copied or converted: a fork leaves them shared, and `munmap`, `shmdt`, `mremap`
and `exec` only change the local mirror. The mirror's own memfd and mappings of devices are not
shown.

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
  memory operations of a vfork child before its `exec`, and the `shmdt` of a segment whose mapping
  was split into several mappings, which removes only the first of them from the local mirror.
- A System V segment is mirrored only when the tracer runs as root (see Shared file mappings).
- `shmat` with `SHM_REMAP` over memfd memory keeps the memfd pages of the memory that it replaces
  until the session ends.
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
- A translation from another thread may be outdated by the time it is used, and so may the
  regions that a guard returns.
- A guard that is held for a long time keeps every record and tree node retired meanwhile
  allocated.

## The MemMeld object and its spear

`library::MemMeld` (`src/library_mem_meld.hpp`, `src/library_mem_meld.cpp`) is the Object that owns an
`automat::MemMeld` and is shown as a spear that sticks out of the globe of its Process. The spear
consists of two toys: the spear tip is the toy of the object (`MemMeldToy`), and the shaft is the toy
of its argument `process` (`SpearShaft`). It is registered with `HideInToolbar`
(`src/prototypes.cpp`), because a MemMeld is made by taking the tip from a globe. The spear exists
only where `automat::MemMeld` is supported, at present Linux: on other platforms
`MakeSpearTip` in `src/library_process.cpp` returns null and no globe shows a tip.

### The object

- The argument `process` is an `ObjectArgument<Process>`. Its state stores a weak pointer to the
  Process. The serializer of the board saves it with the other connected arguments under the key
  `links` (`src/object.cpp`), and loading connects it again (`src/persistence.cpp`), so the object
  has no state of its own to save.
- `Process::mem_meld` names the MemMeld of a process, and the globe reads it to decide whether it
  holds a tip. It is written in one place: `process_Impl::OnConnect` records the MemMeld there when
  the field is expired. A field that names a living MemMeld is kept, so a second MemMeld of the same
  process cannot take the place of the first one. The globe decides whether it holds a tip in its
  `Tick`, so `process_Impl::OnConnect` and the destructor of `MemMeld` call `WakeToys` on the
  Process, which bumps the monitor that the globe observes.
- One MemMeld exists per attachment: the object is made for one session of `automat::MemMeld`,
  whose `Attach` is single-use. `Start` does its work at most once per object (`start_once`, a
  `Once` from `src/once.hpp`, one atomic byte whose `First` is true only for the first caller).
  `Start` returns at once: `attach_thread`, a `std::jthread` owned by the object, runs `Attach`
  until the session ends.
- `attach_thread` is the last data member, so it is destroyed first and joined while `process` and
  `mem_meld` still exist, which the end of the thread function uses. The destructor of `MemMeld` calls
  `Detach`, which returns after the target has its private memory back. Deleting the last owner of
  the object therefore detaches the mirror.
- The callbacks of `Start` (`on_attached`, `on_changed`, `on_mapped`) and the end of the thread
  function call `WakeBothToys`: `WakeToys` bumps the monitor of the object, which the tip observes,
  and `process->WakeToys` bumps the monitor of the argument, which the shaft observes.
- Every failure of `Start` is reported with `ReportError` (no running Process, `pidfd_open`, a status
  of `Attach` that is not OK), and the tip shows the error flames, which `ObjectToy::OnWake`
  updates through `ObjectToy::UpdateErrorFlames`.
- A copy (`Clone`, through the copy constructor) has the same target in `process` and nothing else.
  It is not attached, and it is not recorded in `Process::mem_meld`, because the copy constructor
  does not call `OnConnect`. A copy is started with its `start` Command. The Clone entry of the menu
  gives the same object another owner (`docs/parrots/Boards.md`); it is the same MemMeld with
  another Location.

### The spear tip and the shaft

- One visible thing is one widget for its whole life. The tip is the toy of the object, because the
  same widget is shown on the globe behind the ball, through the drag and on the board. A grab
  re-binds this widget to the MemMeld that the grab makes (`Toy::SetOwner`) instead of replacing it,
  so there is exactly one tip in every frame.
- The shaft is the toy of the argument, because it is what links the MemMeld to its Process. Where it
  connects two toys on one board it is an ordinary connection toy of that board: made through
  `Argument::MakeToy`, ordered by the board, split under its end points by `ArgumentToy::TickSplits`
  and anchored at both ends. The `MemMeldToy` therefore stays small, and its shape is the outline of
  the tip.
- `MemMeldToy` has its origin at the apex of the tip (`CenteredAtZero`) and draws itself turned by
  `turn` around the origin. Its `Shape` is the outline of the tip turned by `turn`; it is what the
  pointer touches and what `ErrorFlames` and `LocationWidget` use. Its `DrawBounds` include the
  bounds of the shaft while it holds the shaft, because the renderer does not visit the children of a
  widget whose bounds lie outside the window, and a free shaft can reach back into the window.
- `SpearShaft` has an empty `Shape`, so it does not react to the pointer: the MemMeld is grabbed by
  its tip. Its `DrawnUnderEndpoints` is true.
- The tip and the shaft are made together: the constructor of `MemMeldToy` makes its shaft and
  orders it below itself. The shaft has a `MortalPtr` to its tip.

### Who holds the shaft

- The tip toy holds the shaft in `MemMeldToy::shaft` from construction until a board first asks for
  the toy of the argument. Until then the shaft has the owner that the tip had at construction,
  which is `dead_object` for a tip made by a globe.
- `BoardWidget::Tick` asks for the toy of an argument with `toys.FindOrMake(arg, this)` when its end
  is on the same board (`src/board.cpp`). `Argument::MakeToy` returns the result of `make_toy` when
  the table sets it, and `process_Impl::OnMakeToy` hands over the existing shaft of the tip toy in
  the same scope: it binds the shaft to the MemMeld of the argument with `Toy::SetOwner` and
  reparents it to the board. `SetOwner` is called there because the `Tick` of the board runs before
  the `Tick` of the tip, and the tip toy may have been re-bound from `dead_object` in the same
  event that put it on the board.
- The shaft has two owners: its tip toy (`MemMeldToy::shaft`) or the scope of a board. The toy of
  the argument is owned by the specific toy `MemMeldToy`, not by `LocationWidget` or `ObjectToy`,
  and the extra API stays minimal: `make_toy` handles the way into a board, and `destroy_toy`
  handles the way out (`docs/parrots/Boards.md`).
- When the Location of the MemMeld leaves a board, `DragLocationAction::Extract` takes the shaft out
  of the scope and passes it to `destroy_toy`. `process_Impl::OnDestroyToy` stores it in the tip
  that it belongs to (`SpearShaft::tip`), reparents it to the tip and orders it below the tip; when
  the tip is gone the shaft is destroyed. While the tip holds the shaft, `MemMeldToy::OnPoll` polls
  it.
- When only the globe leaves the board, the shaft stays in the scope of the board and is drawn with
  a free end. A spear on a board without its globe keeps its shaft in the tip, because that board
  does not ask for the toy of the argument.
- `SpearShaft::OnReparent` applies the fix, wakes the shaft and calls `RedrawThisFrame`, because a
  board assigns the identity to its transform right after it takes the toy, so the texture that the
  shaft drew in the coordinates of its previous parent must not be shown.
- `SpearShaft::Tick` marks the shaft dead when its tip toy is gone. While the shaft is not a child of
  its tip, its `alpha` is the alpha of the `LocationWidget` that holds the tip, the rule of ordinary
  connection toys (`TickVisibility` in `src/ui_connection_widget.cpp`), so a spear whose Location is
  deleted fades as one. A shaft that a board holds and whose MemMeld is gone returns after these
  two steps and keeps its pose, its covers and its ranges, as `TickLifecycle` only fades a
  connection toy whose start argument cannot be locked; without this the shaft would lose its
  globe, because the end of a dead argument cannot be found, and would swing to the free pose while
  it fades. A shaft that is a child of its tip always computes its pose, also in the dead state, in
  which its owner is always expired.
- `kVisibleWhenDisconnected` is false, so a board asks for the toy of the argument only while both
  ends have a Location on it.

### The tip on the globe

- `dead_object` (`src/object.hpp`, `src/object.cpp`) is a global `Object` with no owning reference
  and one weak reference. A `WeakPtr` taken from it is expired and cannot be locked, and its memory
  is never released. The instance is `[[clang::no_destroy]]`, because its destructor would read
  globals of `src/object_lifetime.cpp` that may be destroyed already, and toys may still hold weak
  pointers to it. A toy constructed with `dead_object` as its owner is in the dead state: it has no
  object, and its menu is empty, because `ObjectToy::FillMenu` adds nothing without an owner. The
  dead state lets a toy exist without an object through the existing constructors of its class and
  with a real monitor. The constructor of `ArgumentToy` (`src/argument.hpp`) takes the monitor of
  the object itself for `dead_object`, because the dead object has no state of any argument.
- While no MemMeld of the process exists (`Process::mem_meld` expired), a globe that is not
  iconified holds a tip toy in the dead state in `ProcessToy::spear_tip`, made by
  `MemMeld::MakeDeadToy` through `MakeSpearTip` (`src/library_process.cpp`). Its shaft is in the
  dead state too. An iconified globe (the plate of a Program Launcher, the menu icon of a Process
  option) and a globe whose process has a MemMeld hold no tip. `ProcessToy::Tick` makes the tip and
  drops it. A toy in the dead state has no object to observe, so the globe does not poll it
  (`ProcessToy` has no `OnPoll`).
- The tip is ordered below the globe, so the shaft appears to come from behind the ball. The globe
  assigns `1_mm` to the `shadow_elevation` of the tip once, when it makes the tip, because
  on a board the `LocationWidget` assigns it to its toy, and the tip on the globe has no
  `LocationWidget`.
- The grab area of the tip on the globe is the outline of the tip. `MemMeldToy::FindOption` returns
  the table `kMakeMemMeld` bound to the Process for the left button while the parent of the tip is
  the toy of a Process. A right press opens the menu of the globe: the menu of the tip is empty, so
  `Pointer::ButtonDown` (`src/pointer.cpp`) asks the parent.
- `kMakeMemMeld` (declared in `src/library_process.hpp`, defined in `src/library_process.cpp`) is an
  `ObjectSource::Table` with `Cursor::Copy`. Its `take` calls `AttachMemMeld`, which makes a
  MemMeld, connects its `process` to the Process (`OnConnect` records it in `Process::mem_meld`) and
  calls `Start`, so grabbing the tip attaches the mirror. Its `activate` receives the tip toy, finds
  the globe as its parent, takes the MemMeld, re-binds the tip toy to it with `Toy::SetOwner` and
  hands the MemMeld and the tip toy to `DragNew`.
- A toy is given another owner only while no `ToyScope` stores it, because the key of a scope is the
  owner. The tip on the globe is held by the globe and stored in no scope, so the take can re-bind
  it.
- After menu Delete of a MemMeld the tip toy and the shaft fade with their Location like every
  other toy, and the globe makes a new tip in the dead state, which rises from behind the ball. A
  Delete changes only the VM, so no widget gets a new parent at that moment, and a widget that is
  moved between two subtrees during `Tick` can have two nodes in the tree of the renderer.
- A drop into the black hole destroys the tip toy with the MemMeld, so the globe makes a new tip in
  the dead state, which rises from behind the ball.
- The position of the tip on the globe is a property of the spear, so the tip toy holds it and the
  toy of the Process holds no property of another toy. While the parent of the tip is the toy of a
  Process, `MemMeldToy::Tick` places the tip `spear_length` above the first of the
  `ConnectionPositions` of its parent and animates the `Kinetic` value `spear_length`
  (`SineTowards`, `kSpearLengthPeriod`) towards `kSpearLengthOnGlobe` (`src/library_mem_meld.cpp`).
  A new tip starts at `kSpearHiddenLength`, where the tip is behind the ball. The globe only makes
  and drops the tip, orders it below itself and assigns its `shadow_elevation`.

### The geometry of the shaft

`SpearShaft::Tick` owns the geometry of the spear. It is written as titled blocks.

- The globe is the toy that holds the tip (the parent of the tip toy, when it is an `ObjectToy`
  whose owner is a Process; a `LocationWidget` is also an `ObjectToy`, so the owner is tested), or
  else, while `EndLocation` is not null, the toy of the Process in the scope of the tip toy. A globe
  that was deleted keeps its toy in the scope while it fades, but it has no Location and does not
  count.
- The apex is the origin of the tip toy. The blunt end is the first of the
  `ObjectToy::ConnectionPositions` of the globe (the centre of the ball, `ProcessToy`), and without a
  globe it lies `kFreeLength` straight below the apex. The free shaft is kept because it is where the
  memory is shown when the spear is not on the board of its globe.
- While the board holds the shaft and a pointer holds the toy at one end, the end point is taken from
  the pointer-bound texture anchor of that toy (`PositionOf`, with the anchor from `GrabAnchor`): it
  is the place where the compositor draws that point, not the place that the transform of the toy
  reports. `LocationWidget` moves a dragged toy towards the pointer with a spring, and the
  compositor shifts its texture by the rest of the way; a shaft drawn towards the transform would
  bend at the tip. While the shaft is a child of its tip, the shaft is drawn in the space of the tip
  and moves with its texture, so the transform is used.
- Length and direction are a base plus a remainder, the pattern of `SyncBelt`. `length` and
  `degrees` are the base, assigned in every `Tick` from the two end points, so an end that the
  pointer moves is never behind it. `length_deflection` and `degrees_deflection` are the remainder:
  `Kinetic` values that decay towards zero with `SineTowards` (`kBluntEndLengthPeriod`,
  `kBluntEndAnglePeriod`). The drawn spear uses the sums (`SpearLength`, `Direction`, `BluntEnd`).
- When the spear gains or loses its globe (`had_globe`), the remainder takes up the difference
  between the base of the last `Tick` and the current base. The apex stays where it is and the
  blunt end swings between the ball and the free position. The first `Tick` sets no remainder, so a
  loaded spear on its globe appears in place.
- The shaft then turns its tip: it writes `tip->turn`, and when the value changed it calls
  `RedrawThisFrame` and wakes the tip toy. Render jobs are chosen after every `Tick` of the frame, so
  the tip is drawn with the angle of the shaft in the same frame. `MemMeldToy::Tick` returns
  `Tock::Shape`, so the shape of the tip follows the turn.
- `hidden` is the part of the shaft that the globe covers: `Raycast` on `globe->shape` from outside
  the globe towards the blunt end finds where the shaft leaves the outline, plus `kHiddenMargin`.
  Without a globe it is 0. It is used only by `Draw`, as the place where the address pattern starts.

### Texture anchors and covers

- While the shaft is a child of its tip, it has no texture anchors. A detached child without
  anchors of its own takes the anchors of its parent (the anchor pass in `src/renderer.cpp`), so the
  shaft is warped together with its tip. The blunt end has an anchor only while the board holds
  the shaft and a globe is found, because only then does the globe hold it in place.
- While the board holds the shaft, it has the anchor `kApexAnchorId` at the apex, and while a globe
  is found also `kBluntEndAnchorId` at the blunt end. Each one is a copy of the pointer-bound anchor
  of the toy at that end, moved to the end point (the same pointer, decay and radius), when a
  pointer holds that toy, and a plain anchor at the end point otherwise. The compositor then draws
  both ends of the shaft where it draws the toys at the ends. Both ends use the same rule, so a
  dragged globe pulls the blunt end with it.
- `TickSplits` runs in every `Tick`. While the board holds the shaft, the shaft is drawn under the
  tip and under the globe (`DrawnUnderEndpoints`, `docs/parrots/Split Widgets.md`), so the globe
  covers the part of the shaft behind the ball. While the globe holds the tip, the shaft is a child
  of the tip, which is ordered below the globe, so the ball covers it without a split.
- The shaft copies the `shadow_elevation` of its tip.

### The address pattern

`SpearShaft::Draw` draws the visible shaft along a centre line with one segment per merged range of
mapped memory, with a length proportional to its bytes, and one zig-zag for every gap, the first and
the last included. The pattern starts where the shaft leaves the globe (`hidden`) and ends below the
tip (`kShaftTop`); the part behind the globe is a plain rod. `SpearShaft::Tick` rebuilds
`address_ranges` from a `ReadGuard` when `Toy::observed_monitor` differs from the value stored at
the last rebuild, merging adjacent regions, so `Tick` loads no monitor. A shaft in the dead
state has no address ranges and is a plain rod; a shaft whose MemMeld is gone keeps the ranges
that it has.

### The shading

- The light comes from the upper left (`kLight`) at every angle of the spear. The shading that
  depends on the light is computed in `Draw` from the turn of the spear: the gradient of the gold
  ring, the sweep gradient of its edges (rotated against the turn), the brightness of the two halves
  of the steel tip, the ridge and the highlight of the shaft. The two `RasterPatch` caches of
  `MemMeldToy::Draw` are drawn through the rotated canvas and hold only detail that does not depend
  on the light (scratches and shadows on the ring, dents and specks on the tip).
- The ring refers to the IDIC of Star Trek: its hole is eccentric and the apex of the tip lies
  inside it.
- The shaft is a data display, so it stays close to black and keeps the weight of a black
  silhouette. It is drawn in two layers. The silhouette is the outline of the pattern filled with
  `kSteelShade`, antialiased. The highlight on top is a triangle strip along the centre line with two
  vertices at every corner of the line; its texture coordinates are the positions with the offset of
  the zig-zag removed, so the gradient across the rod (`kShaftHighlight`, whose alpha rises from 0
  at both edges to 1 on the lit side of the axis) is sheared along every zig-zag piece.
- The highlight is not antialiased, because `drawVertices` has no edge antialiasing in Graphite; it
  needs none, because it is transparent at the edges of the rod, where the antialiased silhouette
  lies.

### Persistence and limits

- A loaded MemMeld is connected to its Process (loading calls `Connect`, and `OnConnect` records it
  in `Process::mem_meld`, so the globe shows no tip next to it), but it is not attached. It is started
  with its `start` Command.
- A second MemMeld cannot attach to a process that another MemMeld has attached to; its `Start`
  reports the failure of the seize.
- `Start` works once per object, so a MemMeld whose session ended cannot attach again (the TODO
  in `src/library_mem_meld.hpp`).
- A copy that outlives the MemMeld recorded in `Process::mem_meld` is not recorded there, so the
  globe shows a tip again while the copy exists.
