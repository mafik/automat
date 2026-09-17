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

- A command that runs to completion is a `Command` interface: `DEF_INTERFACE(X, Command, name,
  "Label")` with `OnRun`. Such commands set `static constexpr bool kSchedulesNext = false`, so
  `RunTask::DoneRunning` (src/tasks.cpp) does not fire the object's `next` argument. A command
  exposed this way is also connectable and schedulable by other objects, which is why commands
  are not a separate kind. The default activation schedules the run (`Command::Table::
  DefaultActivate`, src/command.cpp); a command whose activation is a gesture overrides it with
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
  `Syncable::Table`, `turn_on` and `turn_off` in `OnOff::Table`, all four Command tables). The
  interface lays them out itself: `fill_menu` in its table places them in the menu. A derived table
  calls the filler of its base first and then places its own: `Syncable::Table::DefaultFillMenu`
  (src/sync.cpp) places `sync` at E and `unsync` at W while synced, and the OnOff table
  (src/on_off.hpp) calls it and then places `turn_on` or `turn_off` by state at N. The primary
  action stays in `activate` (an OnOff toggles); an interface without `activate` opens its
  sub-options when activated.
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
virtual Interface FindOption(ui::Pointer&, ui::ActionTrigger);
virtual void FillMenu(ui::Pointer&, Menu&);
std::unique_ptr<Action> OpenMenu(ui::Pointer&);
```

`ActionTrigger` is a pointer button or a key. `FindOption` maps gestures: it answers a button or a
key with the interface that the gesture activates. `FillMenu` lays out the menu: it places
interfaces in a `Menu` (src/menu.hpp), each at an angle. A widget lays out the menu of its object;
an interface lays out only its own sub-options, through `fill_menu`. Both hand out plain
`Interface` values without transferring ownership: the walkers (`Pointer::ButtonDown`,
`KeyboardWidget::KeyDown`, `MenuWidget::Activate` and `OpenMenu` in src/menu.cpp, the cursor walk
in src/pointer.cpp) lock the owner through `Closest<Toy>` around the lookup and the activation, and
pass that Toy to `Interface::Activate`.

`Pointer::ButtonDown` and `KeyboardWidget::KeyDown` walk from the hovered widget up to the root and
the first provider that answers wins. A button is not a direction, so this fallback aliases nothing:
a silent zone or toy costs nothing. The cursor (`CheckCursorChanged`, src/pointer.cpp) is the most
recent `CursorOverride` if there is one, else the `cursor` of the first left-button interface found
by the same walk, else the arrow.

Each walker applies its own rules to the option it found. While Control is held (the `control`
FlipFlop of `RootWidget`, synced to the left Control key), every walker first tries
`Interface::DragNewController(pointer, birthplace)` (src/interface.cpp): an option with a controller
(`Interface::MakeController`, a `FlipFlopController` for every OnOff) is wrapped in one, whose toy
is born under `birthplace` and handed to `DragNew`, the same drag that springs a new object out of
a toolbar or a menu slot; an option without a controller falls through. The birthplace is the
widget that answered `FindOption` for a button or key, and the slot icon for a swipe. A swipe
(`MenuWidget::Activate`, src/menu.cpp) then enters the option's sub-options (`Interface::OpenMenu`)
when it has any. Otherwise the primary action runs (`Interface::Activate`). Buttons and keys act,
directions enter: the same OnOff toggles from its power button, opens its sub-menu from a slot of
the object menu, and becomes a controller from either while Control is held.

`FindOption` runs on every pointer move and inside `RootWidget::Tick`, so it only describes the
current state: no mutation, no object mutexes beyond the owner lock the walker already holds, no
widget creation, no `WakeAnimation`, and no pointer position. Which part of a widget is under the
pointer is decided by pointer routing, through action zones. `FillMenu` runs once, when a menu
opens.

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

A `Menu` (src/menu.hpp) is a list of slots, each an interface placed at an angle. `Menu::Place`
takes the angle as a `SinCos` or as one of the eight compass names of `ui::Dir`, which stand for
multiples of 45 degrees counted counter-clockwise from east. Placing at an angle that already holds
a slot replaces that slot, and placing an empty interface removes it. Nothing declares a shape: the
count and the orientation of a menu follow from the placements, so any count and any set of angles
is a valid menu.

`OptionsProvider::OpenMenu` fills a `Menu` through the widget's `FillMenu` and
`Interface::OpenMenu` through the table's `fill_menu`; `MakeMenuAction(pointer, menu, toy)` builds
the bubble (`MenuWidget` in src/menu.cpp) from it and returns nothing when the menu is empty. Icons
come from `Interface::MakeIcon`. The icon of each slot sits at its angle, at two thirds of the
bubble radius, and the bubble area is divided equally among the slots for icon sizing. The pointer
selects the slot whose angle is nearest to the pointer direction (`MenuWidget::PointerSlot`), so
each slot owns the sector reaching halfway to its angular neighbours: two slots at N and S split
the circle into an upper and a lower half, four slots at the cardinal points get 90 degrees each,
and a single slot takes the whole circle. Swiping past the bubble radius activates the slot, with
the slot's icon as the Toy when that icon is a Toy (the prototype icons made by `kMakeObject`),
else the toy the menu was opened from, so a new object flies out of the bubble.

An option keeps its angle in every menu that offers it, whatever else the menu holds; only its
sector width changes with the neighbours. A swipe learned on one menu therefore lands on the same
option in every other menu that has it. Options that exist in one menu only may be spread evenly
instead: the mouse menu (src/library_mouse.cpp) keeps N and S and spaces its six options 60 degrees
apart, and its button sub-menus space their five buttons 72 degrees apart around the middle button
at S.

A widget or table that builds on a base calls the filler of the base first and then places its
own slots, so a later placement at the same angle is an override, visible where it happens
(`MacroRecorderWidget` in src/library_macro_recorder.cpp replaces the Runnable at N with its
`recording` interface). The base places only the slots that mean the same everywhere and never
needs to know the final shape, because the sectors follow from the final list.

A node that only groups other options is a table of kind `kMenu`, made by `MenuTable(name,
fill_menu)` (src/interface.hpp): it has `fill_menu` and a name, but no `activate`, no state and no
methods, so no cast accepts it and `MakeController` returns nothing for it. An Object never reports
such a table through `INTERFACES()`; it is only placed in menus (the camera menu in
src/root_widget.cpp, the decoration menu in src/window_frame.cpp, the shelf categories in
src/library_beta_shelf.cpp, the register pages in src/library_assembler.cpp, the mouse menus in
src/library_mouse.cpp). Interfaces are for Objects to call; a menu exposes nothing to call, so it
is not one of an Object's interfaces.

Three hierarchies meet in a menu and must not compete for directions:

- The widget tree (zone, toy, `LocationWidget`, `BoardWidget`, `RootWidget`) routes triggers and
  never lays out menus.
- The ownership tree (Object, Location, Board, root) is combined by chained menus with one reserved
  direction: the object menu opens on the right button, its S slot is the Location, the Location's S
  is the Board, the Board's S is the camera menu. A menu of any count can hold S, so every small
  menu can still chain. A directional fallback, an outer provider filling the directions an inner
  one leaves free, is rejected: a three-option menu with wide sectors cannot host an outer
  provider's eight sectors, and the outer commands would change meaning with every stacking.
- The stack of objects under the pointer is not a menu hierarchy. Pointer routing picks the topmost
  object; the others are reached by reordering, never by a direction whose target depends on what
  lies below.

Conventions: S is the parent, NW is destructive, N is the primary action. Availability may vary with
state (the Timer's start pusher answers the left button with `run` while the timer is stopped and
with the `turn_off` sub-option of `running` while it runs), the meaning of a slot may not. Sub-menus opened from an interface's sub-options are leaves and reserve no S.

The generic layer provides only the slots that mean the same on every object. `ObjectToy::FillMenu`
(src/object.cpp) places the object's Runnable, or `kThisIsFine` while the object shows an error, at
N and its Location at S. The left button is not answered by the object toy and falls through to
`LocationWidget::FindOption` (src/location.cpp), which answers it with move.
`LocationWidget::FillMenu` places move at N, delete at NW, iconify or deiconify at NE, copy at E,
clone at W, make home at SE when the Location is not the object's home, and the Board at S.
`BoardWidget::FillMenu` (src/board.cpp) places move at N, toggle frame at NE and the camera menu at
S.
`RootWidget::FindOption` (src/root_widget.cpp) maps the W, A, S and D keys to the `Camera` object's
nudge commands and the middle button to the camera drag; `RootWidget::FillMenu` fills the same
camera menu that the Board's S slot opens. An object with more commands overrides `FillMenu`, calls
the base and places each command by hand (`TimerWidget` in src/library_timer.cpp); nothing is placed
by enumeration order, because adding an interface would then shift every direction after it.
