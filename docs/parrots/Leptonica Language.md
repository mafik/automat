# Leptonica Language

This document records the design language of the Leptonica image-processing
objects: the intent behind their shapes, the control vocabulary, and the rules
that keep new work consistent. The objects are implemented in
`src/library_leptonica.hpp` and `src/library_leptonica.cpp`; the controls in
`src/ui_leptonica.hpp` and `src/ui_leptonica.cpp`. This document records the
reasoning; the implementation details are in the code.

## The model

There is one editable image object and many tools that act on it. The
**LeptonicaImage** (the "paper") owns the pixels, stored as a Leptonica `Pix`
at its native depth. Every tool derives from **PhotoTool** and is a verb: it
points at the image it edits through an "Image" cable, applies its operation
in place when run, and chains to the next step through "Next". Keeping a
single editable noun means tools never need to answer "where does my result
go" — it goes back onto the paper, which is what a person editing a photo
expects. The Image cable auto-connects when a tool is dropped near a paper,
because the obvious case should not require manual wiring. Other image
providers, such as a Window capture, are read-only sources; a tool asked to
develop onto one reports an error instead of guessing.

The **shelf** (object name "Leptonica", available in the toolbar) is the entry
point. It presents the curated tool set grouped by editing intent rather than
by API name, because users look for "make it black and white", not for a
function name. The groups, in working order: PAPER holds LeptonicaImage and
Generate; LOOK holds Tone, Color, Channel, Flatten and Fade; NEIGHBORS holds
Convolve and Blend; PALETTE holds Quantize, Posterize and Dither; SHAPE holds
Geometry, Warp, Crop, Deskew and Reduce; MEASURE holds Find Level, Count and
Measure; INK holds Threshold, Select, Morphology and Seedfill. Dragging a tool
off the shelf drops a working copy on the board.

## Why one control vocabulary

Leptonica looks like hundreds of unrelated functions. Wrapping each one in its
own object produces hundreds of single-purpose interfaces that nobody can
learn, and classifying controls by C type — every float becomes a knob — makes
unrelated meanings look identical while hiding what the parameter actually
does. The library's API in fact reuses a small set of parameter primitives: a
rectangle to operate in, a value on the intensity axis, a fraction, a
neighborhood pattern, a connectivity choice, scale factors, and a selector for
which member of an operation family runs. The language therefore assigns one
canonical control to each primitive and classifies controls by image-editing
meaning. A person learns roughly a dozen controls once and can then read every
object.

## The control vocabulary

Each control's form is chosen so that the form itself teaches the parameter's
meaning. The forms are drawn by `src/ui_leptonica.hpp`; the reasoning:

- **Level** sets one value on the intensity axis (a threshold, a clip point, a
  fraction). It is a marker on a band drawn over the image's own brightness
  histogram, because the right threshold can only be judged against the data
  it cuts; the band darkens on one side of the marker and lightens on the
  other, so the control previews the result before the operation runs.
- **Window** sets a low and high pair by placing two Level markers that
  bracket the active range, with everything outside visibly clipped.
- **Curve** sets a value-to-value remap (gamma, levels, inversion). It is a
  graph drawn over the identity diagonal, because the meaning of a tone curve
  is exactly its deviation from identity.
- **Stamp** edits a neighborhood pattern (a structuring element or kernel) as
  a small grid, drawn as the face of a rubber stamp because the operation
  applies the pattern at every pixel. The origin cell is marked because the
  pattern is anchored there.
- The **Mode wheel** picks which member of an operation family runs. Every
  option stays visible around the dial, because a closed dropdown hides the
  family structure that the wheel is meant to teach; the selected option can
  show a small before-and-after glyph on the face.
- **Connectivity** chooses 4- or 8-connectivity with two cells whose spokes
  are the directions a fill may spread.
- **Polarity** shows which value counts as foreground — a two-ended pill with
  a dark and a light end, the active end ringed.
- **Region** selects a rectangle by dragging a marquee directly on the
  preview. A rectangle is spatial, so the control is placed on the image
  itself; the area outside the selection dims, previewing the cut.
- The **Transform ring** sets an angle by dragging a ring drawn around the
  preview; the **Dial** is the off-image fallback when no preview is present.
- The **Channel tap** selects a color channel from a row of taps.
- The **Palette** picks a paint color from the MS-Paint swatch strip, which
  is familiar and matches the beta palette (docs/parrots/Beta Brand.md).
- The **Depth chip** is a readout, not a control: a corner badge stating the
  image's bits per pixel, because depth gates what each operation can accept.

Small integer counts use the Stepper from the beta kit. Seedfill's seed point
is a pin placed directly on the preview. When adding a parameter, the
defaults are: a continuous magnitude becomes a Level, drawn over the histogram
when the operation reads pixel statistics; a value remap becomes a Curve; a
neighborhood becomes a Stamp; a rectangle becomes a Region; an operation
family becomes a Mode wheel; an angle becomes a Transform ring when a preview
is present and a Dial otherwise.

## Parameters are ports

Any parameter may be driven by data instead of its hand control. Connecting a
number-bearing object to a parameter's port grays the control into a readout
of the driven value. This is the modular core of the language: measuring
tools emit their findings through ports, so find-then-apply pipelines need no
special objects. Find Level feeding a Number feeding Threshold's Level port
is an adaptive threshold; Deskew's Fix output feeding Geometry's Angle port
steers the rotation. Port icons name their data type — a framed landscape for
images, a hash mark for numbers, a swatch grid for palette sources — so a
glance tells what may plug where.

## Identity rules

These rules hold for every object; each exists for a reason.

- Objects are named after their operation, plainly: "Threshold", not an
  invented metaphorical name. The metaphor belongs in the form, where it can
  be seen, not in the label, where it must be decoded.
- The silhouette signals the function and is never a rounded rectangle,
  because the outline is the identity signal that survives distance and low
  zoom. Threshold's top edge is the live brightness histogram; Posterize's
  edge is a staircase; Quantize is crenellated with palette chips.
- The underlying Leptonica function appears as a small, muted credit near the
  name. Automat deliberately exposes the primitives it is built on; hiding
  them would limit the user.
- Controls may protrude past the silhouette but must never be clipped, and
  anything interactive must be part of the silhouette itself, because the
  pointer only reaches a widget where its shape contains the point. The
  constraint and its mechanics are recorded at the texture-bounds override in
  `src/library_leptonica.cpp`.
- The live preview never stretches the image. The preview is a measurement,
  and a stretched measurement misrepresents the data; the shared preview
  helper centres the image at its true aspect ratio.
- Run is one shared green disc, identical on every object, placed clear of
  the name, so the most important affordance is learned exactly once.

## Layout

Data is placed on the left edge with inputs above outputs: the Image input
attaches at the left, data outputs are placed below it, and the Depth chip
readout is placed at the preview's lower left. Control flow exits downward
through Develop and Next, so a pipeline reads as a vertical run of bodies
joined by cables, like a list of instructions. Spatial controls are placed on
the preview itself, because separating a control from the thing it changes
forces the reader to look back and forth. Every control carries a word as
well as a glyph, because an icon without a label forces the user to guess.

## States

Every control follows the beta state model and shows each state on two
redundant channels. Disabled, cable-driven and not-applicable are all shown
as gray plus hatching; errors are shown with a red outline and an
exclamation chip; pressed controls have a darker fill and no shine;
selected controls are marked with a dashed ring. Meaning is conveyed by
shape, glyph and label rather than color alone, so every control is
readable in grayscale.
