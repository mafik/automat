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

The list of boards and each board's position on the starfield are stored in `engine.boards` and
`Board::position` (src/engine.hpp, src/board.hpp). `engine.mutex` guards the list and the order of
every board's `locations`. Which boards own an object is answered by the object's owner links
(docs/parrots/Object Ownership.md), so no lookup scans the boards.

A board owns an object through its Location (src/location.hpp), which stores the object's
position and scale on that board. When several boards own one object, each board has its own
Location for it and the Locations share the object. Boards are moved with the Move option in
their menu and never enter drop targets (`MoveBoardAction`, src/board.cpp).

Each board widget has its own ToyScope, which guarantees one widget per object on that board;
`Widget::ToyScope()` resolves to the scope of the nearest enclosing board widget
(src/widget.cpp). The pointer needs no uniqueness guarantee, so it holds the dragged widget
directly through a unique_ptr, together with an owning Ptr to the object. ToyScope is one way
of managing widget lifetimes and is used only where the one-widget constraint must be
enforced. Toys that a widget creates for its own subtree — a Wayland window's client surface
toys, for example — are owned by the parent toy directly and never enter a scope, so they
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

One `DragLocationAction` object lasts for the whole drag. A set board widget marks it as
board-owned; otherwise it is pointer-owned and holds the location widgets itself. Extraction
and entry switch these members in place, so a mode change never touches the pointer slot, the
drag count or the grab anchors. The action never holds an empty
stack: it is constructed with at least one location, and removing the last member
(`DragLocationAction::RemoveFromGroup`) ends the action through `Pointer::ReplaceAction`.
Drags that arrive from outside the process (`DataOffer::BeginDragAction`, src/library_data_offer.cpp)
are ordinary pointer actions in the mouse pointer's left-button slot and end with a
synthesized button release.

## Dropping onto an owning board

Duplication starts in this state: the pointer holds the new widget while the source board
under it still owns the object. Extraction reaches this state when some other board also owns
the object.

The drop merges: the dragged Location is destroyed at the drop, but the dragged widget survives
it as a ghost (`LocationWidget::merging`, src/location.hpp). Its location reference is repointed
at the resident Location, the pointer widget keeps it alive as a zombie, and its snapped pose
eases towards the resident's position while the widget fades out; it dies on arrival. The
ghost passes no pointer events to its toy (`LocationWidget::AllowChildPointerEvents`), so a
click during the flight reaches the widgets under it. The target is read from the resident
every frame, so the ghost keeps converging even when the resident moves mid-animation, and
several quick drops run several ghosts. No resting widget changes position.

Two alternatives were rejected. Moving the resident widget to the drop point would move a
widget the user did not pick up and would disturb the arrangement around it. Rejecting the
drop would turn the gesture into an error. A user who drops an object onto a board that
already owns it usually does not know that the board owns it, so the most useful response is
to show where the object already is. When the drop position was intended, one board-owned move
after the merge places the resident widget there.

Feedback: the merge is announced before it happens, by the same mechanism that previews every
other snap. The widget's body is drawn at the snapped pose, which springs towards where the
Location would rest — `Location::Position` and `Location::Scale`, written every update from the
drop target's `DropSnap`, or set to the resting copy's position and scale while the drag hovers
a board that owns the object (`DragLocationAction::Update`, src/drag_action.cpp). The stretch
towards the hand is not drawn by the location at all: the location only places a texture anchor
(`LocationWidget::AnchorToPointer`, src/location.cpp) — the grab point, bound to the pointer —
and the `WARP` compositor deforms the rendered texture so the body follows the anchor
(assets/warp.sksl, `WidgetDrawable::onDraw` in src/renderer.cpp): an anchor's influence
reach grows with its displacement, so the texture around the grab is displaced to the hand
while a tapered tail stays stretched towards the snapped position. The stretch exists only while the
widget's local coordinate space holds the texture against the anchors —
`Widget::local_to_parent_weight`, the shader's `weightBias`.
`DragLocationAction::Update` aims that weight at 1 while a drop target imposes a pose and at 0
while the drag is over no target, where the lone grab anchor displaces the texture rigidly;
the location eases the weight between the two, so leaving or entering a board is a fade rather
than a mode switch. An anchor holds the texture rigidly within its grip `radius`; beyond that
its pull falls off with distance, dropping to half at `decay` beyond the grip edge
(`Widget::TextureAnchor`, src/widget.hpp; assets/warp_field.sksl). A merge drop writes the
displacement the pointer was producing into the anchor (`Widget::TextureAnchor::warp_by`) and
springs that displacement to zero, releasing the stretch. The
displacement is stored in the anchor rather than derived from the position recorded in the
rendered texture, so a widget that re-renders during the flight keeps it.
Because the pointer-bound anchor is
resolved against the live pointer at composite time, the body follows the hand even while the
widget's animation sleeps. Because
the snapped pose reads `Location::Position`, every `DropSnap` implementation gets the preview
with no code of its own: the board grid, the merge onto an owning board, and the black hole,
which shrinks the held widget in place. It is a separate decoration from the connection radar,
because the radar's only function is to show where a dragged connection would auto-connect.

