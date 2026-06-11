# Multithreading

This document describes the intended multithreading architecture for Automat.

Objectives (in the order of importance):
1. Minimize the latency of event delivery & processing
2. Isolate the Automat's responsiveness from rendering speed
3. Maximize the utilization of available computational resources

Automat distinguishes between `Objects` (types derived from the `Object` class) and `Widgets` (types derived from the `Widget` class). Objects typically are paired with Widgets. The object part is responsible for the computational aspects while widgets take care of drawing their contents. This allows Automat to execute in headless mode with less overhead.

## Objects

Automat's object may be accessed from any thread and therefore are responsible for their own thread safety. They must internally utilize appropriate locking strategies (mutexes / atomics) when implementing the `Object` interface.

Internally objects may use their own concurrency strategy.

Objects must be able to produce a Widget to draw them. Widgets don't need to be thread safe because they are owned by the Automat's rendering layer and their usage is protected by a "client mutex". Whenever Widgets refer back to Objects that created them, they should use `WeakPtr` since their originating objects are owned by external threads and may be deleted at any point.

## Widgets

IMPORTANT: Note that the terminology we use here distinguishes between "drawing" and "rendering". The former is about recording the list of commands that will draw the object using some abstract backend (Vulkan, Metal, Dawn, bare CPU), without caring about the specifics of the backend. Widgets only care about the "drawing" part. The rendering is handled in a cross-platform way by the Automat's rendering code (though right now only Vulkan is supported).

Widgets are accessed from multiple threads, each of which is related to the same Window - (1) the render thread and (2) the event loop thread (there is also third thread that controls audio, but it's only receiving commands to play audio and doesn't care about widgets themselves). Since the Widgets are accessed from two threads, their access is protected by a mutex. Right now Automat displays only one window but its design allows for concurrent access from multiple clients (for example a potential web-based client for remote access).

The event loop thread is responsible for receiving and responding to the OS events. To do so it may invoke the appropriate Widget or Object APIs. The event loops are platform specific and can be found in `*_window.hh` files (types derived from `gui::Window`).

The render thread is responsible for smooth *rendering* of the UI. This is done by a an algorithm that analyzes the tree of widgets and selectively choses which widgets to render in the next frame and which ones should be rendered in the background. Some diagrams of the algorithm (called "Frame Packing") can be found [here](https://www.tldraw.com/ro/3d97dFMiuM0MLgqyyP0SG?d=v0.0.1369.751.oabcxxQj34hFXH8A6_75W).

## Worker threads (TODO)

Objects execute their code directly on the thread that they have been invoked on. They may also follow with the execution of the next object, reusing their current thread. Any side tasks should be put on the task queue, to be executed by worker threads. This avoids the use of extra threads for simple jobs and makes their execution more deterministic. The control of when to execute the next task immediately vs when to defer it to task queue should be controlled by the user (although specifics of how this may happen are not designed yet).

Currently Automat uses only one worker thread (called "Automat Loop") and doesn't exploit thread reuse. This is temporary and will change eventually as the need for more accurate thread control comes up.

## Tasks (TODO)

The main unit of work is based on the "Task" class. The goal of this class is to allow flexible tracking of work done on multiple threads and proper sequencing. Tasks are the main way of "waking up" objects from external threads and are executed on the worker threads.

Task lifetime is managed by the task itself. Two examples of Task lifetime management are:
- RunTask - it's allocated lazily for each Location. It's lifetime is bound to Location
- other tasks - they're allocated whenever they're needed. They delete themselves after calling Execute

Because of their irregular lifetimes, Tasks are managed through raw pointers and require more attention.

## Timer thread

Automat also has a timer thread - a dedicated thread that ensures that events are delivered with accurate timing. See `timer_thread.hh` for details. There is only one timer thread per Automat's instance.

## Summary

- Automat's threading implementation doesn't include all of the features that are part of the design
- Design allows for concurrent access from multiple windows (only one window right now)
- Design allows for variable number of worker threads and thread reuse (only one worker and little thread reuse right now)
- Each window comes with three threads that control its different aspects (event loop, render, audio)
- Single timer thread helps objects execute in a timely manner
