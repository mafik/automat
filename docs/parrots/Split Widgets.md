# Split Widgets

## Problem

Each widget occupies one position in one `layers` stack, so all of its pixels are composited at
one depth. Connections need different depths in different screen areas. Example: a sync belt
should be drawn under the rocker of a FlipFlop at one end, over the boards and objects between
its two endpoints, and under a widget of the other object at the other end.

In the general case, no assignment of one depth per widget can satisfy such requirements. Two
connections between the same two objects, where one is drawn over A and under B and the other
under A and over B, create a cyclic set of ordering constraints. The publication "Meschers:
Geometry Processing of Impossible Objects" (https://anadodik.github.io/publication/meschers/)
describes the same situation: depth relations can be consistent in every local neighborhood and
still contradict each other globally. Rendering such a scene requires resolving the drawing
order per screen region instead of once per widget.

Split widgets implement this per-region resolution. The drawing of one widget is divided
between screen regions, and each region has its own position in the drawing order. The widget
itself stays a single object: one parent, one `Tick`, one cached texture, one hit shape.

## Terminology

- **split widget** — a widget whose drawing is divided between screen regions with different
  depths. Sync belts and connection cables are the current split widgets.
- **cover** — a widget that is drawn over some region of a split widget. Example: the
  FlipFlop's rocker is the cover of the sync belt end that connects to the FlipFlop; a
  location resting on a connection's endpoint is a cover of that connection.
- **`splits_under`, `splits_over`** — two lists present on every widget (src/widget.hpp) that
  store the relation from both sides. The `splits_under` list of a cover holds the split
  widgets drawn under it. The `splits_over` list of a split widget holds its covers.
  `Widget::SplitUnder(over)` adds the relation and `Widget::UnsplitUnder` removes it. Both
  lists are `MortalList`s: when a widget is destroyed, its entries in other widgets' lists are
  removed automatically.
- **body** — the part of a split widget drawn at the widget's own position in the drawing
  order: its texture minus every cover's rectangle.

## Drawing

`BakeChildStack` (src/widget.cpp) records the drawing of each widget. Two additions implement
split widgets:

1. Directly before a widget's own texture is drawn, the widgets from its `splits_under` list
   are drawn, clipped to the widget's rectangle minus the rectangles of each split's lower
   covers. Because this happens immediately before the widget itself, the clipped parts are
   covered by the widget but drawn over everything that comes earlier in the drawing order.
   Pixels claimed by several covers belong to the lowest one: it draws the split before any of
   the other covers paint, which keeps the split under all of them. Without the subtraction, a
   higher cover whose rectangle overlaps a lower cover — easy with stack rectangles, whose
   empty corners can stretch over another cover — would repaint the split on top of it.
2. A widget with a non-empty `splits_over` list has each cover's rectangle subtracted from the
   clip before its own texture is drawn. The remaining part is the body.

The rectangle of a cover is its `Widget::CoverBounds()` transformed to window coordinates and
rounded to whole pixels (`CoverBoundsInWindowPx`, src/widget.cpp). The default `CoverBounds`
is the widget's own drawn extent: `DrawBounds()`, queried by PackFrame every frame and cached
in `Widget::draw_bounds` (widgets whose extent follows the window size, such as the root,
change it without any shape update). `Widget::subtree_draw_bounds` unions the cache over the
child tree in the pass that maintains `subtree_shape`. The window-space rectangle is computed
again on every frame, because the whole scene is re-recorded every frame and panning or
zooming changes it continuously.

The rectangles and the body partition the screen: every pixel of the split widget is drawn
exactly once, from the same cached texture, with the same transform and the same per-frame
deformation. The clips are integer rectangles applied at the playback base matrix, and that
matrix translates by a whole number of pixels, so the clips stay aligned to the pixel grid.
Deformation by the ANCHOR_WARP compositor is applied inside the drawable, after the clip, and
is identical in every draw of the same widget within a frame. As a result there is no visible
seam and no double blending of translucent pixels at region boundaries, under any deformation.

## Ordering

The board orders every connection — cable or sync belt, split or not — directly above the
higher of its two endpoint locations (the ordering pass in `BoardWidget::Tick`, src/board.cpp).
Above both endpoints, because a body ordered below an endpoint would be covered by that object
in the area between the cover's rectangle and the object's boundary, which looks like a gap in
the connection. Directly above and no higher, because connections are ordinary members of the
object stack: an object ordered higher covers the connection, so the user can place an object
on top of a connection.

## Requirements and policies

- A cover must be drawn as a separate step of the drawing sequence. This means it must be a
  detached child (the Over or Under band of `layers`), not baked into its parent's texture. A
  baked child provides no position at which a split could be inserted.
- A split widget may have any number of covers. A sync belt keeps at most two, one per
  endpoint. The cover of an endpoint is the widget of the endpoint's interface, resolved with
  `Toy::FindWidget` (src/toy.hpp; see docs/parrots/Objects and Modalities.md). The default
  implementation returns the toy itself: the connection is then drawn under the entire widget
  and is visible only outside of it. A toy can map an interface to a child widget instead: the
  FlipFlop maps its State interface to the rocker, so belts are visible between the rocker and
  the panel's edge.
- `SyncBelt::Tick` and `ConnectionWidget::Tick` compare their `splits_over` list with the
  wanted cover set and rebuild it when they differ. After a change they call `WakeAnimation`
  on the board so that the ordering pass runs again.

## Covers from stacking

An object dropped onto a connection's endpoint must cover the connection, or the cable would
be drawn across the object while ending under the endpoint beneath it — a depth contradiction
in exactly the area the user is looking at.

Each board maintains the direct overlap relation between its locations:
`LocationWidget::overlapping_above` and `overlapping_below` (src/location.hpp) hold the
locations whose `subtree_shape` intersects this one's, split by z-order. `BoardWidget::Tick`
compares every location's (shape generation, z-index) against a snapshot and rebuilds the
whole relation when anything changed (`BoardWidget::RebuildOverlaps`, src/board.cpp), so the
lists are derived state with a single writer, one frame behind at most. `ForStack` walks the
same relation, so pile pick-up and covers share one source of truth.

A connection's wanted covers are its endpoint covers plus the locations directly overlapping
either endpoint (`AppendObscurers`, src/location.cpp), excluding the two endpoints themselves —
a cable is supposed to be drawn over the faces of the objects it connects. One cover entry per
direct obscurer suffices, because a location's cover rectangle spans its whole pile:
`LocationWidget::CoverBounds()` returns `stack_draw_bounds` — the location's
`subtree_draw_bounds` united with those of everything transitively above it, computed during
the same rebuild. An object overhanging the pile is therefore inside the rectangle without its
own entry, and pile growth changes only the rectangle, never the cover lists.

## Limitations

- A cover's rectangle is larger than its visible shape. Inside the rectangle but outside the
  shape, the split widget is drawn at the cover's depth instead of its own. The difference is
  visible only when a third widget overlaps that area.
- Rectangles are used instead of exact shapes because integer rectangles partition the screen
  exactly. Complementary antialiased path clips would blend some boundary pixels twice or
  leave them unpainted.