The stretch is a deformation of the held widget's own texture, so it is made of the object the
user is holding rather than of ink invented for the occasion, and it never samples the target's
texture. There is at most one merge target: an object rests on at most one board while a copy
of it is in hand.

The grab point is a position in the coordinates of the base toy, the toy of the bottom location
of the dragged stack. By default `DragLocationAction` reads it at pick-up from the pointer
position, clamped to the nearest point within the toy's coarse bounds (`RRect::Clamp`,
src/math.hpp), so the stretched texture is always ink. A caller that wants the object held by
another point passes the grab point to the constructor instead. The snapped pose places the grab
point under the pointer, and the anchor pulls the grab point to the pointer with no offset, so
both follow from the same point and a converged pose shows no stretch. Neither is derived from
the widget's current transform, which is transient at pick-up: an anchor offset computed from
it would remain as a permanent stretch after the pose converged. A fresh clone's body sticks to
the hand from the first frame, trailing its tail back to the original it was pulled out of.

The mark also identifies the pickup mode: a duplication stretches towards its original from the
first frame, while an extraction has nothing to merge into and rests under the pointer.

A ghost is not held, so its drawn pose is its snapped pose and it flies rigidly, with no
stretch: the merge animation is the plain snap of the same widget, plus a fade.

## Connections

A connection is drawn on a board only when that board owns both of its endpoint objects. Every
board that owns both ends draws the connection, each with its own connection widget from its
own ToyScope. A board that owns only one end draws nothing for the connection, even though the
connection still exists in the Engine.

## Wallpaper

A board may show the image of a `File` (src/library_file.hpp) in place of the default board
texture. The Board owns that File through `Board::wallpaper`, an `Owned<library::File>` link
(docs/parrots/Object Ownership.md) guarded by `Board::wallpaper_mutex`. The lock order is
`engine.mutex` before `wallpaper_mutex`, and nothing takes `engine.mutex` while holding
`wallpaper_mutex`; `Board::SetWallpaper` (src/board.cpp) therefore extracts the File's Location
from the board before it takes the mutex. The File leaves the board when it becomes the
wallpaper: on one board the same object is either placed or drawn as the background, never
both, so pulling the wallpaper back out (below) does not duplicate it. A File may be the
wallpaper of several boards, and the wallpaper of one board while placed on another.

