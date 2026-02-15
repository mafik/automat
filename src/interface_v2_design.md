# Interface v2 Design Notes

## Motivation

The current static-interface design (post LLVM-RTTI migration) works well for memory layout
but has three pain points:

1. **Boilerplate**: Each stateful interface on an Object requires coordinated pieces: a SyncState
   member, a static Interface with function table, get_sync_state lambdas, Interfaces() visitor
   entry, and sometimes destructor cleanup.
2. **Verbose usage**: Interface functions need explicit `Object&` and `Interface&` arguments
   threaded through everywhere.
3. **State destruction**: SyncState destructor needs Object& and Syncable& but has neither
   (see TODO in sync.hh ~SyncState).

## Core Idea

Each interface defines its required functions as a **concept**. Objects implement the concept
as a small nested type that **inherits the bound type** (e.g., `OnOff`, `Runnable`). The
impl type is never stored — it's constructed transiently from `(Object*, Table*)` when an
interface method is called. Per-instance state lives in `Iface::Def<T>`, found via a
`state_off` field in the Table.

### Type architecture

Each interface has three facets:

- **`Iface::Table`** — the static function table. Stores RTTI kind, `state_off` (offset from
  Object to Def within the concrete object), name, and function pointers. Table types form
  a C++ inheritance hierarchy. LLVM-style RTTI (`classof`, `isa`, `dyn_cast`) operates on
  `Table*`.

- **`Iface`** (top-level) — the lightweight bound type. Just `Object* + Table*`. This is
  the **primary way** to use an interface. It provides the public API methods (`TurnOn`,
  `IsOn`, `Toggle`, etc.) that handle sync forwarding where needed. Bound type methods
  can access per-instance state via `(State*)((char*)obj + table->state_off)`.

- **`Iface::Def<ImplT>`** — per-instance storage. Contains `Iface::State` (SyncState etc.)
  and a `static constexpr Table` with function pointers wired to ImplT. Declared as a
  member of the Object. The `Def` does NOT inherit ImplT.

```
Table hierarchy (inheritance):       Bound types (top-level names):

Interface::Table                     Interface { obj, table }
    |                                    |
Argument::Table                      Argument { obj, table }
    |                                    |
Syncable::Table                      Syncable { obj, table }
   / \                                 / \
OnOff::Table  Runnable::Table      OnOff    Runnable
```

### ImplT — the transient impl type

ImplT inherits the bound type (e.g., `struct Enabled : OnOff`). This gives it:
- `obj` pointer (from the bound type base) — the Object this interface belongs to
- `table` pointer (from the bound type base) — the static function table
- All public API methods of the bound type

ImplT is **never stored**. It's constructed on the stack in table lambdas from the
`(Object*, Table*)` pair that the lambda receives, then destroyed immediately after.
Zero per-instance overhead.

ImplT provides:
- `using Parent = ConcreteObjectType;` — declares the concrete parent type
- `static constexpr int Offset()` — returns `offsetof(Parent, field_name)` (deferred
  evaluation — works because method bodies are deferred until the enclosing class is complete)
- `static constexpr StrView kName` — the interface's display name
- Impl methods (`OnTurnOn()`, `OnRun()`, `IsOn()`, etc.)

### Typed parent access via `self()`

ImplT methods access their parent Object through a `self()` method on the bound type base.
This uses C++23 deducing `this` to extract ImplT's `Parent` typedef:

```cpp
struct Interface {
  Object* obj;
  const Table* table;

  // Typed access to parent — deduces Parent from the most-derived type (ImplT)
  auto& self(this auto& iface) {
    return static_cast<
        match_const_t<std::remove_reference_t<decltype(iface)>,
                       typename std::remove_reference_t<decltype(iface)>::Parent>
    &>(*iface.obj);
  }
};
```

When called on an `Enabled` (which defines `using Parent = FlipFlop;`), deducing `this`
resolves `decltype(iface)` to `Enabled&`, so `self()` returns `FlipFlop&`. Type-safe,
no manual casting needed in impl methods.

Requires a small helper (defined once, globally):
```cpp
template <typename From, typename To>
using match_const_t = std::conditional_t<std::is_const_v<From>, const To, To>;
```

### Naming conventions

**Nested impl types**: The interface name is omitted when unambiguous, or appended
for disambiguation. Examples: `Toggle` (for a Runnable), `Duration` (for a Syncable),
`Running` (for a LongRunning). Avoid naming a nested type `Def` since it shadows
`Iface::Def<T>`. For on/off state, use `Enabled`, `Active`, or `StateOnOff`.

