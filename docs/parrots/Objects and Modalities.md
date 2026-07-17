# Objects and Modalities

## The model

Objects (src/object.hpp) encapsulate the core functions of computational entities. How an
object is presented and manipulated is the job of a modality: a separate mechanism that
exposes the same objects in one medium. The 2D board UI — Widgets and Toys — is one modality.
Other modalities may be built later around the same objects: text, 3D objects, RPC endpoints.
Everything specific to one medium belongs to that modality's layer, not to the objects.

For the 2D modality this means: instance geometry (shapes, connector positions, anchors) and
placement decisions live in Widgets and Toys. VM code must not create, look up, or measure
widgets.

## Where data belongs

An object may carry modality-specific data when that is its core function. Board and Location
exist precisely to place objects on a 2D plane, so board membership and positions are core
state (src/board.hpp, src/location.hpp). Most other objects must not track the 2D modality.

The VM provides hints so that the 2D world can function. Data of fixed size per type — O(1),
not growing with the number of objects — is fine in VM-side tables: `Argument::Table` carries
the connection style, tint, and autoconnect radius (src/argument.hpp).

## Placement rules

`Location::placement` (src/location.hpp) holds either concrete coordinates (`Direct`) or a
placement rule (`PlaceAhead`, `PlaceBetween`) — never both. When VM code creates an object
whose position depends on other widgets (an argument creating its missing target, a sync
connection creating its Gear), it does not compute coordinates; it stores a rule. Coordinates
are observable only through `Location::Position(LocationWidget&)` and `Scale(LocationWidget&)`,
so only code holding a live widget can read or write them; the first such call resolves a
pending rule with real toys (`Location::FillPosition`, src/location.cpp). Serialization and
other widget-free readers use `PeekPosition()`/`PeekScale()`, which report rules as the
defaults. Until a modality shows such an object, it has no meaningful position.

## Interfaces and widgets

Interfaces within an object can be mapped to 2D shapes or widgets. `Toy::FindWidget`
(src/toy.hpp) maps an interface of the toy's owner to the child widget that displays it; the
default is the whole toy. Connections use their endpoint interfaces' widgets as covers, which
is how a sync belt disappears under a FlipFlop's rocker (docs/parrots/Split Widgets.md).

## Notifications

`Object::wake_counter` is the modality-independent notification channel: an object bumps its
counter on every state change and never calls into a modality. Each modality polls the
counters of the objects it displays. For the 2D UI that is `RootWidget::Poll` (src/root_widget.hpp),
which runs once per frame; other modalities bring their own polling.
