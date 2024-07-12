#include "time.hh"

namespace automat::time {

SystemPoint SystemFromSteady(SteadyPoint steady) { return SystemNow() + (steady - SteadyNow()); }
SteadyPoint SteadyFromSystem(SystemPoint system) { return SteadyNow() + (system - SystemNow()); }

}  // namespace automat::time