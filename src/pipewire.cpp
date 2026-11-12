// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "pipewire.hpp"

#include <dlfcn.h>

#include "crt.hpp"
#include "format.hpp"
#include "str.hpp"

namespace automat {

Libpipewire pw;

void Libpipewire::LoadSymbols(Status& status) {
  static const Str load_error = [this]() -> Str {
    void* library = crt::OpenLibrary("libpipewire-0.3.so.0", RTLD_NOW);
    if (!library) {
      return crt::LibraryError();
    }
#define LOAD(name)                                                      \
  name = (decltype(&::pw_##name))crt::FindSymbol(library, "pw_" #name); \
  if (!name) return f("libpipewire-0.3.so.0 has no {}", #name);

    LOAD(context_connect)
    LOAD(context_destroy)
    LOAD(context_get_object)
    LOAD(context_new)
    LOAD(core_disconnect)
    LOAD(core_get_context)
    LOAD(init)
    LOAD(log_get)
    LOAD(main_loop_destroy)
    LOAD(main_loop_get_loop)
    LOAD(main_loop_new)
    LOAD(main_loop_quit)
    LOAD(main_loop_run)
    LOAD(node_state_as_string)
    LOAD(properties_free)
    LOAD(properties_new)
    LOAD(proxy_destroy)
    LOAD(stream_add_listener)
    LOAD(stream_connect)
    LOAD(stream_dequeue_buffer)
    LOAD(stream_destroy)
    LOAD(stream_get_core)
    LOAD(stream_new)
    LOAD(stream_new_simple)
    LOAD(stream_queue_buffer)
    LOAD(thread_loop_destroy)
    LOAD(thread_loop_get_loop)
    LOAD(thread_loop_lock)
    LOAD(thread_loop_new)
    LOAD(thread_loop_signal)
    LOAD(thread_loop_unlock)
    LOAD(thread_loop_wait)
#undef LOAD
    return Str();
  }();
  if (!load_error.empty()) {
    AppendErrorMessage(status) += load_error;
  }
}

}  // namespace automat
