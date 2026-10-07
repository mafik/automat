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
Toy*)` returns the `Action` that the activation becomes. `activate` receives a `Span<Interface>`,
the chain of interfaces from the first menu bubble to the activated option, for example (Macro
Recorder) → (Location) → (Board) → (Board, toggle frame) → (Board, Sync); the last element is the
option itself, and an activation outside a menu passes a chain of one element. Objects expose their
commands as interfaces, and the widgets only map triggers to those interfaces:

- A command is a `Command` interface (src/command.hpp): `DEF_INTERFACE(X, Command, name, "Label")`
  with `OnRun`. It has no state, and a Gear can synchronize it. Its default `fill_menu`
  (`Command::Table::CommandFillMenu`, src/command.cpp) places Run at N and, when the Command is in
  the table of contents of its object, Sync at E, the option that starts the drag of a belt
  (docs/parrots/Hooks.md). The default activation clears the object's own Error and schedules a
  `RunTask` (`Command::Table::CommandActivate`, src/command.cpp),
  so that a Command that works for a long time never blocks the UI thread (an Instruction's `run`
  executes machine code until the code exits, src/library_instruction.hpp); a command whose
  activation is a gesture overrides it with `OnActivate` and has no `OnRun` (`kSplice` in
  src/library_timeline.cpp). `Command::Run` returns the next Command. `RunCascade`
  (src/command.cpp) runs a Command and then the returned Commands one after another until one
  returns nothing. `RunTask::OnExecute` (src/tasks.cpp) runs a scheduled Command through it, so a
  chain of synchronous commands runs within one task. When `OnRun` returns nothing, the generated
  `on_run` returns the target of the argument at `NextIdx()` (`Command::NextCommand`) unless the
  object reports an Error after the run. When `OnRun` returns a `Locked<Command>`, that value is
  the next Command: a command that works in the background returns an empty one and calls `Done()`
  when its work ends, which schedules the next Command; it calls `Done()` on the Command found at
  its index in the current table of contents, so the hooks of the object see the completion
  (docs/parrots/Hooks.md). `CancelIdx()` names the Command that
  cancels it. Both indices are optional, and 0 means none, because an argument or a command is
  never the object itself. Other objects reach a `Command` through their `next` argument.
- A part of an object that is either on or off is an `OnOff` (src/on_off.hpp). An OnOff is a
  Command: `OnOff::Table` extends `Command::Table` with `is_on`, `turn_on_idx`, `turn_off_idx` and
  `syncable_idx`, the interface index of the `Syncable` interface that a Gear notifies.
  Its `on_run` (`OnOff::Table::DefaultOnRun`, src/on_off.cpp) does not change the object. It
  returns the Command at `turn_off_idx` while the object is on and the Command at `turn_on_idx`
  while it is off, so running an OnOff toggles the object through that Command, and the run
  continues with the next Command of that Command. `Toggle` runs the OnOff through `RunCascade`,
  and `TurnOn` and `TurnOff` run the Command at the matching index the same way, on the calling
  thread. Their callers are an object's own Commands (the Switch's `flip`), the loading of saved
  state and the OnOff track of a Timeline. `NotifyTurnedOn` and `NotifyTurnedOff` tell the synced
  partners about a change that the object made on its own (docs/parrots/Hooks.md, section
  "Notifications"). The activation of an OnOff is the default activation of
  a Command: it clears the object's own Error and schedules a `RunTask`, which toggles the object on
  a worker thread. The Commands at `turn_on_idx` and `turn_off_idx` wake the object's toys when
  they change its state, so a toy shows the new state on its next tick. An OnOff is not an
  Argument. The menu of an OnOff holds Toggle at N, Sync at E, the Command at `turn_on_idx` at W
  and the Command at `turn_off_idx` at S; those two open their own Command menus. A Gear
  synchronizes an OnOff through those two Commands (docs/parrots/Hooks.md).
- A command that runs for a while and shows it (Timer, Timeline, Program Launcher, the GStreamer
  elements, Assembler) declares an `OnOff` named `running` over its `run` and `cancel` Commands
  (src/library_timer.hpp). Each of them keeps its own running flag, except the Assembler. The
  Assembler's machine code controller already records whether machine code is executing
  (`mc::Controller::IsRunning`, src/machine_code.hpp), and a second flag could disagree with it.
  The OnOff is the part that shows the running state, that starts and stops the object from one
  button and that becomes a controller.
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
- Sub-options of an interface are other interfaces of the same object, named by interface index
  (`Object::InterfaceFromIdx`, src/object.hpp). An `OnOff::Table` holds `turn_on_idx` and
  `turn_off_idx`, the interface indices of two Commands that the object's class declares next to its
  OnOff (HotKey's `enable` and `disable`, src/library_hotkey.hpp), so each class names its own on
  and off commands. A swipe onto one of these Commands in a menu enters its sub-menu, which holds
  its Sync option, like a swipe onto any other Command of the object's table of contents. The
  indices are computed at compile time by `SlotOf`, which `INTERFACES()` defines in the class
  (src/interface.hpp), so the tables stay constant-initialized; `OnOffTrack`, `Instruction` and
  `GStreamerElement`, whose tables of contents are built at run time, state their slots as
  constants. The interface lays its sub-options out itself: `fill_menu` in its table places them in
  the menu. A derived table calls the filler of its base first and then places its own. The OnOff
  table (src/on_off.hpp) calls the filler of the Command table and replaces Run with Toggle.
  Run, Toggle and Sync are global tables in `global_toc` (`kRun`, `kToggle`, `kSync`); on
  activation they act on their caller, the element before them in the chain. Run and Toggle
  schedule the caller; Sync starts the drag of a new belt for it.
  The primary action stays in `activate` (an OnOff toggles); an interface without `activate` opens
  its sub-options when activated.
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
std::unique_ptr<Action> OpenMenu(ui::Pointer&, Vec<Locked<>> chain);
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

Each walker applies its own rules to the option it found. While the `control` Switch of
`RootWidget` (src/root_widget.hpp) is on, every walker first tries
`Interface::DragNewController(pointer, birthplace)` (src/interface.cpp): an option with a controller
(`Interface::MakeController`, a `LinkedSwitch` for every OnOff) is wrapped in one, whose toy
is born under `birthplace` and handed to `DragNew`, the same drag that springs a new object out of
a toolbar or a menu slot; an option without a controller falls through. The birthplace is the
widget that answered `FindOption` for a button or key, and the slot icon for a swipe. A swipe
(`MenuWidget::Activate`, src/menu.cpp) then enters the option's sub-options when it has any.
Otherwise the primary action runs (`Activate(chain, pointer, toy)`, src/interface.cpp). Buttons and keys act,
directions enter: the same OnOff toggles from its power button, opens its sub-menu from a slot of
the object menu, and becomes a controller from either while the `control` Switch is on.
`RootWidget::control_gear` (src/root_widget.cpp) is a Gear with two members: the `state` OnOff of
the KeyPresser `left_ctrl` as a source and the `enabled` OnOff of the Switch as a sink. Attaching
`state` starts the monitoring of the left Control key through the Syncable of the KeyPresser. The key
logger of the KeyPresser reports a press with `NotifyTurnedOn`, which turns the Switch on
(docs/parrots/Hooks.md, section "Notifications").

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
a slot replaces that slot, and placing an empty interface removes the slot at the angle. The same
interface may be placed at several angles. Nothing declares a shape: the count and the orientation of a menu follow from the
placements, so any count and any set of angles is a valid menu.

`OptionsProvider::OpenMenu` fills a `Menu` through the widget's `FillMenu`, and a swipe onto an
option fills one through the table's `fill_menu`; `MakeMenuAction(pointer, menu, toy, chain)` builds
the bubble (`MenuWidget` in src/menu.cpp) from it and returns nothing when the menu is empty. The
chain starts with the object whose toy opened the first menu, and every swipe into a sub-menu moves
it into the next bubble with the chosen option appended. The bubble stores each option, and the
chain, as `Locked<>` values, so their objects stay alive and the tables of retired hooks are not
deleted while the menu is open (docs/parrots/Hooks.md). Icons come
from `Interface::MakeIcon`. The icon of each slot sits at its angle, at two thirds of the
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
at S. Menus are not split into pages: the register menu of the Assembler (src/library_assembler.cpp)
places the eight general-purpose registers at the eight compass directions, clockwise from N, so
each register has a permanent direction.

A widget or table that builds on a base calls the filler of the base first and then places its own
slots, so a later placement is an override, visible where it happens: it takes over the angle from
the slot of the base, and it moves an interface that the base placed at another angle
(`InstructionLibrary::Widget` in src/library_instruction_library.hpp moves its `scroll` Command from
N to E and places `pick` at N). The base places only the slots that mean the same everywhere and
never needs to know the final shape, because the sectors follow from the final list.

A node that only groups other options is a table of kind `kMenu`, made by `MenuTable(name,
fill_menu)` (src/interface.hpp): it has `fill_menu` and a name, but no `activate`, no state and no
methods, so no cast accepts it and `MakeController` returns nothing for it. The Object type that
offers such a table lists it in `INTERFACES()`, so that each menu has a slot in the object's table
of contents (`Object::toc`, src/object.hpp) like the object's other interfaces: `Camera` lists the
camera menu (src/root_widget.cpp), `ClientWindow` lists the decoration menu (src/window_frame.cpp)
for the X11, Wayland and Windows window objects, `BetaShelf` lists the shelf categories
(src/library_beta_shelf.cpp), `Assembler` lists the register menu (src/library_assembler.cpp) and
`Mouse` lists the mouse menus (src/library_mouse.cpp). The name of a menu must differ from the names
of the object's other interfaces, because `Object::InterfaceFromName` returns the first match.

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
state (the object menu offers `kThisIsFine` at N while the object shows an error), the meaning of a
slot may not. Sub-menus opened from an interface's sub-options are leaves and reserve no S; the
Off Command of an OnOff menu at S is such a sub-option.

The generic layer provides only the slots that mean the same on every object. `ObjectToy::FillMenu`
(src/object.cpp) places the object's first Command, or `kThisIsFine` while the object shows an error, at
N and its Location at S. A class lists its main command before its other Commands in `INTERFACES()`,
because the first Command is also the one that `Object::Updated` schedules and the one that a `next`
cable dropped on the object connects to (`BoardWidget::ConnectAtPoint`, src/board.cpp). A class
with an OnOff lists the OnOff first, so the toggle is its main command (the `running` OnOff of
Timer, the `enabled` OnOff of HotKey); KeyPresser, which has two, lists `state` before
`monitoring`. The left button is not answered by the object toy and falls through to
`LocationWidget::FindOption` (src/location.cpp), which answers it with move.
`LocationWidget::FillMenu` places move at N, delete at NW, iconify or deiconify at NE, copy at E,
clone at W, make home at SE when the Location is not the object's home, and the Board at S.
`BoardWidget::FillMenu` (src/board.cpp) places move at N, toggle frame at NE and the camera menu at
S.
`RootWidget::FindOption` (src/root_widget.cpp) maps the W, A, S and D keys to the `Camera` object's
nudge commands and the middle button to the camera drag; `RootWidget::FillMenu` fills the same
camera menu that the Board's S slot opens. An object with more commands overrides `FillMenu`, calls
the base and places each command by hand (`TimerWidget` in src/library_timer.cpp); nothing is placed
by enumeration order, because adding an interface would then shift every direction after it. A toy
may place the object's first Command at another angle as well, in addition to N: the Switch toy
places its OnOff at E, the Assembler, Program Launcher, Window, Mouse Button Presser and Key Presser
toys place theirs at NE, and the PipeWire node toy places `muted` at W. A toy does not repeat a
placement of the base, so the HotKey, Macro Recorder, File and Sources toys place nothing of their
own.
