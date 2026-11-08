# Maple Tree

## Problem

Some data in Automat is keyed by ranges of 64-bit integers: one value applies to every key from
`begin` up to, but not including, `end`. Such a map has from a hundred to a hundred thousand
segments, most of them large and far apart. It is read up to ten million times per second from up to
sixteen threads, and it is changed by one thread up to a hundred thousand times per second. A reader
must not wait for the writer, must not see a half-applied change, and must not touch memory that
the writer has freed.

## Solution

`automat::MapleTree` (src/maple_tree.hpp, src/maple_tree.cpp) is a B-tree over ranges, modelled on
the maple tree of the Linux kernel. Its nodes are never changed after they become reachable by
readers. A write builds new copies of the nodes on the paths that it changes, shares every other
node with the previous version, and makes the new version visible with one store of the root
pointer. A reader loads the root pointer once and walks nodes that do not change under it. Path
copying is used because the reader then needs no version check and no retry, and because each
write becomes visible at one instant without any extra mechanism.

Nodes that a write replaces are retired and freed later, when no reader can reach them any more.
Readers announce themselves in counters inside the tree, split by the parity of an epoch number.
The writer advances the epoch and frees the retired nodes once the counters of the previous parity
are zero.

## Contract

The tree stores disjoint, non-empty segments, each with a non-zero value. A `MapleTree::Segment`
`{begin, end, value}` covers the keys `k` with `begin <= k < end`. The key domain is
`0 <= key < UINT64_MAX`: `UINT64_MAX` is the largest representable end, so the key `UINT64_MAX` can
be neither stored nor queried. Half-open ranges were chosen so that adjacent segments share a bound
and the code needs no `+ 1` and `- 1` at segment boundaries. Value zero means that a key is absent;
it is never stored.

- `Update(begin, end, value)` requires `begin <= end`, which an assertion checks in debug builds.
  It makes `[begin, end)` one stored segment with `value`. Stored segments that overlapped it are
  cut, and their parts outside `[begin, end)` keep their old values. A `value` of zero erases
  instead. `Erase` and `Clear` are such updates; `Clear` covers `[0, UINT64_MAX)`. An empty range
  (`begin == end`) changes nothing. Adjacent segments, such as `[a, b)` and `[b, c)`, are never
  merged, also not when their values are equal.
- `Query(key)` requires `key < UINT64_MAX` and returns the stored segment that contains `key`. For
  an absent key it returns value zero and the longest run of absent keys that contains `key`.
- `QueryLowerBound(key)` requires `key < UINT64_MAX` and returns the first stored segment whose
  `end` is above `key`: the segment that contains `key` or else the next one. When there is none,
  it returns value zero and the trailing absent run.
- `QueryRange(begin, end)` requires `begin <= end` and returns every stored segment that intersects
  `[begin, end)`, in ascending order and not clipped to the query. An empty range returns nothing.

There is one writer at a time, and the callers serialize the writers. `Update`, `Erase`, `Clear`,
`TryReclaim`, `Reclaim` and the introspection functions (`NodeCount`, `Depth`, `RetiredCount`,
`AllocatedCount`, `Validate`) are writer operations. Any number of threads read concurrently with
the writer, without registration. `Reclaim` must not be called by a thread that holds a guard of
the same tree, because it waits for that guard and never returns. `TryReclaim` and the writes are
safe on such a thread.

A read takes a `MapleTree::ReadGuard`. The reads of the tree itself take a guard for their own
duration, and several reads through one guard see one version of the tree. A read never blocks,
never allocates and makes no system call. `QueryRange` is the exception to the allocation rule,
because it returns its result in a `std::vector`. The writer never waits for readers inside
`Update`, `Erase` or `Clear`.

A guard that lives for a long time delays reclamation and nothing else: the writer keeps working,
the guard keeps reading its version, and the nodes retired while the guard is alive stay allocated
until it ends, together with the nodes retired in the epoch in which it started, which are usually
those of the last write before it started. The destructor of the tree requires that no guard is
alive.

## Nodes

A node is 256 bytes, four cache lines, aligned to 64 bytes. It holds sixteen values, fifteen
pivots, the number of used values and a flag that tells leaves from inner nodes. The count and the
flag come first, because every visit reads the count before the pivots. A value of a leaf is a
stored value or zero; a value of an inner node is a pointer to a child, stored in the same 64-bit
word. All leaves are at the same depth.

Pivots are exclusive ends: pivot `i` is the end of value `i` and the begin of value `i + 1`. The
first value begins at the begin of the node and the last used value ends at the end of the node;
both bounds come from the parent, and the root covers `[0, UINT64_MAX)`. A reader therefore
computes the segment of a value from the pivots that it has already compared.

The values of the leaves cover the whole key domain. A run of absent keys is a value of zero, and
two neighbouring values, also in different leaves, are never both absent. Every key lies in exactly
one leaf value, so a lookup has no "not found" path, and the segment of an absent key is the
longest absent run that contains it. `QueryLowerBound` follows from this: when the segment of `key`
is absent, the segment that begins at its end is stored. An empty tree is one leaf with one absent
value.

