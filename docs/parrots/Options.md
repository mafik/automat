# Options

## Problem

Every way of interacting with an object (a button press, a key, a drag of one of its parts, a
command in the bubble menu) needs one description that the 2D board UI can query on every pointer
move to pick the cursor, that does not depend on which widget happens to be under the pointer when
a menu command is finally activated, and that later modalities (text, RPC) can reuse without the
widget tree. One direction of the bubble menu must also mean the same thing on every object of a
kind, whatever is stacked around it.

## Solution

Every interface is an option (`docs/Interfaces.md`, section "Activation"). `Interface::Table`
(src/interface.hpp) carries `activate`, `make_icon` and `cursor`; `Interface::Activate(Pointer&,
Toy*)` returns the `Action` that the activation becomes. Objects expose their commands as
interfaces, and the widgets only map triggers to those interfaces:

- A command that runs to completion is a Signal: `DEF_INTERFACE(X, Signal, name, "Label")` with
  `OnRun`. Command Signals set `static constexpr bool kSchedulesNext = false`, so
  `RunTask::DoneRunning` (src/tasks.cpp) does not fire the object's `next` argument. A command
  exposed as a Signal is also connectable and schedulable by other objects, which is why commands
  are not a separate kind. The default activation schedules the run (`Signal::Table::
  DefaultActivate`, src/base.cpp); a Signal whose activation is a gesture overrides it with
  `OnActivate` and has no `OnRun` (`kSplice` in src/library_timeline.cpp).
- A continuous value is a `Scalar` (src/base.hpp): `OnGet`, `OnSet` and an `OnActivate` that starts
  the drag of its handle (Timer `duration`, src/library_timer.hpp).
- Text is a `Text` interface (src/base.hpp); text fields are Toys of it (src/text_field.hpp).
- An object that hands out objects is an `ObjectSource` (src/object_source.hpp): `Take()` returns
  the object, the activation starts the drag. Location `move`, `copy` and `clone`, the prototypes'
  `kMakeObject` (src/object.cpp) and the instruction library's `pick` (its front card) are the
  tables. The activation makes the new object's toy where the object is born and hands the object
  and its toy to `DragNew`, whose drag springs that toy to the pointer: `kMakeObject` births the
  clone under the toy it receives, which is why the toolbar's `PrototypeButton` and the
  `ShelfButton` are Toys of the prototype they display, and `pick` places the card's toy at the
  front of the deck.
- Sub-options of an interface are tables nested inside its table (`sync` and `unsync` in
  `Syncable::Table`, `turn_on` and `turn_off` in `OnOff::Table`); the interface's own activation
  opens the menu that lists them (`Syncable::Table::MenuActivate`, src/sync.cpp).
- Parameters: an enum-valued option is one table per value (Timer `next_range` and
  `prev_range`); an indexed or continuous one is derived from the pointer position and the Toy
  inside `OnActivate`.

No table and no interface holds a widget. The widget that started the activation is passed to
`activate` as the Toy, and an `Action` may keep it as a `MortalPtr`. `pointer.hover` is never
consulted at activation, because the pointer may have moved, or the widget may be gone, by the time
a menu command is activated. State that exists only for the 2D modality (the twist of the timer
hand, an easter egg the Timer object never learns about) lives in a small Object owned by the
widget through a `Ptr` (`TimerHand`, src/library_timer.cpp), so that the zone can bind it like any
other interface. This is the last resort, used only when the operation cannot be expressed on the
displayed object.

## Widget side

`OptionsProvider` (src/widget.hpp) is the base of every widget:

```cpp
virtual MiniMenuMode MenuMode();
virtual Interface FindOption(ui::Pointer&, ui::ActionTrigger);
std::unique_ptr<Action> TriggerActivate(ui::Pointer&, ui::ActionTrigger);
std::unique_ptr<Action> OpenMenu(ui::Pointer&);
```

`ActionTrigger` is a pointer button, a key or a menu direction (`ui::Dir`). `FindOption` is a
switch: buttons and keys map gestures, directions lay out the menu. The tables hold no direction;
the widget that opens a menu decides the layout, and an interface activation that opens a sub-menu
decides the layout of that sub-menu. `FindOption` returns a plain `Interface` without transferring
ownership: the walkers (`TriggerActivate` and `OpenMenu` in src/menu.cpp, the cursor walk in
src/pointer.cpp) lock the owner through `Closest<Toy>` around the lookup and the activation, and
pass that Toy to `Activate`.

