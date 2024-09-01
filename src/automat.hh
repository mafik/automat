#pragma once

#include "status.hh"

// High-level automat code.

namespace automat {

// Sets up gui::window, gui::keyboard, and loads the state from JSON.
void InitAutomat(maf::Status&);

void StopAutomat(maf::Status&);

}  // namespace automat