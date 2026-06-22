#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "mux_epoll.hpp"
#include "wayland_ext.hpp"  // IWYU pragma: export

// Provides utilities for working with the Wayland protocol:
//
// 1. A set of well-documented classes that directly map to Wayland interafces
// 2. Automat's built-in Wayland Compositor
//
// Automat's implementation of Wayland interfaces turns each interface into a lightweight C++ class.
//
// *Requests* are methods that start with 'On*'. Server must implement each of them. The client may
// call them whenever they wish.
//
// *Events* are methods that do not start with 'On'. Server may call them whenever it wishes to.
// They are buffered internally and will be sent over to the client at the next 'server.FlushAll()'.
//
// *Lifetime* of the interfaces is entirely up to clients. Clients may allocate up to ~4.3 billion
// objects and have no obligation to ever free them. Turns out DoS is a core feature of the Wayland
// protocol... Anyway - because Wayland interfaces are so ephemeral (at least on the Server side) -
// they should really be treated more like references to some more permanent objects, potentially
// shared across clients. For Automat those are various Object-derivatives, tracked in a thread-safe
// way through Ptr.
//
// To make the C++ classes easier to use, Automat provides them with some accounting metadata that
// allows natural method calls ('kind', 'id' & 'client'-ptr). Additionally each of them may
// potentially be extended with some interface-specific data by specializing the `Base<Interface>`
// type in `wayland_ext.hpp`.
//
// Current implementation is O(1) wherever possible, uses compact array-based indexing, global
// Colony-based allocation etc. The efficiency is currently balanced with ergonomy of use. A more
// efficient implementation is possible - one that would not store any class (kind + id +
// client-ptr + extension data) and not even their index but instead - just 'kind' for each id (this
// is essentially 1B / object). This would however force the user to store the extra data
// out-of-band (in some kind of hashmap keyed by 'client' + 'id'). This alternative design may be
// considered in the future - especially if the `Base<Interface>` extensions turn out to not be that
// useful...
namespace automat::wayland {

// TODO: Make the API more RAII-like

// Starts a new Wayland Compositor a.k.a. Wayland Server a.k.a. Wayland Display.
//
// Picks an unused WAYLAND_DISPLAY.
//
// Also creates flock-ed ${WAYLAND_DISPLAY}.lock - so that socket may be reused in case of crash.
//
// I/O happens through the given epoll instance.
std::unique_ptr<Server> MakeServer(mux::Epoll&, Status&);

// The process-global compositor. Created in automat::Main via MakeServer; null until then. Every
// caller outside wayland.cpp reaches the compositor through this.
extern std::unique_ptr<Server> server;

}  // namespace automat::wayland