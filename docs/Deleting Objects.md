# Object deletion in Automat

## Memory lifecycle

### Live objects

Objects are tracked through thread-safe Ptr & WeakPtr.

### Object deletion

An Object can be deleted at any point in time. If this happens, `wake_counter`
is incremented, which causes all of the Toys that reference the deleted object
to be woken up.

### Zombie Toys

From the perspective of Toys (living in UI threads), object deletion may only
be observed as nullptr coming from `LockOwner()`. ToyStore periodically scans
all of the WeakPtrs of visible toys & wakes them. When a toy is woken up and
during its `Tick()` detects that its owner is gone, the Toy may choose to
animate its deletion, which should eventually lead to `MarkDead()` call.
This also notifies the toy's parent (through `OnChildDead`) that a toy is
going away.

### Dead Toys

Every toy that can function as a parent needs to be able to handle their 
deletion. Either by reacting to `OnChildDead` notification or by storing its
children in `TrackedPtr`.

(Note that `OnChildDead` callback happens during child's drawing phase)

Cleanup of a dead toy is the responsibility of the `ToyStore` and is based on
the Toy's `dead` flag.

## Visual design

A designated corner is going to be used as a drag target for deleting objects (Vlojure approach)
Optionally also drag a trash icon onto the menu object to delete it
Alternatively a dock object with edit mode or buttons to move & delete objects
Alternatively drop objects outside of the canvas to delete them :P

- excluding the solution with context menu
- excluding the solution with "delete" button on every object

1. Pinning objects to the screen (they don't move with the camera)
2. Trash object that deletes all objects that are dragged over it
3. Trash grabber that can be grabbed to delete objects

4. A new "dock" object that holds the prototypes
  - buttons to delete and move prototypes around
  - "edit" mode when the dock can be altered (iOS approach)

