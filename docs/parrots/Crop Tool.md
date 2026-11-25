# Crop Tool

The Crop Tool shows a part of another object's toy: the object `CropView`, its argument `scissors`
(the Crop Scissors), the interface `Croppable` and the gizmos `CropBox` and `CropHandle`
(src/crop_tool.hpp, src/crop_tool.cpp). `library::File`, `library::MemMeld` and the Crop View
itself are Croppable, so Crop Views can form a chain.

## The tool for the user

A Crop View is offered in the menu of the Beta flask (`docs/parrots/Beta Brand.md`); its random hue
colors its scissors, fabric and gizmos as one tool. Grabbed scissors leave their home, connect over
a Croppable toy (on a spear: over its shaft), open and split, and a Crop Box with handles unfolds
around the touch point. Scissors dragged off the toy return home while the box folds where it was.
A drag of the scissors on one board moves the box of the same Crop View on another board.

## VM world and UI world

The nine entries of `Croppable::Table` are divided into a VM world (`crop_view_serialize`,
`crop_view_deserialize`, `crop_view_make_parameters`), which works on objects without widgets (a
headless Automat, a load, a save), and a UI world (the six other entries), which needs toys. The
crop parameters (`CropView::parameters`) are the only state of a crop. A window sees a gesture made
in another window only as a change of that state, so every UI entry derives what it shows from the
parameters and its own toys. The parameters are written by pointer actions and by
`crop_view_deserialize` under `CropView::mutex`, followed by `WakeToys()`, and never by a `Tick`,
because a `Tick` that bumps the monitor wakes itself again and the tool would never become idle.

## The entries of Croppable

`Croppable::Def` fills each entry from the implementer method of the same name in CamelCase when
the implementer has one; `CropView::croppable` has none, because the entries are always called on
the end of the chain, which is never a Crop View. A Croppable never moves or wakes a gizmo.

- `crop_view_make_parameters` allocates a buffer with default parameters (the whole toy). It is
  called with no lock held, by the cascade that sets the chain end and by `DeserializeKey`.
- `crop_view_serialize` (from `SerializeState`) writes the buffer as one value, or the default
  value when the chain end is another object. `crop_view_deserialize` (from `DeserializeKey`, after
  it assigned the end and a new buffer) reads one value into the buffer and reports errors.
- `crop_scissors_open` (from `DragScissorsAction`, right after `Connect`) gets the toy of the end
  under the scissors (`croppable_toy`) and the pointer in its coordinates and writes the first
  target value.
- `crop_scissors_move` (from `DragScissorsAction`, while connected) gets the changed touch point,
  the center of the box, in the coordinates of `croppable_toy`. It writes the target value and
  assigns the seen target of the group of its own scissors.
- `crop_scissors_tick` (from `CropScissors::Tick`, while the chain end has a toy under the
  scissors) reads the target value, finds or makes its group and gizmos, animates the apparent
  value, assigns `CropScissors::box` and writes the fields of the gizmos (`CropBox::size`,
  `CropHandle::origin`, `scale`, `option`). Without a box its group folds.
- `crop_scissors_close` (from `CropScissors::Tick`, for every other group whose Croppable exists)
  gets no Crop View and no toy, because the scissors may lie on no toy of that object. It folds the
  group, places its gizmos from `Group::extent` and clears the `option` of the handles.
- `crop_make_toy` (from the widget of the Crop View) returns a new toy of the object.
- `crop_tick_toy` (from every `Tick` of that widget with a chain end) composes the parameters of
  the chain into the cropped toy and returns a shaping `Tock` only when the crop of the toy
  changed, so the toy can tell a change of the crop from a change of its contents.

## The parameter buffer

`CropViewParameters` is an empty struct with a virtual destructor; each Croppable derives its own
struct (`File::CropParameters`, `MemMeld::CropParameters`, declared in its header and defined in
its `.cpp` file). `CropView::chain_end`, the `Linked<Croppable>` at the end of the chain, is
guarded by `CropView::mutex` together with `parameters`. The invariant is that `parameters` was
made by `crop_view_make_parameters` of the Croppable in `chain_end`, or both are empty, so a
Croppable reads the buffer with a `static_cast` to its own struct. Another window or a load can
change the end between the read of `chain_end` and the call, so every method tests
`view.chain_end.object_ptr == &*obj` under the mutex before the cast, and a handle drag tests
`chain_end` against the Croppable that it took at the press.

## The chain end and chains

