#include "animation.hh"

namespace automat::animation {

void WrapModulo(Base<float>& base, float range) {
  if (base.value - base.target > range / 2) {
    base.value -= range;
  } else if (base.target - base.value > range / 2) {
    base.value += range;
  }
}

}  // namespace automat::animation