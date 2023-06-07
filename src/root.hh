#pragma once

#include "base.hh"

namespace automat {

extern Location root_location;
extern Machine* root_machine;
extern std::thread automat_thread;

void InitRoot();
void RunOnAutomatThread(std::function<void()>);
void RunOnAutomatThreadSynchronous(std::function<void()>);

}  // namespace automat