`Croppable::State` holds a `std::mutex` and `views`, weak pointers to the Crop Views connected to
the Croppable. Only `CropView::scissors_Impl::OnConnect` keeps the list. For a changed target it
moves the view from the list of the old target to the list of the new one (erasing expired
entries), sets an empty chain end when there was a target, and then the chain end of the new
target: the target itself, or the `chain_end` of a target Crop View. `SetChainEnd` changes nothing
for an equal value; otherwise it makes a buffer of the new end, assigns both members, calls
`WakeToys()` and sets the same end in every view of its own `croppable.views`, recursively. A first
connection to a target with the same end, or to a Crop View without a chain end, therefore keeps
the buffer that a load assigned. `CropView::~CropView` sets an empty chain end in every view of its
list, because their `scissors.target` expires without `OnConnect`; it reads the list without the
mutex, because no other reference to the object exists then. Each step holds one mutex at a time,
and none while a buffer is made or the cascade recurses. Because the cascade makes the buffers of
the views that crop a view again, an inner view loses its crop when the scissors of the outer view
leave or change their Croppable, and shows the whole outer crop after the outer view connects again.

`OnCanConnect` accepts only a `Croppable` and refuses a chain that leads back to the view itself. A
Crop View in the middle of a chain forwards nothing. The parameters of each view are relative to
what the toy under its scissors shows, which for a view that crops a view is that view's crop;
`crop_tick_toy` walks from the given view along `scissors` and composes the parameters of every
view whose chain end is this Croppable. `CropScissors::OnPoll` and `CropViewWidget::OnPoll` sum the
monitors and addresses of the objects along the chain (`SumOfTargetMonitors`) and wake their toy
when the sum changes, because the crop depends on every view up to the end.

## Target value and apparent value

The target value is `CropView::parameters`. The apparent value is what the gizmos of one scissors
toy show, kept in a `CropScissors::Group`. `Group::center` and `Group::size` are the seen target,
the target value as the group last saw it, in units that a change of the toy does not change:
fractions of the body of a File toy, or of the visible shaft of a spear. `Group::extent`, the size
of that whole in box coordinates, is written by `crop_scissors_tick` for `crop_scissors_close`,
which has no toy to measure. The drawn value is the seen target plus the remainders
`center_deflection` and `size_deflection`, the height multiplied by `unfolding` (`DrawnSize`).
`Group::Unfold` lets the remainders take up a change of the seen target (`SmoothTargetUpdate`), so
a dragged toy, a dragged globe or a changed outer crop moves the box without delay, and only a
change of the parameters is animated. `crop_scissors_move` and the handle drags assign the seen
target of their own group right after they write the target value, so the dragged scissors follow
the pointer, while every other scissors toy of the Crop View is woken by the monitor and animates.

## Groups of gizmos

`CropScissors::groups` holds one group per Croppable (`Group::croppable`). Only `crop_scissors_tick`
makes a group (`FindOrMakeGroup`), because the apparent value starts with the gizmos; the other
entries and the handle drags use `FindGroup`. A new group starts folded, except before the first
`Tick` of the scissors toy, so a loaded connection shows no unfolding. The group of the chain end
is the current one when its `crop_scissors_tick` assigned a box. Every other group gets
`crop_scissors_close`, or loses its gizmos at once when its object is gone, and groups without
gizmos are erased. Scissors swung across several toys fold several boxes while the new one
unfolds, and scissors that return to a box that is still folding unfold it from where it is drawn.

The scissors write `box->ToScissors()` (the box without the remainders of the pose of the
scissors) into the `local_to_parent` of the gizmos of the current group and leave the other gizmos
where they are, so a box unfolds at its place on the toy while the scissors travel to it and folds
where it was while they travel away. `OnReparent` fixes every gizmo, and `CropScissors::DrawBounds`
include every gizmo, because the renderer does not visit the children of a widget whose bounds lie
outside the window. `CropHandle::outline` is made once, relative to `origin`, and scaled by the
unfolding factor; `CropHandle::FindOption` returns its option only while it is set, so a folding
handle cannot be pressed. `CropBox` has an empty shape. The gizmos of the current group have no
texture anchors, so a texture that is not fresh takes the anchors of the scissors and follows the
pointer. The gizmos of other groups have one plain anchor at their origin, because the renderer
gives a detached child without anchors those of its parent (`PackFrame`, src/renderer.cpp), and a
folding box must stay where it is while the held scissors move on.

## The drag

The toy of the end under the scissors (`FindEndToy`) is the toy of the target in the `ToyScope` of
the scissors, or, for a target Crop View, the cropped toy that its toy returns from
`FindWidget(&CropView::croppable_tbl)`, while its owner is the chain end (a cropped toy can show
the previous end for one frame). A point lies on the Croppable when the stored `shape` of the
widget that this toy returns from `FindWidget` with the table of the Croppable contains it. The
default `FindWidget` returns the toy itself; `MemMeldToy` returns its shaft, on which the crop is
selected. A Croppable thus offers its area through the shape of one widget.

`DragScissorsAction` keeps an offset from the pointer to the center of the box, taken at a press on
scissors with a box, so the box does not jump; the touch point is the pointer plus the offset. It
does not read `Pointer::hover`, which includes the scissors and their gizmos. Connected scissors
stay connected while the pointer or the touch point lies on the Croppable, because a press on a
half of the scissors can lie outside the shape (always on the narrow band of a spear). Otherwise
they disconnect, the offset becomes zero, `base_source` is cleared so that the next `Tick` takes a
new box as a new source, and the search runs in the same call: the locations of the board of the
scissors, top first, under `engine.mutex`, as in `BoardWidget::ConnectAtPoint`; the first object
that `CanConnect` accepts, whose target has a chain end and on which the pointer lies, is
connected. `Connect` comes before `crop_scissors_open`, because the buffer exists only after the
cascade set the chain end. Scissors that no board holds find no target. A release keeps the
connection.

