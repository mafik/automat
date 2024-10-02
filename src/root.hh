// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <thread>

#include "base.hh"

namespace automat {

extern Location root_location;
extern Machine* root_machine;
extern std::jthread automat_thread;
extern std::atomic_bool automat_thread_finished;

// Starts the Automat main loop
void InitRoot();

// Stops the Automat main loop
void StopRoot();

void RunOnAutomatThread(std::function<void()>);
void RunOnAutomatThreadSynchronous(std::function<void()>);
void AssertAutomatThread();

}  // namespace automat