**Impl methods vs public API**: Methods that need wrapping (sync forwarding, etc.)
use the `On` prefix in the impl (`OnTurnOn`, `OnRun`). The top-level bound exposes
the clean public API without the prefix (`TurnOn`, `Run`). Simple methods that need
no wrapping (like `IsOn`) keep the same name in both layers — no `On` prefix needed.
This signals to callers that no sync/forwarding magic happens between bound and impl.

| Layer | Naming | Example | Responsibility |
|-------|--------|---------|---------------|
| Impl method | `OnX` or `X` | `OnTurnOn()`, `IsOn()` | Raw behavior for one object |
| Bound method | `X` | `TurnOn()`, `IsOn()`, `Toggle()` | Sync forwarding where needed |

**Design philosophy**: Most logic should live on the Object itself. Interfaces are thin
accessors — their impl methods should be short wrappers that call Object-level functions.

### What the Object author writes (the bare minimum):

```cpp
struct FlipFlop : Object {
  bool current_state = false;

  // Object-level logic
  void Toggle() {
    current_state = !current_state;
    WakeToys();
  }

  struct Enabled : OnOff {
    using Parent = FlipFlop;
    static constexpr StrView kName = "State"sv;
    static constexpr int Offset() { return offsetof(FlipFlop, on_off); }

    bool IsOn() const { return self().current_state; }
    void OnTurnOn() { self().current_state = true; self().WakeToys(); }
    void OnTurnOff() { self().current_state = false; self().WakeToys(); }
  };
  OnOff::Def<Enabled> on_off;

  struct ToggleRunnable : Runnable {
    using Parent = FlipFlop;
    static constexpr StrView kName = "Flip"sv;
    static constexpr int Offset() { return offsetof(FlipFlop, flip); }

    void OnRun(std::unique_ptr<RunTask>&) {
      self().Toggle();
    }
  };
  Runnable::Def<ToggleRunnable> flip;

  INTERFACES(on_off, flip)
  // ... Name, Clone, MakeToy, etc.
};
```

### What gets auto-generated:

- **Interface iteration** (from INTERFACES macro)
- **Static table** (`Def<ImplT>::table` with all function pointers)
- **State destruction** (`Def<ImplT>::~Def()` navigates to parent via `state_off` and calls Unsync)

## Key Components

### Interface::Table — the function table hierarchy

Table types use plain C++ inheritance. Each level adds its function pointers.
LLVM-style RTTI (`Kind`, `classof`) lives here. The `state_off` field occupies
what was previously padding between `kind` (4 bytes) and `name` (16 bytes, aligned
to 8) — zero additional memory cost.

```cpp
struct Interface {
  Object* obj;
  const Table* table;

  struct Table {
    enum Kind { kArgument, kNextArg, kSyncable, kOnOff, kLongRunning,
                kLastOnOff = kLongRunning, kRunnable, kLastArgument = kRunnable,
                kImageProvider };
    Kind kind;
    int state_off;  // offset from Object* to Def within the concrete object
    StrView name;
    constexpr Table(Kind kind, int state_off, StrView name)
        : kind(kind), state_off(state_off), name(name) {}
  };

  // Typed parent access — deduces Parent from ImplT via deducing this
  auto& self(this auto& iface) {
    using Self = std::remove_reference_t<decltype(iface)>;
    return static_cast<match_const_t<Self, typename Self::Parent>&>(*iface.obj);
  }

  // Navigate to per-instance state
  template <typename StateT>
  StateT& state() const {
    return *reinterpret_cast<StateT*>(
        reinterpret_cast<char*>(obj) + table->state_off);
  }

  Interface(Object& obj, const Table& table) : obj(&obj), table(&table) {}
  StrView Name() const { return table->name; }
};

struct Argument : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* t) { ... }

    void (*can_connect)(...) = nullptr;
    void (*on_connect)(...) = nullptr;
    NestedPtr<Interface::Table> (*find)(...) = nullptr;
    // ... style, tint, etc.
  };
};

struct OnOff : Syncable {
  struct Table : Syncable::Table {
    static bool classof(const Interface::Table* t) { ... }

    bool (*is_on)(OnOff&) = nullptr;
    void (*on_turn_on)(OnOff&) = nullptr;
    void (*on_turn_off)(OnOff&) = nullptr;
  };

  // Public API
  bool IsOn() { return tbl().is_on(*this); }
  void TurnOn()  { /* sync forwarding + */ tbl().on_turn_on(*this); }
  void TurnOff() { /* sync forwarding + */ tbl().on_turn_off(*this); }
  void Toggle()  { IsOn() ? TurnOff() : TurnOn(); }
};
```

