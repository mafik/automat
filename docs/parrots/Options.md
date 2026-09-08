# Options

## Problem

Every way of interacting with a widget used to be described twice. `VisitOptions` listed the
commands shown in the bubble menu, and `FindAction` decided what a button or key did when it was
pressed over the widget. The cursor was a third mechanism: widgets installed `CursorOverride`s
from their hover hooks. Keeping the three in agreement was manual work, and the menu could not know
which of its commands were also reachable with a button or a key.

## Solution

One virtual method describes all of it (`OptionsProvider` in `src/widget.hpp`, `Option` in
`src/menu.hpp`):

```cpp
virtual void Options(ui::Pointer&, OptionVisitor&);
```

A widget reports its own options, most important first, by calling the visitor with each option.
The visitor's callback returns `LoopControl::Break` when the consumer has what it needs; the visitor
then ignores every further option, so producers do not have to check a result. Groups of options are
options that are themselves `OptionsProvider`s, so the tree structure reaches the menu unchanged.

An `Option` is described by virtual methods only:

- `Triggers()` returns the buttons and keys that activate it directly. Menu-only commands return an
  empty span. An option that acts on several buttons is reported once per button, so `Activate`
  never has to ask which trigger fired.
- `Cursor()` is the cursor shown while this is the first option that the left button would
  activate. `Cursor::None` (the default) leaves the cursor alone.
- `Activate(Pointer&)` performs the option and returns the `Action` that represents it. A null
  result means that nothing happened, and the search continues with the next option. Options that
  act instantly return an `EmptyAction`.
- `MakeIcon()` returns the widget that represents the option in a menu. The menu keeps clones of
  the options (`OptionsProvider::OpenMenu`) and lists all of them; gestures use `TextOption` with a
  short label. An option that returns no icon is reported as an error and gets an empty slot.

Options are `ReferenceCounted`, so a widget may keep long-lived options as `Ptr` members and report
them each time, or construct them on the stack inside `Options()`. Menu commands hold `WeakPtr`s to
the objects they act on because their clones outlive the call.

## Consumers

- `OptionsProvider::FindAction(Pointer&, ActionTrigger)` activates the first option of one widget
  that the trigger matches and that returns an action. `Pointer::ButtonDown` and
  `KeyboardWidget::KeyDown` call it on the hovered widget and then on each parent, so the deepest
  widget wins.
- When no option claims the right button, `Pointer::ButtonDown` opens the bubble menu of the nearest
  widget, from the hovered one upwards, that has an option to clone. Widgets no longer open
  menus themselves.
- `Pointer::cursor` is recomputed by `Pointer::UpdatePath` and whenever a `CursorOverride` is
  installed or removed: the most recently installed `CursorOverride` if there is one, else the
  `Cursor()` of the first left-button option found walking from the hovered widget upwards, else the
  arrow. A change is reported to the platform layer through `OnCursorChanged`. `CursorOverride`s
  remain for cursors that are not tied to an option, such as the shape requested by a Wayland
  client, the crosshair of a global grab, and the cursor that an `Action` shows while it runs.
- The bubble menu (`src/menu.cpp`) keeps the icon widget and the position spring of every option in
  its own slot arrays, so an `Option` carries no display state.

## Rules for `Options()`

`Options()` runs on every pointer move and inside `RootWidget::Tick`, so it must only describe the
current state. It must not mutate the widget or the object, take object mutexes, create widgets or
wake animations. The pointer position is used only inside `Options()`: a widget with several
pointer-sensitive parts hit-tests there and reports the option of the part under the pointer, with
the part's parameters stored in the option. `Activate` never looks at the pointer position, because
the pointer may have moved while the menu was open; it only performs the option and may re-check
that the object still exists.
