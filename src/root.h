#pragma once

#include "base.h"

namespace automaton {

extern Location root_location;
extern Machine *root_machine;
extern std::thread automaton_thread;

void InitRoot();
void RunOnAutomatonThread(std::function<void()>);
void RunOnAutomatonThreadSynchronous(std::function<void()>);

} // namespace automaton