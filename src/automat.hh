// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <thread>

#include "base.hh"

// High-level automat code.

namespace automat {

extern std::stop_source stop_source;
extern std::shared_ptr<Location> root_location;
extern std::shared_ptr<Machine> root_machine;
extern std::jthread automat_thread;
extern std::atomic_bool automat_thread_finished;

void StopAutomat(maf::Status&);

void EnqueueTask(Task* task);
void RunOnAutomatThread(std::function<void()>);
void RunOnAutomatThreadSynchronous(std::function<void()>);
void AssertAutomatThread();

void RunLoop(const int max_iterations = -1);

}  // namespace automat