## The widget of the Crop View

`CropViewWidget::Tick` destroys the cropped toy without a chain end, and otherwise calls
`crop_make_toy` when there is no cropped toy or its owner is another object, then `crop_tick_toy`,
and wakes the cropped toy when it made it or the `Tock` is shaping. A new cropped toy is followed by
`InvalidateConnectionWidgets(true, false)` of the location, because the scissors of an inner view
may have found no toy before and nothing else wakes them. The cropped toy lies 0.3 mm above the
fabric, under the scissors, and its pointer events go to the Crop View.

## The scissors toy

The scissors are one widget for the life of the toy of the Crop View. `CropViewWidget` holds them
while no board does; `OnMakeToy` and `OnDestroyToy` of `scissors` move them to and from a board
(`docs/parrots/Boards.md`). The pose (pivot, turn, scale), the distance between the halves and the
elevation are each a base plus a remainder, as in `SyncBelt` (src/sync.cpp). The base of the pose
is the box, else the pointer with a horizontal turn, else the home; when the widget that gives it
(`base_source`) changes, the remainders take up the difference. The base of the distance between
the halves is the drawn height of the box; each half is clipped to its half of the picture, so a
box of any height shows each half outside it. The base of the elevation lies 1 mm above the toy
under the scissors, or 5 mm while a pointer holds them, because the renderer measures a shadow from
the elevations of the widgets in one texture (`RenderHeightMap` in src/renderer.cpp). The scissors
and the gizmos have `local_to_parent_weight` 0, and the scissors have a texture anchor at the drawn
pivot only while a pointer holds them or drags the toy that gives their base, computed as
`EndpointAnchor` (src/ui_connection_widget.cpp) computes the end of a connection. A press on the
scissors moves the location of the Crop View to the top of its board (`Board::MoveToTop`). The
board orders a connection toy above the higher of the locations at its ends and above the widget
that `FindWidget` returns at its end when that widget is a child of the board, so the scissors lie
above every object and above the shaft that they crop.

## Persistence

`CropView::SerializeState` writes `"hue"` and, with a chain end, `"crop"` with the name of the end
and the value of its `crop_view_serialize`. The saved crop names the end because the format of the
buffer belongs to it and because the links of an object are connected after its state keys are
read. `DeserializeKey` finds the end with `LookupInterface`, assigns it with a new buffer and calls
`crop_view_deserialize`; the later `OnConnect` keeps the buffer.

## The two Croppables

`library::File` stores `File::CropParameters`, a rectangle in fractions of what the toy shows,
which is also the seen target; `FileToy::crop` is the composed rectangle. A cropped toy has the
aspect ratio of the crop and no label. Four corner handles (`kDragCropHandle`) keep the opposite
corner and `kMinCropSize`. The `croppable` slot of the `HeapToC` of a File is always present, so a
link to it survives a load. `library::MemMeld` stores `MemMeld::CropParameters`, an address range
as offsets from the begin of the parent crop (`CropInside` keeps it inside and never empty);
`MemMeldToy::crop` is the absolute range. The box lies on the shaft between the positions of the
ends of the range (`SpearShaft::sections`), and two `MemMeldToy::CropEndHandle`s keep
`kMinCropLength` (`docs/parrots/Mem Meld.md`).

## Look

The colors share one HSV hue (`CropView::hue`, `CropColor`). The outline of the Crop Box is a white
hairline with dashes of the tool's color, so it is seen on light and dark content. The light comes
from the upper left at every pose: shading that does not depend on it is cached (`RasterPatch`),
and the gradients that depend on it turn with each part; a handle uses the turn of its own
`local_to_parent`. The stitches follow an outline built from the cropped toy, not from `shape`.

## Known limits

- Held scissors are drawn under the toolbar. A copy of a Crop View gets a new hue and no
  connection. A File without an image is cropped by its icon. Menus offer no crop entries.
- A view whose target is a Crop View without a chain end writes no crop. Scissors dragged off their
  Croppable lose the crop and the crops of the inner views; nothing recovers them.
- A press between the two halves of connected scissors reaches the toy under them, because the
  shape of the scissors is two outlines around the pivot that move apart with the halves.
- The gizmos of a deleted Croppable vanish in one frame. A shaft without visible length folds the
  box. A loaded MemMeld is not attached until it is started, so its crop is a short box at the
  bottom of its shaft. A new memory layout can make a cropped range shorter than `kMinCropLength`.
- A toy dragged fast over a board is bent by the weight of its location, so a part of it far from
  the grabbed point is drawn behind the scissors. Crop modes and an animation of the shape of the
  Crop View are not implemented.
