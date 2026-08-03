# Boards

## Problem

A single board accumulates every object and every connection. Past some number of objects the
connections cross so much that they cannot be followed. One position per object also forces a
single arrangement: a user cannot lay out one view around timing and another around data flow,
because both views would have to be the same view.

Boards solve both problems by letting one Object be owned by several Boards at once. Each
owning board places the object independently, so each board can be arranged around one aspect
of the object's behavior. A connection is drawn only on boards that own both of its endpoint
objects, so each board shows only the connections that belong to its aspect.

## Terminology

- **owning board** — a Board (src/board.hpp) that holds an owning reference to an Object and
  displays it. Several boards can own the same object at the same time.
- **resident widget** — the widget an owning board displays for an object. A board has exactly
  one resident widget per object.
- **pointer-owned** — the state of an object during a drag in which the Pointer
  (src/pointer.hpp) holds the owning reference and the widget.
- **extraction** — removing an object from a board into the pointer.
- **duplication** — picking up a new widget for an object into the pointer while the source
  board keeps its ownership and its resident widget.
- **merge** — the drop of a pointer-owned object onto a board that already owns it.

## Ownership

The list of boards and each board's position on the starfield are stored in `vm.boards` and
`Board::position` (src/vm.hpp, src/board.hpp). `vm.mutex` guards the list and every board's
`locations`, because worker threads look objects up while the UI thread moves them.

A board owns an object through its Location (src/location.hpp), which stores the object's
position and scale on that board. When several boards own one object, each board has its own
Location for it and the Locations share the object. Boards are moved with the Move option in
their menu and never enter drop targets (`MoveBoardAction`, src/board.cpp).

Each board widget has its own ToyStore, which guarantees one widget per object on that board;
`Widget::ToyStore()` resolves to the store of the nearest enclosing board widget
(src/widget.cpp). The pointer needs no uniqueness guarantee, so it holds the dragged widget
directly through a unique_ptr, together with an owning Ptr to the object. ToyStore is one way
of managing widget lifetimes and is used only where the one-widget constraint must be
enforced. Toys that a widget creates for its own subtree — a Wayland window's client surface
toys, for example — are owned by the parent toy directly and never enter a store, so they
move between boards together with the widget tree.

One resident widget per object per board is a deliberate limit:

- The board boundary is what makes one object with several widgets readable. A reader
  attributes repetition across boards to the boards, because each board is a separate view.
  Within one board, two widgets of the same object could not be told apart from two separate
  objects until they changed in lockstep, which would look like an error.
- Connection drawing stays unambiguous. With one resident widget per object, drawing a
  connection on a board produces exactly one geometry. With several widgets per object, every
  connection would need a stored per-widget binding, an interface for retargeting it, and a
  rule for what happens when the bound widget is deleted. Split widgets rely on the same
  guarantee: the covers of a connection are found through its endpoint toys, and "the endpoint
  toy" is well defined only while a board has one toy per object (docs/parrots/Split
  Widgets.md, `Toy::FindWidget` in src/toy.hpp).
- The merge behavior and its feedback (below) need the question "where is this object on this
  board" to have a single answer.

If one board ever needs the same object in two places, the intended primitive is a reference
widget that is visibly marked as a reference, not a second resident widget.

## Dragging

`DragLocationAction` (src/drag_action.hpp) implements all three modes and the transitions
between them. A board-owned move starts with a left drag or the Move option; duplication is
the Clone entry and copying is the Copy entry of the "New..." menu (src/object.cpp).

There are three drag modes:

1. **Board-owned move.** The widget stays owned by the board and follows the drag. It is not
   reparented to the pointer. The object's connections stay drawn.
2. **Extraction.** The board removes the object. The pointer takes the owning reference and
   the widget. The object's connections on that board stop being drawn.
3. **Duplication.** The pointer receives a new widget for the object and an additional owning
   reference. The source board keeps its resident widget and its connections.

The mode changes during the drag:

- A board-owned move that crosses the edge of its board becomes an extraction. The same
  happens when the drag point moves into the black hole or another screen-space overlay.
