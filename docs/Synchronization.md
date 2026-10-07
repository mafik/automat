# Synchronization

The point of synchronization is to "fuse" some aspect of two objects - to make them act as one. Whenever something happens to one object - it would be also applied to all the others.

## Interaction models

**Direct.** Within Automat's (multithreaded) Engine, Objects generally refer to one another using `Linked<T>` (weak pointer which keeps the other end in memory - although allows destruction). The main example is the "next cable" which passes control flow to the next object. Under this model, all interactions require some object to wake up and explicitly push or pull some data.

**Monitoring.** Another mechanism for fetching data relies on storing a weak pointer (just like above) and periodically polling its `monitor` property. When `monitor` changes - it's a signal that it should be properly locked & checked for updates. This is how UI (Toys) follow object updates (`Poll()` checks monitors & `Tick()` properly locks & checks for updates). This kind of synchronization resembles a "parallel computers" update model - all machines must be "alive": to periodically bump their monitors & check the monitors of others.

**Hooking** allows one Object to modify the ToC (the list of supported interfaces) of another object. It may add & replace arbitrary interfaces (as long as they stay compatible). This can be used to arbitrarily modify the behavior of another object - possibly even replacing some of its functions with Automat-hosted implementations that use machine-code!

The main problem with Synchronization is that it might be applied at many levels within object logic, so *Direct* approach would result with lots of unused "hook pointers". *Monitoring* approach would require constant polling (acceptable for the UI - but not so much for headless operation). That leaves *hooking* as the main mechanism of synchronization.

When some interface is being synchronized, then that interface (and/or its linked-interfaces) are replaced with stubs that propagate the events to other linked objects.

## Gear

Every synchronized interface adds a `Ptr<Gear>` argument to the hooked Object. `Gear` serves as a concurrency synchronization point and as a storage for all the other linked members. This kind of argument differs from most others - which store `Linked<T>`. The assumption is that synchronization will become very commonplace and leaving gears all over the place would lead to visual mess. `Ptr` allows Gear to exist in a *detached* state - where it's not visible but is still accessible to objects.

**Note: Direct synchronization.** It should be possible to synchronize two objects without a `Gear` in between. BUT it's really an optimization that only makes many-to-many synchronization harder. For this reason this design has been abandoned (may be revisited later).

## Notifications & suspension

Objects may control the direction of the synchronization by setting their `suspended` bool. That bool is a part of every Object! The idea behind it is that when an object is woken up, it should raise its `suspended` bit. Failure to do so (`suspended` was already true) means that the Object has already done its work and should not do it again.

`suspended` allows objects to invoke their own methods as notifications. `suspension` is the mechanism that *Object's implementations* use to prevent backflow of signals. The user-visible mechanism is the *sources & sinks*:

## Sources & sinks

For some situations it may be good to be able to control how a particular Interface is synchronized:

- **Sink** - can be notified of events from other Interfaces.
- **Source** - notifies other Interfaces that an event has happened.

On the Gear side this information is stored in the explicit `sink` boolean - which then allows to skip non-sinks during updates.

TODO: User Interface for sync directions

## Concurrency

Interfaces are allowed to block the worker threads for arbitrary periods of time. Because of this synchronization requires all of the synchronized interfaces to be executed in parallel.

**Not single-threaded.** In some cases serial execution might be better (for example synchronization of very fast, tiny actions) - but for those there might be better, dedicated primitives. Synchronization will take care of the higher-level stuff.

The completion of synchronized actions must also be synchronized! To facilitate this, Gear must count & delay the execution of the 'next' argument of synchronized actions. It must also properly handle cancellation.

Cancellation of one of the synchronized interfaces should probably also cancel the other.

## Goal: Gear is Interface-agnostic

Gear should stay as interface-agnostic as possible. It may store some extra data but it should not be interface-specific.

## On/Off synchronization

There are some Objects in Automat that offer various On/Off-type interfaces. Those interfaces might be synchronized with other On/Off interfaces and should then work as if their `on` & `off` methods were synchronized.

On/Off itself is a combination of three different commands: `on`, `off` & `toggle`. This makes simple 1:1 synchronization with Commands difficult. To allow it, the Switch object (which may function as an "On/Off controller") is going to expose "command adapters":

* **Start/Stop adapter**: Command execution is synchronized with On/Off's "on" command BUT once all of the commands complete, the "off" is executed (as if it was the "done" signal). Explicit "off" may be used to cancel execution.
* **Wait adapter**: Command execution is synchronized with On/Off's "on+off". On runs immediately, and other Commands wait for the "off" to be executed. Explicit "off" is not cancellation - it only allows the control to proceed. Wait adapters may be useful in extending the runtime of other Commands.

These command adapters will only be accessible through the synchronization mechanism.

## Menu structure

Menu is structured to present the most important options first (start/stop sync, wait sync) while hiding confusing options like "toggle sync" deeper.

### Every On/Off interface

(ctrl + pull creates a Switch controller)

1. On => Command interface sub-menu (Run/Sync/Controller options)
2. Off => Command interface sub-menu (Run/Sync/Controller options)
3. Toggle => Command interface sub-menu (Run/Sync/Controller options)
4. Controller => Creates a Switch controller for this interface

### Switch's top-level menu

1. State => On/Off interface sub-menu (On/Off/Toggle commands)
2. "Start/Stop (Sync)" => synchronizes the "Stop" command adapter within Switch
3. "Wait (Sync)"=> synchronizes the "Wait" command adapter within Switch
