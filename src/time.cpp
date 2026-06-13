// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "time.hpp"

namespace automat::time {

SystemPoint SystemFromSteady(SteadyPoint steady) { return SystemNow() + (steady - SteadyNow()); }
SteadyPoint SteadyFromSystem(SystemPoint system) { return SteadyNow() + (system - SystemNow()); }

}  // namespace automat::time