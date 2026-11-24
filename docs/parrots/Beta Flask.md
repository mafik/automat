# Beta Flask

The toolbar object `BetaShelf` (src/library_beta_shelf.cpp, `BetaShelfToy`) is drawn as a
corked Erlenmeyer flask of clear glass that is a third full of a green opalescent liquid. The
liquid is a particle simulation, drawn by the shader `assets/flask_liquid.sksl`.

## Requirements

The liquid reacts to every movement of the flask on the screen: a drag on a board, the pull of a
new flask out of the toolbar, the shift and growth of the toolbar icons, the drop animation and a
camera pan. A flask whose liquid is settled is idle: it neither ticks nor draws. The settled
liquid is exact at every zoom: a level surface with a small meniscus and an edge of one pixel.
The look is modest in detail, because the flask stands next to the hand-crafted toys of the
toolbar and is not drawn in the beta style (see `docs/parrots/Beta Brand.md`).

## Two pictures of the liquid

The liquid has two pictures. The level picture is computed from a formula: the height `level`
plus the meniscus near the cone wall. It needs no particles, it is exact at every zoom, and it
is the picture that stays on the screen while the toy is idle. The particle picture shows the
liquid while it moves. The particles alone cannot be exact or idle, because the surface of a
particle liquid is never exactly level and never exactly still.

`level_weight` mixes the two pictures: at 1 only the level picture is drawn, at 0 only the
particle picture. It also scales the strength of the streaks, so the streaks fade together with
the particle picture. `BetaShelfToy::Tick` lowers `level_weight` towards 0 while the particles
are not settled, and raises it towards 1 while they are settled and the flask moves at the
velocity that the liquid already follows. Before it rises, `level` is set from `Liquid::Level`,
so the level picture takes over at the height of the particles. The fall is fast, so that a
disturbance shows at once, and the rise is slow, so that the change of picture is not seen.
While `level_weight` is below 1 the particle picture is on the screen, and the toy ticks and
draws in every frame. When it reaches 1, every `sheen` is set to zero, so that old streaks do
not return with the next movement, and the toy becomes idle unless the flask moves.

## How movement reaches the liquid

The renderer writes `local_to_window` before `Tick` and calls `TransformUpdated` when it
changed. `TransformUpdated` only wakes the toy at `timer.last`, so that the woken `Tick` sees the
time of one frame. `Tick` maps the change of the window position of the liquid back into local
units with the inverse of `local_to_window` and divides it by the frame time. The result is the
velocity of the flask in local units, so a flask behaves the same at every zoom relative to its
own size. Window coordinates are used instead of the position on the board because the toolbar
icon never changes its position on a board, and a camera pan moves no location.

While a pointer drags the flask, its board position moves in steps of whole millimetres and the
toy follows it with an easing, but the picture on the screen follows the pointer through the
texture anchor that `LocationWidget::AnchorToPointer` puts on the toy. `Tick` therefore adds the
displacement of every pointer anchor in `texture_anchors` to the window position, and ticks in
every frame while such an anchor exists, because the pointer can move without a change of
`local_to_window`. The anchors are only read.

A change of position faster than `kJumpSpeed` is a jump of the picture (the first placement of
the toolbar icons, a camera jump, a widget that returns to the screen) and is not passed to the
liquid. While the flask moved in a frame, `Tick` requests the next frame, so the `Tick` in which
the flask stops is seen and the stop reaches the liquid. A flask that moves at a constant velocity
ticks in every frame but does not simulate or draw.

The liquid is simulated in the frame of the flask, so the walls do not move, and the acceleration
of the flask acts on every particle with the opposite sign. `Liquid::Simulate` lets
`Liquid::flask_velocity` approach the measured velocity in every fixed step and takes the change
per step as the acceleration, so a change of velocity within one frame does not become an
acceleration that depends on the frame time. The response saturates: a small acceleration is
passed at a fixed fraction and a large one approaches a maximum. A linear response cannot serve
both ends, because movements on the screen range from the shift of a toolbar icon to a shake
that is two orders of magnitude stronger; a gentle movement must show and a shake must keep the
liquid inside the flask. Gravity is scaled down so that the small flask sloshes like a large
vessel.

## Simulation

`Liquid::Simulate` implements double density relaxation in its symmetric form (Clavet, Beaudoin
and Poulin, "Particle-based viscoelastic fluid simulation", 2005), with pairs found in a grid of
cells, a signed distance to the inside wall of the glass and a viscosity impulse for approaching
pairs. The same step also updates `sheen`, the shear of every particle, which the shader draws as
streaks. It runs whole steps of `kStepTime` and keeps the remainder in `unsimulated_time`, so the
result does not depend on the frame rate; one `Tick` simulates at most `kMaxTickTime`. `settled`
in the returned `animation::Progress` means that every particle is slower than `kSettledSpeed`.
`SettledLiquid` fills the inside with a lattice of particles and simulates it for `kSettleTime`,
once per process. Every new toy copies that liquid, so a new flask starts settled.

## Drawing

The liquid is drawn into a layer in device space, clipped to the inside of the glass, whose paint
is `SkImageFilters::RuntimeShader` with the shader of the liquid. While `level_weight` is below 1
every particle is drawn into the layer as a soft round sprite, in one `drawAtlas` call, and the
red channel of the sprite carries its `sheen`. The alpha of the layer is then a density field.
The shader computes the signed distance to the surface from the field and its gradient, measured
in pixels, so the edge of the moving liquid is one pixel wide at every zoom instead of a blur
that scales with the sprites. The layer has 16-bit float channels because the shader takes
differences of the field between neighbouring pixels and divides the red channel by the alpha,
and 8-bit channels would quantize both into visible steps. The colors depend only on the position
in the flask and on the layer; the shader has no time uniform, so the picture is still when the
particles are.

The cork and the glass are static. The paths, the sprite image, the compiled shader and the paints
are built once, and the cork and the glass are cached in `RasterPatch` images that all flasks
share. Every draw composites with source-over, because cached images drawn with other blend modes
rendered wrong at small sizes on a Windows GPU (the same rule as the Process globe in
src/library_process.cpp).

## Decisions

The flask is closed with a cork, so that the cork stops splashes and no particle can leave the
flask. The flask carries no lettering, because the requirement of modest detail excludes
labels and marks.

## Known limits

Gravity points down in local coordinates, so a rotated toy would show a level that is not
horizontal on the screen. The meniscus formula assumes a surface on the cone, not in the neck.
Pairs beyond `kMaxPairs` are ignored. Below `1 / kMaxTickTime` frames per second the liquid moves
slower than real time. A movement faster than `kJumpSpeed` is not passed to the liquid.
