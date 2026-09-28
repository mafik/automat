# Control Flow

## Core

Automat's control flow is based around a "Command" interface:

```cpp
struct Command {
  virtual Command* OnRun();
};
```

It can perform some work & return the next Command to execute (called 'successor').

At this interface level, it's possible for Automat objects to implement loops, conditionals, select statements & coroutines - by returning different successor Commands.

Most objects would store an explicit successor - and it would be always returned. This is what the user can tweak by re-connecting the cable.

## Continuations

Every Object also stores a `then` member variable:

```cpp
struct Object {
  // ...
  Command* then;
  //...
};
```

Every Command that would return `nullptr`, should check the `then` member & return it instead (swapping it to `nullptr` at the same time).

Every Command that would return non-`nullptr`, should copy its own `then` member to that target (setting its own value to `nullptr` at the same time).

Objects linked by the `then` relation form a stack (a list actually).

The idea with `then` is that it's an extension point that enables more primitives:
- sub-procedures (code that can be called from any where and will eventually return)
- and-then chains (async functions, promises, monads)

*Sub-procedures* can be implemented by storing the continuation address in sub-procedure's `then` & then returning that sub-procedure.

*And-then chains* can be implemented by following the `then` list till the end & setting the last `nullptr` to the desired and-then target.

Some objects would manage their own `then` pointer internally - those would be dedicated objects that implement these more advanced continuation management.

For most simple objects, the `then` management would be handled by **Hooks** - another system that can extend Objects by mutating ther ToC (see `Interfaces.md`). Examples of such hooks would be: synchronization (Gear) or sub-procedure call (Lightning).

## Cancellation

There is a contract in the `Command` interface that once a `Command` is successfully scheduled (for eventual concurrent exeucution) or simply returned from `OnRun()` - then there is a kind of "promise" that it will eventually be executed.

Due to various reason this promise may sometimes not be satisfiable. When this is the case then a command may be *cancelled*. A command may be cancelled befure it started execution (between schedule/return & its `OnRun`), during its `OnRun` or even after it (very rarely, but might happen due to concurrency).

To support cancellation, every command may specify another Command that should be executed to cancel it - this is the *Cancel Command*. Every command may have one. Even Cancel Commands may have more cancel commands.

Once a command is cancelled (cancellation may be called on another thread) then the original command should stop execution. If it doesn't, it may be interrupted with a signal/exception.

Execution contract & cancellation is a mechanism that allows Objects to implement primitives such as RAII & scopes.

## Scheduling

Should a scheduler hold a strong (or weak?) reference to the scheduled Command?

Obvious consequence: weak reference may be dropped.

Scheduler is a concurrent queue - hard to iterate & visualize interactively.

Intuition points towards weak references...