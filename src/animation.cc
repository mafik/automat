#include "animation.hh"

namespace automat::animation {

maf::Vec<Display*> displays;

void WrapModulo(Base<float>& base, float range) {
  if (base.value - base.target > range / 2) {
    base.value -= range;
  } else if (base.target - base.value > range / 2) {
    base.value += range;
  }
}

void LowLevelSineTowards(float target, float delta_time, float period_time, float& value,
                         float& velocity) {
  float D = value - target;
  if (fabsf(D) < 0.00001f) {
    value = target;
    velocity = 0;
  } else {
    // Use the cosine tweening here.
    // D = A * (cos(t) / 2 + 0.5)
    // V = - A * sin(t) / 2
    // -V*2/sin(t) = A
    // D = (-V*2/sin(t)) * (cos(t) / 2 + 0.5)
    float x = fabsf(velocity) < 0.00001f ? 0 : -2 * atan2(velocity, D);
    if (x <= -M_PIf) {
      x += M_PIf * 2;
    } else if (x > M_PIf) {
      x -= M_PIf * 2;
    }
    if (x <= -M_PIf / 2) {
      // The function that we're using for animation (cos(t) scaled by A) has a section where small
      // deviations from target value are amplified. The object effectively accelerates away from
      // the target. We avoid this section by simply clamping the value of x.
      x = -M_PIf / 2;
    }
    float A = fabs(x) < 0.00000001f ? D : -2 * velocity / sin(x);
    float x2 = x + delta_time / period_time * M_PIf * 2;
    if (x2 > M_PIf) {
      x2 = M_PIf;
    }
    value = A * (cos(x2) / 2 + 0.5) + target;
    velocity = -A * sin(x2) / 2;
  }
}
void LowLevelSpringTowards(float target, float delta_time, float period_time, float half_time,
                           float& value, float& velocity) {
  float Q = 2 * M_PI / period_time;
  float D = value - target;
  float V = velocity;
  float H = half_time;

  float t;
  float amplitude;
  if (fabsf(D) > 1e-6f) {
    t = -atanf((D * M_LOG2Ef + V * H) / (D * H * Q)) / Q;
    amplitude = D / powf(2, -t / H) / cosf(t * Q);
  } else {
    t = period_time / 4;
    amplitude = -velocity * powf(2.f, t / H) / Q;
  }
  float t2 = t + delta_time;
  value = target + amplitude * cosf(t2 * Q) * powf(2, -t2 / H);
  velocity =
      (-(amplitude * M_LOG2Ef * cosf(t2 * Q)) / H - amplitude * Q * sinf(t2 * Q)) / powf(2, t2 / H);
}

float SinInterp(float x, float x0, float y0, float x1, float y1) {
  float t = (x - x0) / (x1 - x0);
  if (t <= 0) {
    return y0;
  } else if (t >= 1) {
    return y1;
  } else {
    return y0 + (y1 - y0) * (1 - cosf(t * M_PIf)) / 2;
  }
}

}  // namespace automat::animation