Sixteen values keep the tree shallow (five to six levels for a hundred thousand segments, because
ascending insertion leaves nodes about half full) while a lookup scans at most fifteen pivots,
which fill two cache lines. A linear scan is used because it is simple and because the pivots of one
node are already in the cache after the first comparison.

## Lookup

`ReadGuard::Query` starts at the root and, in every node, takes the first value whose pivot is above
the key, or the last used value. It narrows the segment to that value and continues in the child
until it reaches a leaf, whose value gives the segment. `QueryLowerBound` performs one or two
lookups. `QueryRange` performs one lookup per returned segment and per absent run between them; each
lookup starts at the root, which is simple and is fast enough for the expected sizes.

## Writes

A write replaces a run of consecutive nodes on each level with new nodes, starting at the leaves:

1. The leaf run is the set of leaves that contain the key `begin - 1`, the key `end` and every key
   between them, clipped to the key domain. The two neighbouring keys are included so that a new
   absent value can be merged with an absent neighbour in the same pass. The values of these leaves
   are copied, `[begin, end)` is written over them (cutting the values that it overlaps), and
   neighbouring absent values are merged.
2. On every level, if the new values are fewer than the minimum fill of eight and the run does not
   already span the whole level, the run is extended by the neighbouring node on the left (or on
   the right at the start of the domain), whose values are added to the new values.
3. The new values are cut into `ceil(n / 16)` nodes of equal size. Any count of seventeen or more
   gives nodes of at least eight values, so the minimum fill holds after step 2.
4. The run of the next level is the set of parent nodes that cover the keys of the current run.
   Their values are copied, and the values that point to replaced nodes are replaced by the new
   nodes.
5. The loop stops at the old root level, or above it if the root was split, when exactly one node
   remains. That node becomes the root. When the run spans the whole level and the new values are a
   single child, no node is built for them and the child is carried to the next level. A root
   with one child therefore never exists, and a tree shrinks by as many levels as needed.

Every node of every run is retired. Every other node is shared with the previous version. A write
of a wide range visits all nodes under that range, because they must be retired; `Clear` is an
update of zero over the whole domain.

The rebalancing rules are therefore: a node never has more than sixteen values and never fewer than
eight, except the root, which has at least one value as a leaf and at least two as an inner node.
A node that would fall below the minimum is joined with a neighbour and the result is cut again, so
merging and borrowing are the same operation. A node is found by a descent from the root, so the
tree needs no parent pointers, which path copying would make impossible to keep.

## Reclamation

The tree keeps an epoch number and 64 reader shards (4 KiB). Each shard is a cache line with one
reader counter per epoch parity. Sixteen reader threads hashed into 16 shards almost always share
some shards, which made a guard cost about 40 to 50 nanoseconds instead of about 25 with 64
shards. Memory freed by the tree is only ever node memory, allocated with plain `new` and freed
with `delete` so that AddressSanitizer sees every use after free.

A reader picks a shard from a hash of its thread identity, so that different threads mostly use
different cache lines. To enter, it loads the epoch `e`, increments the counter of parity `e & 1`,
and loads the epoch again. If the epoch changed, it decrements the counter and starts over.
Otherwise it loads the root and reads. To leave, it decrements the counter.

The writer keeps two lists of retired nodes: `retired`, the nodes retired since the last advance of
the epoch, and `waiting`, the nodes retired before the last advance. `TryReclaim` checks whether the
counters of the parity opposite to the current epoch are zero in all shards. If they are, it frees
`waiting`, moves `retired` into `waiting`, advances the epoch by one and returns true. Otherwise it
does nothing and returns false. Every write calls `TryReclaim` first. `Reclaim` performs this step
twice, and before each step it yields until the check passes, so it frees everything retired before
the call and returns after every guard that existed at the call has ended.

### Why it is safe

The argument uses the single total order of all `seq_cst` operations. Let `E` be the epoch when a
check of `TryReclaim` passes.

Claim 1: every reader that entered with an epoch below `E` has left. A reader entered with epoch
`e` when its second epoch load returned `e`. That load follows its counter increment, and it
precedes the store of `e + 1` in the total order, because it would otherwise have returned
`e + 1` or later. The check follows the store of `E`. So for a reader with epoch `E - 1`, its
increment precedes the check in the total order. The check therefore reads a counter value that is
not older than the increment in the modification order of the counter. This step needs the C++20
rule on coherence-ordered-before for `seq_cst` operations: if the check read an older value, it
would be coherence-ordered-before the increment, and since both are `seq_cst`, it would precede the
increment in the total order. The decrements are not `seq_cst`, so the total order alone does not
give this step, and under the C++17 wording it does not hold.

A zero then means that the reader's decrement happened as well. The increments and decrements of
one reader alternate in the modification order of its counter, because they are operations of one
thread on one object. A prefix of the modification order therefore contributes one for every
reader whose increment it contains without the matching decrement, and zero for every other
reader. A prefix that contains the increment and sums to zero therefore contains the decrement.
The check reads from that decrement or from a later read-modify-write, which belongs to the release
sequence of the decrement, so the decrement synchronizes with the check.

