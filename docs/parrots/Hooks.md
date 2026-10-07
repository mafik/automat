# Hooks

## Purpose

A hook changes the interfaces that an Object offers without changing the class of the Object. It
replaces entries of the Object's table of contents (`ToC`, src/interface.hpp) with its own tables
and appends entries after the existing ones. A replacement table is a copy of the table it replaces
with some fields changed, for example a Command table whose `on_run` does additional work and then
runs the table it replaced. The only hook is the Sync hook (`Sync::Table`, src/sync.hpp), which
makes the Commands of several Objects act as one through a Gear.

## The hook stack

`ToC` holds a `std::span` of table pointers and is indexed from 1. `StaticToC` stores the tables of
a class in static memory, and `toc_of<...>` is the variable that `INTERFACES()` names. An Object
whose tables change at run time (File, MediaFile, GStreamerElement, Instruction, the Timeline tracks)
keeps a `HeapToC` member instead, which stores the tables in a `std::vector`.

`Object::toc` is an atomic pointer to the ToC that readers use. A hook is itself a ToC: `Hook`
(src/object.hpp) derives from `ToC`, stores its tables in `Hook::buffer` and points with
`Hook::tail` to the ToC below it. The hooks of an Object therefore form a stack whose top is
`Object::toc` and whose bottom is the base ToC. `Hook::Attach` copies the array of the current ToC
into `buffer`, replaces or appends entries, sets `tail` and publishes the hook with a
compare-exchange on `Object::toc`. `Hook::Detach` is only called on the top hook; it restores
`tail` as the object's ToC and returns the interface index that the hook was attached to, so the
same hook can be attached again. A hook copies the ToC below it, so it contains the edits of every
hook below it, and it keeps every index of the ToC below: `turn_on_idx`, `next_idx` and the indices
stored in `Linked<T>` keep their meaning, and appended entries get new indices after the existing
ones.

`Object::hooks_lock` serializes the changes of one stack. `Object::hook_count` counts the attached
hooks, because the destructor of an Object cannot inspect the base ToC: a base ToC that is a member
of the derived class is already destroyed when `~Object` runs.

## Removal and re-attachment

A hook in the middle of the stack cannot be taken out alone, because the hooks above it copied its
entries. `Object::RemoveHook` detaches the hooks above the target, detaches the target, attaches the
others again with their returned indices, lowest first, and retires the target. The same order of
operations serves `Object::ReattachHooks`: an Object with a run-time ToC edits an entry of its base
ToC (the image slot of a File, the streams of a MediaFile, `Instruction::UpdateToC`) and then
attaches its hooks again, so that their copies contain the edit.

## The base ToC

`Object::BaseToc` returns the ToC below all hooks. `INTERFACES()` defines it for classes with a
static ToC, and classes with a run-time ToC override it to return their `HeapToC`. A hook never
uses `BaseToc`: it reaches the entry below itself through `tail`, so every hook below it keeps
working. `BaseToc` is where a caller finds the original version of an interface, without the
changes of any hook. `Object::InterfaceIdx` searches only the current ToC. A table that a hook has
replaced has no index any more, so a caller that attaches a hook keeps the `Sync::Table` instead of
the binding it attached it with (`Gear::FullSync` in src/sync.cpp and the hook loading in
src/persistence.cpp).

A copy starts from `other.BaseToc()` (`Object(const Object&)`), because hooks belong to the Object
they were attached to. An implicitly defined copy constructor copies the `SetToc` member of
`INTERFACES()` instead of running its initializer, so the base constructor sets the ToC. Classes with
a run-time ToC point `toc` at their own `HeapToC` in their constructors, including the copy
constructors. `~Object` detaches and deletes the hooks that are still attached.

## Reclamation

A reader can still hold a table or the array of a hook after the hook was detached. A retired hook
is therefore not deleted at once: `RemoveHook` pushes it onto `Object::garbage`, a list linked
through `Hook::tail`, and `Hook::~Hook` deletes the rest of the list. `Object::toc_refs` counts the
readers: every `Locked<T>` counts for its lifetime, in addition to the owning reference it holds,
and `Object::TocGuard` counts for a scope (`Object::Interfaces`, `Linked<T>::Lock`, the menus and
belts that compare table pointers). The reader that brings the count to zero deletes the garbage
list (`Object::DecrementTocRefs`).

A hook that is re-attached rebuilds its `buffer` and its replacement tables in place, while another
thread may still read the old contents. This race is known and not handled yet.

## Calling through a hook

