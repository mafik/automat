# Multi-Window Architecture Plan

Goal: support opening multiple windows (each owning its own `RootWidget` and
UI-side threads) that interact with the same VM objects. Ownership and
lifetimes should be properly nested.

## Starting point (facts verified by reading the code)

**Global state is already an explicit, short list:**
- `src/automat.hh:13-17` — `stop_source`, `root_location`, `root_board`, `main_thread_id`.
- `src/root_widget.hh:36-37` — `std::vector<RootWidget*> root_widgets` (the vector already exists) and the legacy `unique_ptr<RootWidget> root_widget`.
- `src/xcb.hh:15` — `xcb_connection_t* connection`, plus `screen`. XCB natively supports many windows per connection.
- `src/touchpad.hh:57-58` — `touchpads_mutex`, `touchpads`.
- `src/vk.hh:19-20` — `initialized`, `graphite_context`, `background_context`.
- Inside `src/vk.cc`: a single file-scope `swapchain`, `surface`, `graphite_recorder`, `command_pool`. `Init/Resize/AcquireCanvas/Present` operate on these singletons (`vk.cc:1112,1172,1184,1186,1149`). `graphite::Context` is fine to share; `Surface`+`Swapchain` must become per-window.

**Lifetime already deterministic.** `automat::Main()` in `src/automat.cc:47-125`
initialises subsystems in explicit order and tears them down in reverse. No
`App` struct is needed — `automat.hh` is the manifest, `Main()` is the owning
scope.

**Already per-window on `RootWidget`:** `render_thread`, `window`, `toys` (ToyStore), `pointers`, `keyboard`, camera, `active_actions`, `loading_animation`, `toolbar`, `zoom_warning`, `black_hole` (see `src/root_widget.hh:43-184`). The render thread is created in `RootWidget::Init()` at `src/root_widget.cc:165`.

**Known bugs surfaced by this audit (present even today):**

1. `src/root_widget.cc:377-383` — `RootWidget::Tick` iterates **all** `root_widgets` updating all pointers. With two windows each window does everyone's work.
2. `src/root_widget.cc:119-121` — `image_provider->TickCache()` runs inside every render thread. Race with ≥2 windows.
3. `src/root_widget.cc:247-255` — every `RootWidget::Tick` consumes `touchpad::touchpads` pan/zoom (resets to 0). With two windows, whoever ticks first wins.
4. `src/location.cc:108-109` — `Location::InsertHere` directly calls `ui::root_widget->WakeAnimation()`. The code itself has a `TODO` comment saying this is wrong: VM should never mutate UI directly.
5. `src/location.cc:442` — `Location::InvalidateConnectionWidgets` guards its body with `if (!ui::root_widget || !object)` as an "is UI up" probe. Body is clean; guard is a cross-layer leak.
6. `src/argument.cc:44` — `Argument::ObjectOrMake` (VM code) calls `ui::root_widget->WakeAnimation()`. Redundant once site 4 is fixed: `Board::Create` → `Location::InsertHere` does the notify.

**VM → UI protocol (the only one allowed).** Objects have
`AtomicCounter wake_counter` at `src/object.hh:37`. VM code bumps it via
`WakeToys()` (`src/object.hh:47`, `src/argument.hh:198`). UI-side `ToyStore::Tick`
(`src/toy.cc:32-60`) polls counters each frame and calls `WakeAnimationAt` on
affected Toys. The offenders in sites 4-6 bypass this protocol; fix is to
remove the direct UI calls and lean on the existing counter mechanism.

**Input → UI is a separate, legitimate wake direction.** Touchpad HID thread
(`src/touchpad.cc:226`) writes to the shared `TouchPad` struct and must break
the UI's powersave sleep (`src/root_widget.cc:59-80`). This is not a VM → UI
violation; it's an input producer nudging a consumer. It still needs to be
made multi-window-aware.

