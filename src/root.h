#pragma once

#include "base.h"

namespace automaton {

extern Location root_location;
extern Machine* root_machine;

void InitRoot();
void RunOnAutomatonThread(std::function<void()>);
void RunOnAutomatonThreadSynchronous(std::function<void()>);

}