`Command::Table::on_run` receives two arguments. `self` is the interface used to invoke the
Command: the Object and the table that the caller addressed, which is the table of the top hook
when the caller looked the Command up in the current ToC. `true_self` is the table whose `on_run`
is running. `Command::Run` passes `*table` as both. A hook finds its own data from the address of
`true_self`, because its replacement tables are members of the hook, and it calls the entry below
with the same `self` (`below.on_run(self, below)`). A base Command reads the data that hooks
provide, such as `next_idx`, from `self.table`; a base Command that stores its own data in a
subclass of its table reads it from `true_self` (the register filters of the instruction library,
the property Commands of GStreamer and GEGL, the digit Commands of Number). Passing the ToC instead
of the table would not work, because the ToC of an upper hook also holds copied pointers to the
tables of lower hooks, so a ToC does not tell which hook owns a table. `fill_menu` receives the
same `true_self` for the same reason.

`Linked<T>` (src/interface.hpp) stores an interface index instead of a table pointer and resolves it
through the current ToC when it is locked, so a cable, a button or a rocker reaches the tables of
the hooks and never holds a pointer into a retired hook. `Interface::DefBase::Bind`, which binds a
`Def` member of an Object, takes the index of the member's table from the constexpr `SlotOf` of the
Object type at compile time and returns the table at that index in the current ToC, so `run->Done()`
on a synced Command reaches the replacement table of its hook. `INTERFACES` generates that `SlotOf`.
An Object with a `HeapToC` whose `Def` members are bound defines it by hand over the fixed slots of
those members (`GStreamerElement`, `Instruction`).

## Sync

### Sync::Table

`Sync::Table` is at once three things: the table of the Sync argument, which the hook appends to the
ToC and which is drawn as the belt; the hook; and the member of a Gear. There is one `Sync::Table`
per synced interface. Its fields name the synced interface (`attached_toc_idx`), its Object
(`object`), its Gear (`gear`, null while it is not connected), its neighbours in the ring of the Gear
(`prev`, `next`), the directions (`sink`, `source`) and the scroll counter of its belt
(`sync_balance`). `Sync::Table` is abstract and has one subclass per kind of synced interface. The
replacement tables are members of the subclasses, so a `Sync::Table` is the memory behind the table
pointers it puts into the ToC. `FindOrMakeSyncTable` (src/sync.cpp) creates the subclass that
matches the kind of the table at the synced index.

`Sync::CommandTable` hooks a Command. It replaces the Command (`cmd_wrapper`, which forwards the
start) and the Command at its `cancel_idx` (`cancel_wrapper`, which forwards the end). The
`next_idx` of `cmd_wrapper` points at an appended Argument of the invisible style
(`completion_arg`), whose `find` returns an appended Command (`completion_detector`).
`Sync::OnOffTable` hooks an OnOff. It replaces the OnOff (`wrapper`, for the menu only) and the
Commands at `turn_on_idx` (`on_wrapper`) and `turn_off_idx` (`off_wrapper`); it has no completion
tables. Every member appends the Sync argument itself.

What a partner runs is decided by the subclass through the virtual `Start` and `End` of
`Sync::Table`. `Start` of a `CommandTable` runs the Command below `cmd_wrapper` and `End` runs the
Command below `cancel_wrapper`; `Start` of an `OnOffTable` runs the Command below `on_wrapper` and
`End` runs the Command below `off_wrapper`. `SyncableOf` is also virtual: only an `OnOffTable` names
a `Syncable`, through the `syncable_idx` of its `wrapper`.

### Forwarding

A replacement table calls `Start` or `End` of every other member and then runs the entry below it,
with the same `self`. The members are visited in the order of the ring, starting after the calling
member, so the caller's own Command runs last. Each partner is reached through its own `tail` and
called with its own replacement table as `self` (`CallBelow` in src/sync.cpp), so the data of the
partner's hook, such as its completion, applies to it. Start is the run of a Command or the on of an
OnOff; end is the cancel of a Command, the completion of a Command or the off of an OnOff. A partner
Command receives its run or its cancel, and a partner OnOff receives its on or its off. The next
Command that a partner returns is scheduled on a worker thread.

A member forwards only when it is a source, and only to members that are sinks. The Gear is
suspended while a replacement table forwards (`Suspend`, src/object.hpp), and a replacement table of
a suspended Gear only calls the entry below, so a Command that is run by a forward does not forward
again through the same Gear. During loading every Object is suspended, so nothing forwards. The flag
is a plain `bool`: two members of one Gear that start at the same moment on different threads may
race, and one of them may pass through without forwarding.

The partners are copied out of the ring under a shared lock of `Gear::mutex` as `Locked<Sync>`
values, which keep the partner Objects and their hooks alive, and they are called after the lock is
released.

### Notifications

