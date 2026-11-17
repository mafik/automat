# In which way the memory increases?

## Arrow notation

The first arrow shows the local direction, and the subsequent arrows show the "higher level" direction (which way the notation will continue after wrapping around). The last arrow can be thought of as a "global" direction:

LTR text in columns: ⇒⇓⇒

RTL text: ⇐⇓

Japanese writing: ⇓⇐

Automat Registers: ⇐⇑ (because of left/right shifts & little-endian)

## Numbers ⇒

Let's look at numbers first:

### Spoken & written numbers

- in human language we communicate numbers starting with the largest components
- numbers are also written ⇒, starting with their highest components (even in RTL scripts)

### Numbers in memory

- computers can store & transmit numbers in big-endian or little-endian encoding
- in a big-endian memory numbers are stored ⇒ when the memory addresses increase ⇒
- in a little-endian memory numbers are stored ⇒ when the memory addresses increase ⇐

### Bits

If numbers are written ⇒ then bits must be written ⇐ (because of left/right shifts & little-endian)

## Physical world

- "high" things tend to go up and "low" down - which would imply memory growth upwards ⇑
- physical objects are placed "stacked" at the bottom, end then go upwards ⇑
- gravity makes things go down when they have freedom to do so ⇓

## Typography

- chat logs are written downwards (new logs appear at the bottom) ⇓
- the "order of recency" goes back in time - so ⇑
- notification updates appear on the top, so they're read in up order ⇑

## Diagrams

- wikipedia memory diagrams of data structures use a primary top-down memory axis, with left-to-right for bits & words (⇒⇓)
- hex dumps also follow ⇒⇓, which makes integer decoding tricky

## Picking directions

There must exist a global direction. (For memory this is: ⇑)

Memory is not just a sequence of bytes - they *encode* higher level abstractions.
Local direction within any of those objects can be *whatever*.
Boundaries between objects that stem from change of encoding direction is *good* - because it helps us distinguish those objects.

## Wrapping

But just one direction would produce a "skinny" / hard to read notation, so it needs at least one wrapping direction.

This wrapping can be perpendicular to the main axis. It may wrap: ⇉/⇇, alternate: ⇄, twist around: ↝ or even scatter ✴

A popular idea from the literature are hilbert curves (fractal "C" shape)

The other two confounding factors are that:
- the semantic size of the objects doesn't match their byte size (short text buffer > long section of zeros)
- the semantic representations may have fancy shapes

The approach that fits the best with fuzzy & irregular shapes is scattering. Space-filling approach may show the maximum number of objects at maximum size.
