# Beta Brand

## The problem this style solves

Automat mixes hand-crafted Toys with Toys made by stochastic parrots, and a
person must be able to tell which is which at a glance — on the board, at low
zoom, in a screenshot. The mark has to be clearly visible without reducing
usability: a parrot-made tool is still a real tool.

## The solution

Parrot-made Toys are drawn in a deliberately cheap, MS-Paint-like hand
style: thick wobbly outlines that do not quite close, flat fills that do not
exactly match their outlines, hard offset shadows, irregular ornaments and
hand-printed lettering. The style itself is the identification. Only the
rendering is sloppy; the layout, affordances and contrast follow the usual
interface rules. The style is implemented as the drawing kit in
`src/ui_beta.hpp` and `src/ui_beta.cpp` (namespace `automat::ui::beta`);
colors, stroke weights and sizes are the constants defined there.

Three redundant signs make the identification survive cropping and zoom: the
tilted red starburst sticker reading "BETA", drawn half outside the panel's
top-right corner, which is the primary mark; the outline with mismatched
corner radii and bowed edges; and the fill that is offset against its
outline. At low zoom the sticker is the sign that remains visible.

## Principles

- Only the rendering is irregular, never the layout. Outlines, fills and
  glyphs wobble; spacing, alignment, hit targets and hierarchy are exact.
- Legibility comes from contrast rather than clean edges: every fill gets a
  dark outline, and pale fills such as yellow and lime are always ringed,
  never used as bare text or unringed marks on white. A thin line is never
  drawn directly against a bold border, because the combination looks like a
  rendering artifact.
- The irregularity is deterministic. Every wobble is seeded from a stable
  key and renders identically every frame. Changing shapes suggest activity,
  and that suggestion is reserved for actual state changes.
- Every irregularity parameter is bounded — wobble of a few pixels, fill
  offset of a few pixels, glyph rotation of a few degrees. Within these
  bounds the style looks cheap; beyond them it looks like broken software.
- Anything a person must read or click stays unambiguous and correctly
  sized. When something has to degrade, it is always the decoration.

## Color

The palette is the default MS-Paint palette; using exactly that palette is
the point, so it must not be modified. The exact values are the kit
constants. The roles: all outlines are ink; panels are white or cream; red
indicates errors and is the sticker color; green indicates that something
can run or is running; yellow marks draggable handles — the slider gem, the
knob's tip dot, the level tag — and is always used as a fill, never as a
line, because yellow lines are invisible on bright backgrounds; cyan fills
value indicators such as slider tracks; blue is used for links, selection
and ports; gray indicates disabled. Text color is not chosen by arithmetic
contrast but by the rule implemented in `TextOn`: white on the deep
saturated fills, ink on everything else.

## Type

Text uses the Kindergarten font (a Fontalicious freeware face, extended
with the Polish character set) at `assets/Kindergarten.ttf`, loaded from the
embedded assets. The face misses several symbols, so text renders through a
per-codepoint fallback chain to NotoSans rather than drawing any glyph by
hand. The font is already irregular, so titles receive only a slight
per-glyph wobble. Numeric readouts always sit on a straight baseline so
they stay easy to read.

## States

Every interactive component shows its state on two redundant channels, so
color is never the only signal and every state is readable in grayscale.
Disabled controls are gray with hatching; the label stays readable. Errors
are shown with a red outline, an attached red exclamation chip and a
squiggle. Pressed controls have a darker fill and no shine. Selected
controls are marked with a dashed blue ring.

The kind of a control is visible at rest, not only under the cursor. The
view is straight top-down and nothing moves sideways when pressed. Buttons
have a white specular shine on their face; the shine separates them from
same-colored status pills and disappears while pressed. Buttons have no
shadow. Hovering a button brightens its fill and changes the drawing's seed
a few times per second; the variants switch instantly, without easing.
Draggable handles are filled yellow; hovering one makes its outline bolder.
Selectable controls use blue and show the dashed selection ring on hover.
Thin colored lines are never used to indicate state.

## On the board

Automat's board is a warm dark texture. White and cream panels have high
contrast against it, so no dark theme is needed. Ink strokes drawn directly
on the board have low contrast; marks should be drawn on a panel.
