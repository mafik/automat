#include "animation.hh"

namespace automat::animation {

maf::Vec<Display*> displays;

Phase LowLevelSineTowards(float target, float delta_time, float period_time, float& value,
                          float& velocity) {
  float D = value - target;
  if (fabsf(D) < 0.00001f) {
    value = target;
    velocity = 0;
    return Finished;
  } else {
    // Use the cosine tweening here.
    // D = A * (cos(t) / 2 + 0.5)
    // V = - A * sin(t) / 2
    // -V*2/sin(t) = A
    // D = (-V*2/sin(t)) * (cos(t) / 2 + 0.5)
    float x = fabsf(velocity) < 0.00001f ? 0 : -2 * atan2(velocity, D);
    if (x <= -kPi) {
      x += kPi * 2;
    } else if (x > kPi) {
      x -= kPi * 2;
    }
    if (x <= -kPi / 2) {
      // The function that we're using for animation (cos(t) scaled by A) has a section where small
      // deviations from target value are amplified. The object effectively accelerates away from
      // the target. We avoid this section by simply clamping the value of x.
      x = -kPi / 2;
    }
    float A = fabs(x) < 0.00000001f ? D : -2 * velocity / sin(x);
    float x2 = x + delta_time / period_time * kPi * 2;
    if (x2 > kPi) {
      x2 = kPi;
    }
    value = A * (cos(x2) / 2 + 0.5) + target;
    velocity = -A * sin(x2) / 2;
    return Animating;
  }
}
Phase LowLevelSpringTowards(float target, float delta_time, float period_time, float half_time,
                            float& value, float& velocity) {
  float Q = 2 * M_PI / period_time;
  float D = value - target;
  float V = velocity;
  float H = half_time;

  float t;
  float amplitude;
  if (fabsf(D) > 1e-6f) {
    t = -atanf((D * kLog2e + V * H) / (D * H * Q)) / Q;
    amplitude = D / powf(2, -t / H) / cosf(t * Q);
  } else {
    if (fabsf(V) < 1e-6f) {
      value = target;
      velocity = 0;
      return Finished;
    }
    t = period_time / 4;
    amplitude = -velocity * powf(2.f, t / H) / Q;
  }
  float t2 = t + delta_time;
  value = target + amplitude * cosf(t2 * Q) * powf(2, -t2 / H);
  velocity =
      (-(amplitude * kLog2e * cosf(t2 * Q)) / H - amplitude * Q * sinf(t2 * Q)) / powf(2, t2 / H);
  return Animating;
}

float SinInterp(float x, float x0, float y0, float x1, float y1) {
  float t = (x - x0) / (x1 - x0);
  if (t <= 0) {
    return y0;
  } else if (t >= 1) {
    return y1;
  } else {
    return y0 + (y1 - y0) * (1 - cosf(t * kPi)) / 2;
  }
}

void WrapModulo(float& value, float target, float range) {
  value = std::remainder(value - target, range) + target;
}

}  // namespace automat::animation