- A pointer-owned object over a board that does not own it converts immediately into a
  board-owned move on that board: the board takes the ownership and the widget. The conversion
  happens at entry rather than at drop so that the object's connections appear while the drag
  is still in progress. In particular, dragging an object out of a board and back in restores
  its connections at re-entry.
- A pointer-owned object over a board that already owns it stays pointer-owned. Dropping it
  there is a merge (next section).

Entry-time conversion has two consequences. Crossing from a board directly onto another board
transfers the object in one motion, because the exit extracts it into the pointer and the
entry converts it into a board-owned move. Dragging across an intermediate board gives that
board ownership at entry and removes it at exit, so the object's connections on that board
appear and disappear during the pass.

Dropping a pointer-owned object on the background, outside every board, creates a new board
under the dropped widget and transfers the object's ownership and the widget to it.

## Dropping onto an owning board

Duplication starts in this state: the pointer holds the new widget while the source board
under it still owns the object. Extraction reaches this state when some other board also owns
the object.

The drop merges: the dragged Location is destroyed at the drop, but the dragged widget survives
it as a ghost (`LocationWidget::ghost`, src/location.hpp). Its location reference is repointed
at the resident Location, the pointer widget keeps it alive as a zombie, and its snapped pose
springs towards the resident's position while the widget fades out; it dies on arrival. The
target is read from the resident every frame, so the ghost keeps converging even when the
resident moves mid-animation, and several quick drops run several ghosts. No resting widget
changes position.

Two alternatives were rejected. Moving the resident widget to the drop point would move a
widget the user did not pick up and would disturb the arrangement around it. Rejecting the
drop would turn the gesture into an error. A user who drops an object onto a board that
already owns it usually does not know that the board owns it, so the most useful response is
to show where the object already is. When the drop position was intended, one board-owned move
after the merge places the resident widget there.

Feedback: the merge is announced before it happens, by the same mechanism that previews every
other snap. A dragged widget has two poses. The snapped pose springs towards where the Location
would rest — `Location::Position` and `Location::Scale`, written every update from the drop
target's `DropSnap`, or set to the resting copy's position and scale while the drag hovers a
board that owns the object (`DragLocationAction::Update`, src/drag_action.cpp). The drawn pose
is anchored to the stable position — the on-screen point where the pointer holds the toy — so a
held widget never follows a distant snapped pose off the screen, where the renderer would clip
it. `LocationWidget::Tick` (src/location.cpp) animates both; `LocationWidget::Draw` renders the
body at the drawn pose with its material pulled towards the snapped pose (assets/pull_rt.sksl).
A region of roughly one finger around the held point stays rigid, the rest of the body is
displaced with bounded compression, and the r/g/b channels separate slightly along the pull,
more for distant targets. Because the preview reads `Location::Position`, every `DropSnap`
implementation gets it with no code of its own: the board grid, the merge onto an owning board,
and the black hole, which shrinks the held widget in place. It is a separate decoration from the
connection radar, because the radar's only function is to show where a dragged connection would
auto-connect.

The stretch is a displacement of the held widget's own rendering, so it is made of the object
the user is holding rather than of ink invented for the occasion, and it never samples the
target's texture, which may be off-screen. Both ends mark the same material point: the held
point on the drawn body and its image under the snapped pose. There is at most one merge
target: an object rests on at most one board while a copy of it is in hand.

The point where the widget is held is clamped to the nearest point within the toy's coarse
bounds (`RRect::Clamp`, src/math.hpp), so the rigid region around the finger always holds ink.
A fresh clone does not appear at the pointer: the slack between its spawn pose and the stable
position is stored (`LocationWidget::grip_offset`) and decays to zero, so the clone grows out
of its original and glides into the hand.

The mark also identifies the pickup mode: a duplication stretches towards its original from the
first frame, while an extraction has nothing to merge into and rests under the pointer.

A ghost is not held, so its drawn pose is its snapped pose and it flies rigidly, with no
stretch: the merge animation is the plain snap of the same widget, plus a fade.

## Connections

A connection is drawn on a board only when that board owns both of its endpoint objects. Every
board that owns both ends draws the connection, each with its own connection widget from its
own ToyStore. A board that owns only one end draws nothing for the connection, even though the
connection still exists in the VM.