An Object that changes its state on its own, such as a KeyPresser that sees its key pressed or a
Window that gains the focus, calls `OnOff::NotifyTurnedOn` or `OnOff::NotifyTurnedOff`. These do
nothing when the Object is already suspended; otherwise they suspend the Object and call `TurnOn` or
`TurnOff`, which reach the replacement tables through the current ToC. The replacement tables
forward to the partners as usual, and the Object's own Command, which runs last, must stay inactive
because the state has already changed. Every class whose own on or off Command would act again
checks `suspended` itself (the Window's activate and deactivate, the KeyPresser's press and release,
the Assembler's cancel); a generic check is not possible, because a HotKey turns itself on during
loading, while every Object is suspended. An OnOff whose running state ends on its own (Timer,
Timeline, Program Launcher, the GStreamer elements, the Assembler) calls `NotifyTurnedOff` when it
ends, because its on Command is not the same as its on state.

### Completion

A Command member reaches its completion through its `next_idx`. A synchronous Command returns
`NextCommand()` from the `on_run` that `Command::Table::FillFrom` generates, and `self.table` is the
replacement table of its hook, so the next Command is `completion_detector`. A long-running Command
calls `Done()` through its `Def` member (`run->Done()`), which binds the table at the member's index
in the current ToC, for the same reason. `completion_detector` cancels the other Commands of the
Gear, turns off its other OnOffs and then returns the next Command of the entry below, reached
through `tail`. `completion_detector`
runs after the replacement table returned, when the Gear is no longer suspended, so a completion
always forwards: an instant Command synced with the run of a Timer cancels the Timer right after it
starts, whichever of the two is started first.

### Gear

A Gear (src/sync.hpp) holds the ring of its members (`Gear::members`) under `Gear::mutex`. The
members hold the Gear through `Ptr<Gear>`, so a Gear that is removed from every board keeps working
as a hidden Gear, and members stop syncing only when they are disconnected. `Gear::AddSink`,
`Gear::AddSource` and `Gear::FullSync` take the synced interface, create its `Sync::Table` or reuse
the existing one, and set the directions; `RootWidget::control_gear` uses them to connect the left
Control key of a KeyPresser as a source to the `control` Switch as a sink.

### Syncable

An OnOff can name a `Syncable` interface of its Object through `OnOff::Table::syncable_idx`
(src/syncable.hpp). Its `on_sync` runs when a `Sync::Table` of that OnOff starts being a source in a
Gear, and its `on_unsync` runs when it stops. A `Window` starts and stops watching the focus of the
operating system, and a `KeyPresser` starts and stops monitoring its key. The destruction of an
Object sends no notification, because the Object is already partly destroyed; `Window` releases its
watch in its own destructor.

### The Sync option

The Command menu places the global `kSync` option at E when the Command is in the ToC of its
Object (docs/parrots/Options.md). `kSync` starts a `SyncAction` for its caller. The replacement table
of a synced interface replaces `fill_menu`: it fills the menu of the entry below and then places the
Sync argument of its own `Sync::Table` at E, so Sync on a synced interface reuses that `Sync::Table`.
There is no Unsync option; dragging a belt away disconnects it.

### The drag

`SyncAction` (src/sync.cpp) owns the hook during a drag. It holds the Sync argument as a
`Locked<Sync>`, which keeps the Object alive and counts as a reader of its ToC for the whole drag,
so the hook cannot be freed under the action. Given an interface that is not a Sync argument, it creates and attaches a `Sync::Table` without a Gear; given a Sync argument, from a click
on the pinion of a belt or from the reused option, it takes that `Sync::Table`, which leaves its Gear
when it is picked up. When the drag ends, `BoardWidget::ConnectAtPoint` connects the argument at the
pinion, and `on_connect` (`Sync::Table::DefaultOnConnect`) handles the drop: an interface that has a
`Sync::Table` in a Gear makes the member join that Gear; a Gear makes it join the Gear; another
Command gets a `Sync::Table` of its own, and a new Gear is inserted between the two Objects on the
board of the dragged Object (`Location::PlaceBetween`). A drop that connects nothing detaches the
hook and retires it.

### Belts

A belt (`SyncBelt`, src/sync.hpp) is the toy of a Sync argument. It is keyed by the `Sync::Table`,
which may be retired and freed while the toy exists, so the belt only compares its table pointer
with the entries of the current ToC under a `TocGuard` before it reads the table, and it retracts and
marks itself dead when the table is gone. The toy of the Gear (`GearWidget`) turns when a forward
wakes it, and the texture of every belt of the Gear scrolls with it.

### Saved state

`ObjectSerializer::Serialize` (src/object.cpp) saves the links of an Object from its base ToC, so no
appended entry of a hook is saved as a link. It writes the members of an Object under `"hooks"` as
`{"gear", "interface", "sink", "source"}`. The Gear is named through `ResolveName`, so it is saved as
an Object of its own, also when it is hidden, and it saves no member list. A `Sync::Table` without a
Gear is not saved. `LoadState` (src/persistence.cpp) creates every Object in its first pass and
attaches the members through `AddSink` and `AddSource` while it reads the `"hooks"` key, which is
written after the other keys of the Object.