Three paths lead to `Board::SetWallpaper`: the "Set Wallpaper" option, the drop of exactly one
image File onto the starfield (`DragLocationAction::Drop`, src/drag_action.cpp, after the new
board has been entered, so the File's toy fades out in place), and the `wallpaper` key of a
saved board. Several objects dropped at once, or a File that is not an image, are placed as
objects.

"Set Wallpaper" is `kSetWallpaper` (src/board.cpp), an option table with only an activation,
built like `kRun` (docs/parrots/Options.md). `FileToy::FillMenu` (src/library_file.cpp) places
it at E when the toy shows an image and has a `BoardWidget` ancestor, and the activation acts on
that board, found through the toy it was activated from. The File class declares nothing for
it: no interface and no table-of-contents slot. A File dragged by the pointer has no board above
its toy and shows no such option.

"Wallpaper" is `Board::take_wallpaper` (src/board.hpp), an `ObjectSource` placed at W of the
board menu while a wallpaper is set. Its icon is the File's own toy, so the slot shows the
picture. Taking it releases the link, restores the default texture and hands the File to
`DragNew`, so the File is dragged like any object and may be dropped on a board or on the
starfield.

### Animation

The pose in which a File's toy coincides with the wallpaper is `WallpaperTransform`
(src/board.cpp): the toy centred on the board and scaled so that its shorter side spans the 1 m
board, which is the placement `BoardWidget::Tick` gives the image shader, expressed for the toy.
The toy shows the whole image while the wallpaper shows the centred square crop, so for a
non-square image the cropped parts lie outside the board whenever the toy is in that pose.

Setting a wallpaper is not animated. `Board::SetWallpaper` extracts the File's Location, which
destroys it, and the board draws the new wallpaper from the next frame. The resident widget
finds its Location dead, fades out in place and dies (`LocationWidget::Tick`). The wallpaper
appears at once while the toy fades, so a flight of the toy into the wallpaper pose would start
after the picture it lands on is already there. A location widget whose Location is dead passes
no pointer events to its toy (`LocationWidget::AllowChildPointerEvents`), so a click on the
fading toy opens the board menu and not the menu of the File.

Taking a wallpaper grabs the toy out of the board. `Board::take_wallpaper` makes the new toy a
child of the board widget with `WallpaperTransform` as its transform before handing it to
`DragNew`, so the drag's pose animation starts from where the wallpaper is drawn and the image
shrinks into the hand. The default grab point, the image point under the pointer, stays under
the pointer throughout, so the shrink is centred on the hand.

While the toy is larger than the board, the parts of the image that the wallpaper cropped would
show past the board's edges. `Board::take_wallpaper` puts the new location widget into the
`Baked` range of the board's layer stack once, after the drag has entered the board, and
`BoardWidget::Draw` clips its baked children to the board, so the toy is drawn within the board
and under the board's other objects, where the wallpaper was. `BoardWidget::Tick` orders every
location above the board except a dragged one whose toy does not fit within the board, which it
leaves where it is; once the toy fits, the ordering places the widget at the top of the stack.
Leaving the board with the pointer extracts the location, which removes its widget from the
board's stack.

`LocationWidget::Tick` (src/location.cpp) animates a toy's scale with a damped spring in the
logarithm of the scale and maps the result back through the exponential. The spring overshoots
in that domain, and every value of the domain, negative ones included, maps to a positive scale,
so a shrink undershoots to a fraction of its final size and rebounds: for the twentyfold ratio
of the wallpaper pose the fraction is 0.69, for the 1.2 ratio of a toy's appearance it is 0.98.
The half-time of the spring (`LocationWidget::kSpringHalfTime`, src/location.hpp) sets the
rebound to about 12 percent for the ratio 2.5 of a toolbar pick-up and 19 percent for the ratio
4 of a deiconified toy. A spring on the scale itself passes through zero and negative scales
for large ratios. The position keeps its cosine ease.

`BoardWidget::Tick` (src/board.cpp) reads the File's lazy image (`File::Image()`) and builds the
inputs of `assets/bg.sksl` from it: the image shader, scaled uniformly so that the shorter side
spans the 1 m board and the longer side is cropped symmetrically (pixels stay square); a 64 × 64
thumbnail shader for the base tone of the halftone; and `background_px`, the image's pixels per
metre on the board, which the shader uses both as the halftone threshold and as the dot pitch,
as it uses 3840 for the default texture. The widget rebuilds the inputs only when the image
changes.

Automat holds no decoded pixels. Graphite decodes the lazy image the first time a frame draws
it, inside `AutomatImageProvider::findOrCreate` (src/textures.cpp) on a Vk recorder thread, and
keeps only the GPU texture, in the provider's cache keyed by the image's `uniqueID`. The File's
toy draws the same `SkImage`, so a File that was on screen before it became a wallpaper is not
decoded again. The thumbnail is a picture-backed deferred image (`SkImages::DeferredFromPicture`)
of the cropped square: Graphite renders it on the GPU from the cached texture the first time the
shader samples it, replaying the picture inside the outer `findOrCreate`, which calls
`findOrCreate` again for the image inside the picture; `AutomatImageProvider::mutex` is recursive
for that reason. The default texture's thumbnail is made the same way.

A wallpaper File without an image (its file was deleted or replaced by a non-image) is not drawn:
the board shows the default texture, and the "Wallpaper" option still offers the File.

Saving writes the File's name under the board's `wallpaper` key; the File is saved like any
other object. At shutdown `automat::Main` (src/automat.cpp) suspends the wallpaper File together
with the objects on the boards, because `File::~File` deletes the disk file of a File that is
not suspended.

Not covered: a Data Offer transfer that is still running when the card is dropped on the
starfield (the File is placed on the new board when it arrives, docs/parrots/Data Offer.md);
image data that fails to decode after a valid header (Skia draws nothing for it); transparency (the shader uses the decoded colour).