**Main-loop shape.** `Main()` blocks in `root_widget->window->MainLoop(stop_token)`
at `src/automat.cc:91`. `MainLoop` is a virtual on `Window` (`src/window.hh:28`).
XCB version (`src/xcb_window.cc:615`) uses the shared `xcb::connection`;
events carry the target `xcb_window_t` so per-window dispatch from a single
connection-level loop is natural. Win32 uses per-HWND WndProc via
`GWLP_USERDATA`. A single main-loop can drive many windows on both platforms.

**Singleton `ui::root_widget` reads (~25 call sites).** Categories:

- *UI-thread sites that already have a RootWidget in scope* (Pointer has `root_widget&`, Widget has `FindRootWidget()`): `drag_action.cc:147,166,194,199`, `loading_animation.cc:58,59,103`, `library_window.cc:735`, `library_key_presser.cc:30`, `library_macro_recorder.cc:111`, `location.cc:77,80,154`, `system_tray.cc:188,195`, and so on. Mechanical rewrite.
- *VM-thread sites* (three): bugs — see the six-item list above.
- *Input-thread sites* (one, touchpad): needs multi-window routing, not a singleton.

## Plan

Each step is independently landable. Order matters — later steps depend on
earlier ones.

### Step 1 — Fix cross-window logic already in the code
One-line fix in `RootWidget::Tick` at `src/root_widget.cc:378-382`: iterate only
`this->pointers`, not all `root_widgets`.

### Step 2 — Remove VM → UI `WakeAnimation` calls (fix existing bugs)
Not "wake every window" — these are protocol violations that should use the
existing `wake_counter` path.

- `src/location.cc:109` → replace with `object->WakeToys()`. Delete the `TODO` comment at line 108.
- `src/location.cc:442` → drop the `!ui::root_widget` half of the guard; keep `!object`.
- `src/argument.cc:44` → delete. The wake is issued transitively by `Location::InsertHere` (after site 2 is fixed).

Audit pass: grep for `WakeAnimation` in files that run on VM threads
(`library_*.cc`, `tasks.cc`, `sync.cc`, `object.cc`, `board.cc`,
`persistence.cc`, `argument.cc`, `location.cc`) and confirm no further
VM-side `WakeAnimation` calls exist. (Current audit says clean apart from
sites 4-6.)

### Step 2b — Multi-window touchpad wake
`src/touchpad.cc:226` currently reaches through the singleton. It's an
input → UI wake, legitimate but singleton-coupled. Short-term: iterate
`ui::root_widgets` and wake each. Final: route to the focused window (see
step 5).

### Step 3 — Route UI-thread `root_widget` uses through local context
Replace every singleton read on the UI side with `Widget::FindRootWidget()`
or an added parameter. Files: `drag_action.cc`, `loading_animation.cc`,
`library_window.cc`, `library_key_presser.cc`, `library_macro_recorder.cc`,
`location.cc:77,80,154`, `system_tray.cc`. After this step the only remaining
uses of `ui::root_widget` are `automat.cc` and `system_tray.cc`'s
minimize/restore paths (the tray commands are inherently ambiguous with
multiple windows and will be rethought in step 10).

### Step 4 — Move `image_provider->TickCache()` out of the render thread
Call it once per frame from `RootWidget::Tick` or from the main loop.
One-site change; eliminates the only known race under multi-window
(`src/root_widget.cc:119-121`).

### Step 5 — Focused-window tracking
Add `ui::focused_root: RootWidget*` (updated on OS focus events, platform
layer notifies it). Only the focused RootWidget consumes the shared
`touchpads` state. Before multi-window ships this is a behavioural no-op.
Step 2b can be tightened at this point: wake only the focused window.

### Step 6 — Per-window Vulkan surface + swapchain (biggest change)
Move `surface`, `swapchain`, per-image semaphores, offscreen backbuffers,
and the per-frame state currently at file scope in `src/vk.cc` into a
`struct vk::WindowContext` owned by each `Window` (candidate: field on
`Window`, or on `RootWidget`). Keep `graphite_context` and
`background_context` shared.

