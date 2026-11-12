#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <pipewire/pipewire.h>

#include "status.hpp"

namespace automat {

struct Libpipewire {
  void LoadSymbols(Status&);

  decltype(&::pw_context_connect) context_connect;
  decltype(&::pw_context_destroy) context_destroy;
  decltype(&::pw_context_get_object) context_get_object;
  decltype(&::pw_context_new) context_new;
  decltype(&::pw_core_disconnect) core_disconnect;
  decltype(&::pw_core_get_context) core_get_context;
  decltype(&::pw_init) init;
  decltype(&::pw_log_get) log_get;
  decltype(&::pw_main_loop_destroy) main_loop_destroy;
  decltype(&::pw_main_loop_get_loop) main_loop_get_loop;
  decltype(&::pw_main_loop_new) main_loop_new;
  decltype(&::pw_main_loop_quit) main_loop_quit;
  decltype(&::pw_main_loop_run) main_loop_run;
  decltype(&::pw_node_state_as_string) node_state_as_string;
  decltype(&::pw_properties_free) properties_free;
  decltype(&::pw_properties_new) properties_new;
  decltype(&::pw_proxy_destroy) proxy_destroy;
  decltype(&::pw_stream_add_listener) stream_add_listener;
  decltype(&::pw_stream_connect) stream_connect;
  decltype(&::pw_stream_dequeue_buffer) stream_dequeue_buffer;
  decltype(&::pw_stream_destroy) stream_destroy;
  decltype(&::pw_stream_get_core) stream_get_core;
  decltype(&::pw_stream_new) stream_new;
  decltype(&::pw_stream_new_simple) stream_new_simple;
  decltype(&::pw_stream_queue_buffer) stream_queue_buffer;
  decltype(&::pw_thread_loop_destroy) thread_loop_destroy;
  decltype(&::pw_thread_loop_get_loop) thread_loop_get_loop;
  decltype(&::pw_thread_loop_lock) thread_loop_lock;
  decltype(&::pw_thread_loop_new) thread_loop_new;
  decltype(&::pw_thread_loop_signal) thread_loop_signal;
  decltype(&::pw_thread_loop_unlock) thread_loop_unlock;
  decltype(&::pw_thread_loop_wait) thread_loop_wait;
};

extern Libpipewire pw;

}  // namespace automat
