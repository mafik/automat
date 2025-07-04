// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <thread>

#include "base.hh"

// High-level automat code.

namespace automat {

extern std::stop_source stop_source;
extern Ptr<Location> root_location;
extern Ptr<Machine> root_machine;
extern std::jthread automat_thread;
extern std::atomic_bool automat_thread_finished;

extern std::thread::id main_thread_id;

void StopAutomat(Status&);

void EnqueueTask(Task* task);
void RunOnAutomatThread(std::function<void()>);
void RunOnAutomatThreadSynchronous(std::function<void()>);
void AssertAutomatThread();

void RunLoop();

}  // namespace automat