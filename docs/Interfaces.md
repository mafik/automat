# Interfaces

Most objects in Automat are snowflakes that hold bespoke data and behave according to their own rules. In order for them to interact with each other, Automat relies on its own kind of interfaces. Unlike language-level interfaces, Automat's interfaces are dynamic & introspectable. They store the vtable pointer as part of the "pointer-to-interface", rather than as part of the object itself.

This means that even if some object implements hundreds of interfaces, its size will stay constant (downside is that each pointer to such interface occupies 16, rather than 8 bytes).

This also means that it's possible to construct new interfaces for existing objects at runtime - an important capability for any dynamic system.

## Interface Table

The core of the interface is a table of interface-level data and function pointers. The interface table is a simple struct which may be constructed at compile or run time. It has no logic - only data.

Each interface table must extend the base `Interface::Table`. Derived interfaces should keep extending this table, adding their own function pointers, and possibly filling some of the base function pointers.

## Interface Kind

Interfaces are typed using LLVM-style RTTI. Each interface table starts with a 'kind' member that identifies the interface. Not all interfaces have unique kinds - many interfaces are anonymous. Only the well-known interfaces get their own kind.

Interfaces form a simple hierarchy which allows for very cheap dynamic casts, where the 'kind' member is checked for range membership.

## Interface Binding (the main Interface type)

The primary way of using the interface is through its bound type (binding for short). Binding is the top-level type of the interface. Binding is extremely lightweight. It's just a pair of pointers - always 16 bytes in total. The first pointer points to the object, and the second pointer points to the interface table. This is pointer-to-interface, bound to some object.

Each Binding must derive from `Interface` which provides the pair of pointers and the common logic for generic interfaces. Bindings may not introduce any data on their own - they may only use what's provided in their base type (which is two pointers). Bindings of interfaces should form a type hierarchy that mirrors the interface table hierarchy.

Binding contains all the methods for interacting with the interface. It IS the interface.

Under the hood, all of the Binding's methods use the included interface table to dynamically dispatch calls to the correct implementation. 

## Null States

Bindings are nullable. Both pointers default to `nullptr`. There are three valid states:

- **Empty** (`obj == nullptr, table == nullptr`) — no interface. Evaluates to `false`. Returned by failed casts and empty `NestedPtr` conversions.
- **Top-level object** (`obj != nullptr, table == nullptr`) — points to an object without a specific interface. Used by connection targets that connect to an object as a whole.
- **Interface** (`obj != nullptr, table != nullptr`) — a proper interface binding. All bound type methods require this state.

Bindings are convertible from `NestedPtr<Table>`, which makes it easy to go from a locked weak reference to a usable interface.

## State

Some interfaces may want to introduce some state to objects which implement them. This may be done through the `State` struct of the interface.

Just like `Table` and the top-level bound types, `State` structs must form a matching hierarchy of types.

Each interface `Table` includes one more member - that hasn't been mentioned yet - the state offset. This is the offset within the object, where the state of the interface is stored. State offset is used by `Interface::GetState()`, which allows the bindings to easily access their state within the object.

Naturally, every object that implements a stateful interface must store the state of that interface at some offset within itself - and set that offset in the interface table.

Stateless interfaces still get a state offset, but they're free to use it however they want. It's just a well-known integer within `Interface::Table`.

## DEF_INTERFACE & Impl

Defining interface tables & constructing bound types is quite a bit of boilerplate. It can be avoided using a `DEF_INTERFACE` & `DEF_END` macros. Every object that "defines" some interface using these macros gets two benefits:

1. Automatically constructed interface table, which is initialized based on functions defined between `DEF_INTERFACE` and `DEF_END` macros.
2. A zero-cost member with a fancy `->` operator that can be used as if it were the main interface itself (it acts on whatever object it was invoked from).

The implementation of the interface (called Impl) should be located between the macros. All of the methods on Impl should follow the `On*` naming convention - to distinguish them from the main interface methods, which may go through dynamic dispatch. All of the Impl methods may access the interface's state (through `state`), parent object (through `obj`) and table (through `table`).
