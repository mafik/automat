#include "root.h"

namespace automaton {
  
Location root_location;
Machine* root_machine;

void InitRoot() {
  root_location = Location(nullptr);
  root_machine = root_location.Create<Machine>();
}

}