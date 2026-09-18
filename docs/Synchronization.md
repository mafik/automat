# Synchronization

Within Automat's (multithreaded) Engine, Objects can connect to Interfaces using any of these modes:

- **Lazy observers** - store a `Linked<T>` (weak pointer which keeps the other end in memory - although allows destruction) and every now and then locking the pointer to check the state of the linked interface. This kind of synchronization resembles a popular "pull" update model.
- **Monitoring observers** - store a weak pointer and periodically poll its `monitor` property. When `monitor` changes - it's a signal that it should be properly locked & checked for updates. This is how UI (Toys) follow object updates (`Poll()` checks monitors & `Tick()` properly locks & checks for updates). This kind of synchronization resembles a "parallel computers" update model.
- **Synchronized observers** - they explicitly register themselves in the other interface which then allows them to be called synchronously whenever an update happens. This kind of synchronization resembles a "push" update model.
- All of these modes allow concurrent destruction of linked interface (while it's not locked). *Is there a kind of synchronization which also OWNS the related object?*

## `Syncable`

The main file which controls how synchronization works is `sync.hpp` and its `Syncable` interface.

Every `Syncable` interface can point to a `Gear` which then points to more synchronized objects.

Note: *Direct synchronization*. It should be possible to synchronize two objects without a `Gear` in between. BUT it's really an optimization that only makes many-to-many synchronization harder. For this reason this design has been abandoned (may be revisited later).

### Hidden Gears (loose idea)

If we assume that Gear is *always* present when a sync relation is active then instead of storing gears as weak ptrs - they could be stored as proper `Ptr`! Gear could still be placed within a `Location` but it could also exist in a *detached* state - where it's not visible but is accessible to objects.

### One-way sync

Syncables may wish to:

- be notified that an event has happened
- notify other Syncables that an event has happened

Currently sync implies both of those, but for some situations it may be good to limit this.

- **Sink** - can be notified of events from other Syncables.
- **Source** - notifies other Syncables that an event has happened.

Within Syncable this information is stored in the last bit of `gear_weak` pointer. If an interface is not marked as a sync *Source* then it may skip the whole synchronization code.

On the Gear side this information is stored in the explicit `sink` boolean - which then allows to skip non-sinks during updates.

TODO: If / when interfaces (objects?) can store reverse-link information then it might be possible to instead set `gear_weak` to nullptr & omit the non-sinks from the Gear's list.

TODO: User Interface for sync directions

### Async execution (early design)

When something happens, we typically (in the code) receive control flow (an attention of a CPU core / a thread of execution).

Sometimes when we receive it, we are also expected to give it back eventually (maybe returning some value).

The default should be simple to understand (everything executes synchronously) but it should also be possible to execute stuff concurrently. Essentially a Gear needs to maintain the "execute in parallel" bit. When it's set, then instead of executing that code immediately, it would schedule it for another thread.

As long as we retain control flow, the chained calls should not grow the stack - tail calls are required and *suspended* must also be disabled before tail-call.

TODO: implement this

### Command - how would it look if it was a "Syncable" interface?

The LongRunning / Runnable interfaces might then not be needed.

#### Goal: no blocking

Worker threads are meant to do actual work. There should be no "park this thread until work is done" in the design. It wastes a CPU thread. Instead the design should allow external wakeup when the work is done.

IF work requires actualy CPU work - then it's perfectly fine to block a worker thread - to do this work.

Otherwise the worker should return to the pool of workers.

#### Goal: blocking work == background work for the user, bot not for workers

Simple commands (executed atomically on the worker thread) and long-running (executed on some background thread / external work) should behave identically from user's perspective. The only difference is that the blocking command may temporarily occupy a worker thread.

Synchronizing blocking & background commands in all combinations should produce the same predictable results.

#### Goal: granular events

Synchronization should be possible at a more granular level (maybe through nested interfaces). Something like "work started" / "work finished" / "work cancelled" could be exposed as more primitive signals that can be hooked together to achieve some specific behaviors.

#### Goal: "Synchronized commands" make sense

The behavior that user probably expects from synchronization is that:
* start of either object starts the other
* cancel on either also cancels the other
* (blocking sync) execution continues only after both synchronized commands finished
* (asynchronous sync) execution continues on either end when that ends is done

This ideal behavior would probably require some dedicated code *somewhere* (where??).

#### Goal: "Synchronized on/off" make sense

The behavior that user probably expects from on/off is that:
* on on either object turns the other also on
* off on either object turns the other also off

#### Goal: "Synchronized Commands-On/Off" make sense

The behavior that user probably expects from on/off - Command sync is that:
* on/start on either object turns the other also on
* off/cancel/finish causes other objects to be turned off/cancelled
* directions (source / sink) may be used to:
  * have on/off control the command, without command affecting the on/off
  * have command control the on/off, without on/off affecting the command

#### Goal: Gear is Interface-agnostic

Gear should stay as interface-agnostic as possible. It may store some extra data but it should not be interface-specific.

## TODO: How would the *Next* relation work? *scary thought because it replaces cables*

* The "begin" & "finish" become events / signals
* "Finish" on one command is connected to a "Begin" on the next one

"Next" becomes a special kind of argument that manages this kind of relation.

Cable is only shown when the Gear is in a "Hidden mode" (no Gear widget on current Board but the other end has a Widget).

This is strictly better than our current cables because:
- the Finish=>Begin connection can be marked as parallel
  - allows early return, background execution & precise threading control
- alternative Begin=>Begin sync can be created to make two objects execute in parallel