Note that table function pointers now take `OnOff&` (the bound type) instead of
separate `(Table&, Object&)` arguments. The bound type carries both.

### Def<ImplT> template

Each interface type defines a `Def` template that:

1. **Stores per-instance state** (SyncState for Syncable hierarchy, NextState for NextArg, etc.)
2. **Has a `static constexpr Table`** with function pointers wired to ImplT
3. **Has a destructor** that handles cleanup (e.g., Unsync)

`Def` does NOT inherit ImplT. ImplT is only used as a template argument to extract
types and generate the static table.

```cpp
template <typename ImplT>
struct OnOff::Def {
  // Per-instance state
  SyncState sync_state;

  // Static function table — one per ImplT instantiation
  static constexpr OnOff::Table table{
    OnOff::Table::kOnOff,
    ImplT::Offset(),
    ImplT::kName,
    // Function pointers construct ImplT transiently
    .is_on = +[](OnOff& iface) -> bool {
      return static_cast<ImplT&>(iface).IsOn();
    },
    .on_turn_on = +[](OnOff& iface) {
      static_cast<ImplT&>(iface).OnTurnOn();
    },
    .on_turn_off = +[](OnOff& iface) {
      static_cast<ImplT&>(iface).OnTurnOff();
    },
  };

  ~Def() {
    // Navigate Def → parent Object via state_off, then Unsync
    auto* obj = reinterpret_cast<Object*>(
        reinterpret_cast<char*>(this) - table.state_off);
    Syncable(*obj, table).Unsync();
  }
};
```

