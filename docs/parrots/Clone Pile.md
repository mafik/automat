# Clone Pile

## Problem

In several places an element responds to touch by creating a new object instead of moving
itself: the toolbar, the shelves, and any future palette or spawn menu. Without a visible sign
this behavior is undiscoverable, and the element looks identical to a placed instance while
behaving in the opposite way. One sign is needed that is learned once and then read everywhere
creation-on-touch occurs.

## Solution

A prototype that creates on touch is drawn as the squared top of a small pile of copies of
itself. The pile is chosen because its entailments are exactly the facts to communicate: there
are more underneath, so the supply will not run out; the user takes the top one, so touching
yields a new object; the pile stays, so the source is not consumed; and the top of the pile is
the live prototype itself, so the copy is exactly what is shown. The contrast communicates the
rest without words: a pile is a source, a plain shadow is an instance, and dragging an
instance moves it.

## The three states of the sign

- **Static.** The prototype sits squared at the top of two further copies of itself, fanned by
  rotation only — about five degrees counterclockwise and about three and a half degrees
  clockwise around the centroid. Each layer under the top darkens one step with ambient
  occlusion, tight contact shadows separate the layers, and the whole pile casts a single
  ground shadow, wider and softer than a lone object's, because the pile's thickness shows as
  penumbra. The shadow's offset encodes the light direction, never a camera angle.
- **Hover.** With a pointer hovering, the top copy lifts slightly — about one percent of scale
  over eighty milliseconds — and its shadow detaches and softens. Nothing underneath moves.
  Touch devices skip this state and lose no information, because hover only reinforces what
  the static state already shows.
- **Take.** The top copy departs with the pointer. The new top then squares up from its fanned
  pose over roughly a quarter of a second with a slight overshoot, uncovering a fresh copy
  already seated in the pose it vacated, while a brief specular glint sweeps across it.
  Insertion at the bottom of the pile is fully hidden from a top-down viewer, so the supply is
  infinite without any replenishment animation.

## The three rules

- **The under-copies are real.** They are the object's own rendering, rotated and darkened —
  never outlines, contours or hand-drawn marks. Depth is shown by shadow and occlusion, not by
  ink.
- **The projection stays top-down.** Layers are fanned by rotation alone, with no translation
  offsets between them, because offset layers read as an oblique camera angle and Automat's
  board has none. The top-down depth cues are the rotation slivers, the occlusion darkening
  and the shadow spread.
- **Bottoms rest, tops move.** The only element that ever animates is the top copy. The pile
  beneath never slides, fans or breathes, matching how a real pile of paper behaves when the
  top sheet is taken.

## Integration

The host — a toolbar slot (`PrototypeButton`, src/library_toolbar.cpp), a shelf cell
(`ShelfButton`, src/ui_shelf_button.hpp), or a future spawn menu entry — draws the
under-copies from the prototype's own rendering, rotated about its centroid, so no object
needs any per-object support.

Pickup behavior comes from the drag machinery, not from the pile. Piles are drawn at reduced
scale, so the standard LocationWidget scale animation already enlarges the taken object, and
its elevation shadow — new objects start shadowless and gain shadow as they rise — already
supplies the transition from a flat pile element to a grabbed object. The pile therefore adds
no jump, pop or overshoot of its own; doubling the existing animation would read as a
malfunction.

Placed objects are never drawn with the pile presentation.