The readers with epochs `E - 3`, `E - 5` and so on use the same counters and are covered in the
same way. The readers with epochs `E - 2`, `E - 4` and so on use the other counters. The check that
passed in epoch `E - 1`, just before the epoch became `E`, showed by the same argument that every
reader with an epoch below `E - 1` had left, and no reader can enter with such an epoch after the
epoch became `E - 1`. Here "after" means after in the total order, not after in time: a reader may
load an old epoch at any time, but its second load then follows the store of `E - 1` in the total
order, returns a newer epoch, and the reader retries. The first check, in epoch zero, has no
earlier readers to account for.

Claim 2: a reader that entered with an epoch of `E` or more cannot reach a node that was retired
before the epoch became `E`. The write that retired the node stored its new root before the epoch
store of `E` in program order. The reader's second epoch load, an acquire load, read the store of
`E` or a later store of the same thread, so the root store happens before the reader's root load,
which therefore returns that root or a later one. Neither reaches the node: a write links only new
nodes and nodes that are reachable from the current root, so a retired node never becomes reachable
again. "The same thread" assumes one writer thread. When the caller serializes writers on several
threads, the caller's synchronization orders the stores of one writer before those of the next, and
the claim holds through it.

`waiting` holds only nodes retired before the epoch became `E`, so by both claims no reader can
reach them, and `TryReclaim` frees them. The decrement that ends a read is a release operation and
the check that observed it is an acquire load, which is the current check or, for the other parity,
the check before the last advance. Either one precedes the `delete` in the writer's program order,
so the reads of a finished reader happen before the `delete`.

The race between a reader that loads the epoch and a writer that advances it ends in one of two
ways. If the reader's first load returns the old epoch and the writer advances it before the
reader's second load, the second load returns the new epoch and the reader retries, after it
undoes its increment. If the second load also returns the old epoch, the reader is counted under
the old parity, which the writer checks before it frees what the reader may reach. A counter that
is briefly non-zero because of a retrying reader only makes a check fail; the next write or call
retries it.

### Memory orders

The `seq_cst` operations are the reader's counter increment, the reader's second epoch load, the
writer's epoch store and the writer's counter loads in the check. They form the store-buffering
pattern: the writer stores the epoch and then loads a counter, the reader modifies a counter and
then loads the epoch. With weaker orders both sides may read the old values, the reader proceeds
with the old epoch while the writer sees no reader, and the writer frees nodes that the reader is
about to read.

All other atomic operations are weaker. The reader's first epoch load is `relaxed`, because the
second load decides. The decrement that undoes a failed entry is `relaxed`, because the reader read
nothing. The decrement that ends a read is `release`. The root store is `release` and the reader's
root load is `acquire`, to publish the contents of the new nodes. The writer reads its own root and
epoch with `relaxed` loads, because only the writer stores them.

A reader therefore executes two atomic read-modify-write operations per guard, both on a cache line
that it shares only with threads that hash to the same shard, and no other synchronization.

The `seq_cst` choices cannot be tested on x86: a lock-prefixed instruction is a full barrier
whatever its memory order, and ThreadSanitizer does not model store buffering. They are protected by
the proof alone. A missing `release` or `acquire` is found by running the concurrency tests under
ThreadSanitizer, so every change of the reclamation code needs that run.

## Limits

- One writer at a time. The tree does not detect concurrent writers.
- The key `UINT64_MAX` cannot be stored or queried, because it is the exclusive end of the
  domain.
- A write copies the whole path from the changed leaves to the root, so it allocates about one node
  per level, and more when it splits, joins or spans many leaves. A write that changes nothing,
  such as an erase of an absent run or an update with an identical segment, still copies its
  path. An update of an empty range returns at once.
- An update or erase of a range that covers many leaves visits and retires all of them.
- `QueryRange` starts every step at the root, so it costs one lookup per returned segment and per
  absent run between them.
- Reclamation progresses only through writes, `TryReclaim` and `Reclaim`. Retired nodes stay
  allocated while no writer operation runs, and while a guard is alive that started in the epoch
  in which they were retired or earlier.
- While a guard is alive, the writer keeps about one node per tree level per write, about 1.6 KiB
  per write at a hundred thousand segments. These nodes are freed only after the guards that started
  before the write have ended. The two retired lists keep their peak capacity.
- A reader that is preempted between its counter increment and its second epoch load holds up the
  next advance of the epoch, although it holds no guard yet.
- `Reclaim` must not be called by a thread that holds a guard of the same tree; it never
  returns. `TryReclaim` is safe there.
- The shard count is a constant in the header. Threads that share a shard contend on its cache
  line.
- A guard must not outlive the tree.
- Every write advances the epoch when the check passes. A reader retries its entry only when an
  epoch store falls between its two epoch loads, which was measured at 0.1 to 0.5 percent of the
  loads at a hundred thousand writes per second.
- Allocation failure ends the program: `Update`, `Erase` and `Clear` are `noexcept`, because a write
  that stopped halfway would leave live nodes in the retired lists.