The `static_cast<ImplT&>(iface)` is valid because:
- `ImplT` inherits `OnOff` and adds no data members (it's a transient view type)
- The lambda receives `OnOff&` which is the base subobject of a conceptual `ImplT`
- Since `ImplT` has no additional state, the cast is a no-op at runtime

Each template instantiation gets its own unique static `table` with stable address.
Works as ToyStore key, serialization ID, etc.

### INTERFACES macro

```cpp
namespace detail {
inline void VisitIfaces(const std::function<LoopControl(Interface::Table&)>&) {}

template <typename First, typename... Rest>
void VisitIfaces(const std::function<LoopControl(Interface::Table&)>& cb,
                 First& first, Rest&... rest) {
  if (cb(std::remove_reference_t<First>::table) == LoopControl::Break) return;
  VisitIfaces(cb, rest...);
}
}  // namespace detail

#define INTERFACES(...)                                                              \
  void Interfaces(const std::function<LoopControl(Interface::Table&)>& cb) override {\
    ::automat::detail::VisitIfaces(cb, __VA_ARGS__);                                 \
  }
```

### Bound type state access

Bound type methods can access per-instance state through `state_off`:

```cpp
struct Syncable : Argument {
  // Access the SyncState for this interface instance
  SyncState& sync_state() const {
    return *reinterpret_cast<SyncState*>(
        reinterpret_cast<char*>(obj) + table->state_off);
  }

  void Unsync() { /* uses sync_state() */ }
};
```

This replaces the old `get_sync_state` function pointer — the offset is enough.

## Design Properties

### Solves Issue 1 (boilerplate)
Implementing an interface: define impl type inheriting the bound type (with Parent,
Offset, kName, and methods), add `Def<Impl>` member, list in INTERFACES. That's it.
No separate SyncState, no static definition in .cc, no get_sync_state lambda.

### Solves Issue 2 (verbose usage)
Impl methods access the parent via `self()` — no explicit arguments needed.
Call sites use the bound type directly: `OnOff(self, table).Toggle()`.
The top-level names (`OnOff`, `Runnable`, etc.) are the primary API.

### Solves Issue 3 (state destruction)
`Def<ImplT>::~Def()` navigates to the parent Object using `state_off` (which is
known from the static table) and calls cleanup. No STATE_OF macro needed.

### Memory layout
Stateful interfaces (Syncable hierarchy, NextArg, LongRunning) store per-instance state
(SyncState, NextState, etc.) inside Def<ImplT> — same footprint as today.

Stateless interfaces (plain Argument, ImageProvider) produce an empty Def<ImplT>.
Declaring the member with `[[no_unique_address]]` eliminates it from the struct layout:

```cpp
struct Window : Object {
  // Stateful — occupies sizeof(SyncState) bytes
  OnOff::Def<Enabled> on_off;

  // Stateless — zero bytes (empty Def<T> + [[no_unique_address]])
  [[no_unique_address]] Argument::Def<Source> source;
  [[no_unique_address]] ImageProvider::Def<Preview> preview;

  INTERFACES(on_off, source, preview)
};
```

Which interfaces are stateful vs stateless:
- **Stateful**: Syncable (+SyncState), OnOff (+SyncState), Runnable (+SyncState),
  LongRunning (+SyncState + RunTask ref), NextArg (+NextState)
- **Stateless**: Argument, ImageProvider — their Def<T> adds no members

### state_off — free navigation

The `state_off` field in `Interface::Table` fits in the 4-byte padding gap between
`Kind` (4 bytes) and `StrView` (16 bytes, 8-byte aligned). Zero additional memory.

It enables any code holding a bound type (`Object* + Table*`) to find the per-instance
state without knowing the concrete Object type. This replaces both:
- `get_sync_state` function pointer (Syncable hierarchy)
- `STATE_OF` / `PARENT_REF` macros for parent navigation

### ToyStore compatibility
`Def<ImplT>::table` has stable unique address per template instantiation. Works as
ToyStore key and serialization identity (uses `Interface::Table*`).

### RTTI
LLVM-style RTTI operates on `Interface::Table*`:
```cpp
if (auto* on_off_table = dyn_cast<OnOff::Table>(&some_table)) { ... }
```

## Refinements to Consider

1. **Optional concept methods**: Use `if constexpr (requires { ... })` in the table
   initializer to conditionally populate nullable function pointers.

2. **Interface customization** (tint, autoconnect_radius, etc.): Impl provides a static
   `Configure(Table&)` method, called conditionally via `if constexpr`.

3. **Flat Def per leaf type**: OnOff::Def sets ALL function pointers (its own +
   Syncable's + Argument's) rather than trying to compose Def templates up the
   hierarchy. Simpler and less template machinery.

4. **Incremental migration**: Old-style and new-style can coexist. Interfaces() visitor
   just needs to visit both. Migrate one Object at a time.

5. **Header vs source split**: Impl types are small and live in headers. Static table
   initialization lives in the Def template (inherently header-based). Complex helper
   functions can still live in .cc files.

6. **ArgumentOf replacement**: The current `ArgumentOf` binding type becomes just
   `Argument` — since the top-level name IS the bound type.

## Comparison: Current vs Proposed

### Current FlipFlop

Header:
```cpp
struct FlipFlop : Object {
  bool current_state = false;
  SyncState flip_sync;
  static Runnable flip;
  SyncState on_off_sync;
  static OnOff on_off;

  void Interfaces(const std::function<LoopControl(Interface&)>& cb) override {
    if (LoopControl::Break == cb(flip)) return;
    if (LoopControl::Break == cb(on_off)) return;
  }
  // ...
};
```

Source (static definitions with lambda soup):
```cpp
Runnable FlipFlop::flip(
    "Flip"sv, +[](FlipFlop& obj) -> SyncState& { return obj.flip_sync; },
    +[](const Runnable&, FlipFlop& self, std::unique_ptr<RunTask>&) {
      FlipFlop::on_off.Toggle(self);
    });

OnOff FlipFlop::on_off(
    "State"sv, +[](FlipFlop& obj) -> SyncState& { return obj.on_off_sync; },
    +[](const OnOff&, const FlipFlop& obj) -> bool { return obj.current_state; },
    +[](const OnOff&, FlipFlop& self) { self.current_state = true; self.WakeToys(); },
    +[](const OnOff&, FlipFlop& self) { self.current_state = false; self.WakeToys(); });

FlipFlop::~FlipFlop() { on_off.Unsync(*this); }
```

### Proposed FlipFlop

Header only:
```cpp
struct FlipFlop : Object {
  bool current_state = false;

  void Toggle() {
    current_state = !current_state;
    WakeToys();
  }

  struct Enabled : OnOff {
    using Parent = FlipFlop;
    static constexpr StrView kName = "State"sv;
    static constexpr int Offset() { return offsetof(FlipFlop, on_off); }

    bool IsOn() const { return self().current_state; }
    void OnTurnOn() { self().current_state = true; self().WakeToys(); }
    void OnTurnOff() { self().current_state = false; self().WakeToys(); }
  };
  OnOff::Def<Enabled> on_off;

  struct ToggleRunnable : Runnable {
    using Parent = FlipFlop;
    static constexpr StrView kName = "Flip"sv;
    static constexpr int Offset() { return offsetof(FlipFlop, flip); }

    void OnRun(std::unique_ptr<RunTask>&) {
      self().Toggle();
    }
  };
  Runnable::Def<ToggleRunnable> flip;

  INTERFACES(on_off, flip)
  // No explicit destructor needed — Def handles cleanup
  // ...
};
```

No .cc static definitions needed. No manual Interfaces() override.
No explicit destructor for sync cleanup.
