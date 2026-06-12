# Beta Brand

## The problem this style solves

Automat mixes hand-crafted Toys with Toys made by stochastic parrots, and a
person must be able to tell which is which at a glance — on the board, at low
zoom, in a screenshot. The mark has to be impossible to miss yet cost nothing
in usability: a parrot-made tool is still a real tool.

## The solution

Parrot-made Toys draw themselves in a deliberately cheap, MS-Paint-like hand
style: thick wobbly outlines that do not quite close, flat fills that miss
their edges, hard offset shadows, lopsided ornaments and hand-printed
lettering. The style itself is the identification — an honest visual
admission of machine manufacture, worn cheerfully. The sloppiness is only a
skin: underneath it the layout, affordances and contrast stay disciplined.
The style is implemented as the drawing kit in `src/ui_beta.hh` and
`src/ui_beta.cc` (namespace `automat::ui::beta`); colors, stroke weights and
sizes are the constants defined there.

Three redundant signs make the identification survive cropping and zoom: the
tilted red starburst sticker reading "BETA", half off the panel's top-right
corner, which is the primary mark; the outline with mismatched corner radii
and bowed edges; and the fill that misregisters against its outline. At low
zoom the sticker is the signal that remains.

## Principles

- Jank the marks, never the layout. Outlines, fills and glyphs wobble;
  spacing, alignment, hit targets and hierarchy stay straight. The drawing
  may look careless, but the interface underneath must not be.
- Contrast does the legibility work that the messy edges give up: every fill
  gets a dark outline, and pale fills such as yellow and lime are always
  ringed, never used as bare text or unringed marks on white.
- The jank is deterministic. Every wobble is seeded from a stable key and
  renders identically every frame, because shimmering edges read as activity
  and activity is a signal reserved for actual state changes.
- Every jank parameter is bounded — wobble of a few pixels, fill offset of a
  few pixels, glyph rotation of a few degrees. Inside the bounds the style
  reads as cheap; past them it reads as broken software.
- Function is sacred and only decoration is dumb. Anything a person must
  read or click stays unambiguous and correctly sized; when something has to
  degrade, it is always the decoration.

## Color

The palette is the default MS-Paint palette, which is the whole joke and
therefore non-negotiable; the exact values are the kit constants. The roles:
ink outlines everything; panels sit on white or cream paper; red is danger
and the sticker; green means ready to run; yellow is the highlight and the
draggable accent; cyan fills value indicators such as slider tracks; blue
marks links, selection and ports; gray is the disabled wash. Text color is
not chosen by arithmetic contrast but by the brand rule implemented in
`TextOn`: white on the deep saturated fills, ink on everything else.

## Type

The hand-printed voice is the Kindergarten font (a Fontalicious freeware
face, extended with the Polish character set) at `assets/Kindergarten.ttf`,
loaded from the embedded assets. The face misses several symbols, so text
renders through a per-codepoint fallback chain to NotoSans rather than
drawing any glyph by hand. The font is already irregular, so titles receive
only a slight per-glyph wobble — and numeric readouts always sit on a
straight baseline, because numbers are function, not decoration.

## States

Every interactive component shows its state on two redundant channels, so
color is never the only signal and every state reads in grayscale. Disabled
is gray with hatching drawn under a still-readable label, with no shadow.
Error is a red outline with a docked red exclamation chip and a squiggle.
Pressed sinks toward the page and loses its shadow. Selection wears the
dashed ring.

## On the board

Automat's board is a warm dark texture. The white and cream panels pop
against it and the hard hand shadows read as real lift, so the kit needs no
dark theme. Bare ink strokes drawn directly on the board sit low in
contrast; marks belong on a panel.