Convert `vk::Init/Resize/AcquireCanvas/Present` from free functions on
globals into methods on `WindowContext`. Update:

- `src/root_widget.cc:82-107` (`VulkanPaint`)
- `src/renderer.cc:1468` (`vk::Present`)
- `src/renderer_test.cc:61,175`

Audit: grep `vk::` across `src/`. Callers that do not hold a specific
window (e.g. the background recorder in `src/textures.cc:112-148`) should
continue to use the shared `graphite_context`.

### Step 7 — Single main loop, multiple windows
Replace the `Window::MainLoop` virtual with platform-level functions:

- `xcb::MainLoop(stop_token)` — loops on `xcb::connection`, dispatches events to the `XCBWindow*` matching `event->window`.
- `win32::MainLoop(stop_token)` — standard message pump; WndProc already routes via `GWLP_USERDATA`.

`automat::Main()` calls the platform `MainLoop` and exits when stop is
requested and all windows are closed.

### Step 8 — `ui::root_widget` unique_ptr → `ui::root_widgets` vector of unique_ptrs
`root_widgets` already tracks raw pointers (`src/root_widget.hh:36`); change
the global to own the RootWidgets. Add:

```cpp
namespace automat::ui {
RootWidget& OpenWindow();           // constructs, Init()s, adds to root_widgets
void CloseWindow(RootWidget&);      // reverses above
}
```

In `Main()` the first call opens the initial window; shutdown iterates and
destroys in reverse. Delete the `ui::root_widget` global. `system_tray.cc`
and remaining `automat.cc` references become iterations.

### Step 9 — Multi-window `ToyStore::FindOrMake` semantics
`ToyMakerMixin::ForEachToyImpl` already iterates `root_widgets`
(`src/toy.cc:22-30`) so VM-side state updates fan out correctly. The UI-side
creation sites must target the `RootWidget` they are running in, not an
arbitrary one. Audit every `root_widget->toys.FindOrMake(...)` /
`root_widget->toys.FindOrNull(...)`:

- `src/root_widget.cc:393-470` (Tick's children build)
- `src/pointer.cc:250`
- `src/object.cc:149`
- `src/ui_connection_widget.cc:536`
- `src/sync.cc:567,580-600`
- `src/library_toolbar.cc:35`
- `src/library_mouse.cc:114`
- `src/library_macro_recorder.cc:393`
- `src/text_field.cc:143`

### Step 10 — Surface "New Window" to the user
Toolbar button / menu item / Object that calls `ui::OpenWindow()`. Revisit
the tray commands (`src/system_tray.cc:188,195`) for the "minimize the
(ambiguous) window" question.

### Step 11 — Multi-window persistence
`RootWidget::SerializeState`/`DeserializeState`
(`src/root_widget.cc:682-774`) is already per-RootWidget. Adjust the
top-level persistence layer (`src/persistence.cc`) to save/restore a list
of windows rather than one.

## Dependencies between steps

- Steps 1, 2, 2b, 3, 4 are independent hygiene fixes — any order, any subset.
- Step 5 should land before step 2b is tightened.
- Step 6 is the structural change everything else builds on.
- Step 7 depends on step 6 (each window needs its own Vulkan surface before the event loop can be shared).
- Step 8 depends on step 7 (opening a second window is only possible once the main loop can drive it).
- Steps 9-11 are polish after the structural work lands.

## Notes for resuming

- Branch: `claude/multi-window-architecture-plan-XNV8s`.
- Previous commit on this branch: `CLAUDE.md: forbid sub-agents for codebase exploration`.
- Do **not** use sub-agents for reading this codebase (see CLAUDE.md).
- The authoritative global-state manifests are `src/automat.hh` and `src/root_widget.hh`; `automat::Main()` in `src/automat.cc` is the owning scope.