`Pointer::ButtonDown` and `KeyboardWidget::KeyDown` walk from the hovered widget up to the root and
the first provider that answers wins. A button is not a direction, so this fallback aliases nothing:
a silent zone or toy costs nothing. The cursor (`CheckCursorChanged`, src/pointer.cpp) is the most
recent `CursorOverride` if there is one, else the `cursor` of the first left-button interface found
by the same walk, else the arrow.

`FindOption` runs on every pointer move and inside `RootWidget::Tick`, so it only describes the
current state: no mutation, no object mutexes beyond the owner lock the walker already holds, no
widget creation, no `WakeAnimation`, and no pointer position. Which part of a widget is under the
pointer is decided by pointer routing, through action zones.

## Action zones

`ui::ActionZone` (src/widget.hpp) is a widget with nothing to draw: `DrawBounds` returns
`nullopt` and `Draw` is empty. Only its shape matters, for pointer routing. A part of a toy that
has its own gesture is a small `ActionZone` subclass with its own `Shape()` and `FindOption`,
created as a child in the toy's constructor (`DurationHandleZone` and `HandZone` in
src/library_timer.cpp). `FillPath` picks the zone as the hovered widget, its `FindOption` answers
the left button with the interface of that part, and the toy's own `FindOption` never hit-tests.
Zones are reshaped through the ordinary Tick mechanism: a zone returns `Tock::Shape` and the parent
wakes it when the part moves.

## Menus

The bubble menu (`Menu` in src/menu.cpp) has eight fixed slots of `NestedWeakPtr<Interface::Table>`.
`OptionsProvider::OpenMenu` asks `FindOption` for every direction valid in the provider's
`MenuMode` and opens the menu when any slot is filled; icons come from `Interface::MakeIcon`.
Swiping past the bubble radius locks the slot and activates it, with the slot's icon as the Toy when
that icon is a Toy (the prototype icons made by `kMakeObject`), else the toy the menu was opened
from, so a new object flies out of the bubble. `MakeMenuAction(pointer, mode, options, toy)` builds a
menu from a slot list; interface activations use it for their sub-menus, and `MenuTable<Table>(name,
open)` (src/interface.hpp) makes a table whose activation is such a menu.

Three hierarchies meet in a menu and must not compete for directions:

- The widget tree (zone, toy, `LocationWidget`, `BoardWidget`, `RootWidget`) routes triggers and
  never lays out menus.
- The ownership tree (Object, Location, Board, root) is combined by chained menus with one reserved
  direction: the object menu opens on the right button, its S slot is the Location, the Location's S
  is the Board, the Board's S is the camera menu. S is valid in every `MiniMenuMode`, so every small
  menu can still chain. A directional fallback, an outer provider filling the directions an inner
  one leaves free, is rejected: a three-option menu with wide sectors cannot host an outer
  provider's eight sectors, and the outer commands would change meaning with every stacking.
- The stack of objects under the pointer is not a menu hierarchy. Pointer routing picks the topmost
  object; the others are reached by reordering, never by a direction whose target depends on what
  lies below.

Conventions: S is the parent, NW is destructive, N is the primary action. Availability may vary with
state (the Timer's start pusher answers the left button with `run` while the timer is stopped and
with the `turn_off` sub-option of `running` while it runs), the meaning of a slot may not. Sub-menus opened by interface activations are leaves and reserve no S.

The generic layer provides only the slots that mean the same on every object. `ObjectToy::FindOption`
(src/object.cpp) answers N with the object's Runnable, or with `kThisIsFine` while the object shows
an error, and S with its Location; its `MenuMode` is two directions. The left button is not answered
by the object toy and falls through to `LocationWidget::FindOption` (src/location.cpp): Left and N
move, NW delete, NE iconify or deiconify, E copy, W clone, S the Board. `BoardWidget::FindOption`
(src/board.cpp): N move, NE toggle frame, S the camera menu. `RootWidget::FindOption`
(src/root_widget.cpp) maps the W, A, S and D keys and the N, S, W and E directions to the `Camera`
object's nudge Signals and the middle button to the camera drag. An object with more commands
overrides `FindOption` and `MenuMode` and places each command by hand (`TimerWidget` in
src/library_timer.cpp); nothing is placed by enumeration order, because adding an interface would
then shift every direction after it.
