# Object Ownership

## Problem

An Object can be held by several owners at once: a Location on every board that shows it, a
Timeline for each of its tracks, a Program Launcher for its launch. Objects need to find their
owners: to reach the board they are placed on, to schedule work keyed by their Location, to wake
the connection widgets of the boards they appear on. Scanning every board for the owner does not
scale to the intended size of Automat (millions of objects, most of them nested inside others),
and a scan needs a global lock.

## Owner links

Structural ownership is expressed with `Owned<T>` (src/object.hpp). An `Owned<T>` holds a
`Ptr<T>` and is at the same time a node in an intrusive circular list kept by the owned object
(`Object::owners`). The node records the owning Object, so the owned object enumerates its owners
through `Object::Owners()` in constant time per owner. The link registers itself when it receives
a target, unregisters itself when it loses the target or is destroyed, and re-registers itself in
place when it is moved, so links can live in a `deque` or a `Vec` whose elements shift. Links are
not copyable, because a copy would register the same owner twice. Unlike `Ptr`, a link is not
trivially relocatable.

Owners are listed in the order in which they took the object. `Object::MyLocation()` returns the
first owner that is a Location on a board, and `Board::LocationOrNull` returns the owner Location
that belongs to the given board. Both replace scans of `engine.boards`.

The structural holders are `Location::object`, `Board::locations`, `Timeline::tracks` and
`ProgramLauncher::launch`. Every other reference to an Object stays a `Ptr`: locals, task
targets, the locations dragged by a pointer, prototypes, the results of an owner walk. A `Ptr`
keeps an object alive without becoming its owner, so an object held only by temporaries reports
no owners. A holder becomes an `Owned` link exactly when a query for the object's owners should
find it.

## Locking

Every object carries a one-byte spinlock (`Object::owners_lock`, `SpinLock` in
src/spin_lock.hpp) that guards only its owner list. The lock lives in the padding after
`Object::suspended`, so it costs no memory. A per-object lock was chosen over `engine.mutex` for
three reasons.

- Every query reads one object's owners. The object's cache line is in use anyway, so the lock
  costs one uncontended atomic exchange, and queries on different objects from the worker
  threads, the timer thread and the UI thread never contend on a shared line.
- A spinlock cannot be taken recursively, so no callback can run under it. `engine.mutex` is
  recursive because board mutations happen inside connection callbacks; the owner list must not
  inherit that.
- Each link belongs to the list of its target, so it is guarded by the target's lock alone. No
  operation takes two owner locks. Walking up a chain of owners is done hand over hand: lock the
  child, copy the owners out, unlock, then lock the parent.

`Object::Owners()` copies the owners out as `Ptr`s under the lock and returns them; callers work
on the copy, never inside the lock. An owner is taken with `IncrementOwningRefsNonZero`, the same
operation `WeakPtr::Lock` uses. An owner whose reference count already reached zero is being
destroyed on another thread: its destructor is blocked in the link's unlink on this very lock, its
memory stays valid until the destructor completes, and the walk skips it.

The critical sections consist of pointer writes only. The lock spins a few dozen times and then
yields.

`engine.mutex` keeps guarding `engine.boards` and the order of every board's `locations`. It no
longer answers ownership questions.

## Object destruction

An object is destroyed only after its last `Ptr` is released, and every link holds one, so at
destruction the owner list is empty. `Object::~Object` asserts this.
