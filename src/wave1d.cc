#include "wave1d.hh"

namespace automat {

Wave1D::Wave1D(int n, float wave_speed, float column_spacing)
    : n(n), wave_speed(wave_speed), column_spacing(column_spacing), state(n * 2) {}

// See https://en.wikipedia.org/wiki/Tridiagonal_matrix_algorithm
static void Thomas(const int X, float x[X], const float a[X], const float b[X], const float c[X]) {
  /*
   solves Ax = d, where A is a tridiagonal matrix consisting of vectors a, b, c
   X = number of equations
   x[] = initially contains the input, d, and returns x. indexed from [0, ..., X - 1]
   a[] = subdiagonal, indexed from [1, ..., X - 1]
   b[] = main diagonal, indexed from [0, ..., X - 1]
   c[] = superdiagonal, indexed from [0, ..., X - 2]
   scratch[] = scratch space of length X, provided by caller, allowing a, b, c to be const
   */
  float scratch[X];
  scratch[0] = c[0] / b[0];
  x[0] = x[0] / b[0];

  /* loop from 1 to X - 1 inclusive */
  for (int ix = 1; ix < X; ix++) {
    if (ix < X - 1) {
      scratch[ix] = c[ix] / (b[ix] - a[ix] * scratch[ix - 1]);
    }
    x[ix] = (x[ix] - a[ix] * x[ix - 1]) / (b[ix] - a[ix] * scratch[ix - 1]);
  }

  /* loop from X - 2 to 0 inclusive */
  for (int ix = X - 2; ix >= 0; ix--) x[ix] -= scratch[ix] * x[ix + 1];
}

void Wave1D::Step(float dt) {
  const int kColumnCount = n + 2;

  // Note that the `height` & `velocity` vectors have size n, but all the calculations use n+2
  // columns!
  auto height = Amplitudes();
  auto velocity = Velocity();

  // Copy of the initial amplitude, so that it can be used to take the steps according to
  // the 4th order Runge-Kutta method.
  float height_prev[kColumnCount];
  for (int i = 0; i < n; ++i) {
    height_prev[i + 1] = height[i];
  }
  height_prev[0] = height_prev[1];
  height_prev[kColumnCount - 1] = height_prev[kColumnCount - 2];

  // Same as above - for the velocity.
  float velocity_prev[kColumnCount];
  for (int i = 0; i < n; ++i) {
    velocity_prev[i + 1] = velocity[i];
  }
  velocity_prev[0] = velocity_prev[1];
  velocity_prev[kColumnCount - 1] = velocity_prev[kColumnCount - 2];

  // Temporary vectors for RK.
  float velocity_star[kColumnCount];
  float height_star[kColumnCount];
  float accel_star[kColumnCount];

  auto EstimateVelStar = [&](float dt) {
    for (int i = 1; i < kColumnCount - 1; ++i) {
      velocity_star[i] = velocity_prev[i] + (dt * accel_star[i]);
    }
    velocity_star[0] = velocity_star[1];
    velocity_star[kColumnCount - 1] = velocity_star[kColumnCount - 2];
  };

  auto EstimateHeightStar = [&](float dt) {
    for (int i = 1; i < kColumnCount - 1; ++i) {
      height_star[i] = height_prev[i] + (dt * velocity_star[i]);
    }
    height_star[0] = height_star[1];
    height_star[kColumnCount - 1] = height_star[kColumnCount - 2];
  };

  auto EstimateAccelStar = [&](float dt) {
    const int X = kColumnCount;
    // TODO: replace `x` with `accel_star`
    float x[X];
    const float gamma = wave_speed * wave_speed / (column_spacing * column_spacing);
    const float kappa = gamma * dt * dt;
    // Fill x with Wikipedia's "d" which is Encinographic's "b", which is roughly wave
    // curvature.
    for (int i = 0; i < X; ++i) {
      x[i] = 0;
    }
    for (int i = 0; i < X; ++i) {
      if (i > 0) {
        x[i] += height_star[i - 1] - height_star[i];
      }
      if (i < X - 1) {
        x[i] += height_star[i + 1] - height_star[i];
      }
    }
    for (int i = 0; i < X; ++i) {
      x[i] *= gamma;
    }
    float a[X];
    float b[X];
    float c[X];
    for (int i = 0; i < X; ++i) {
      a[i] = -kappa;
      b[i] = 1 + 2 * kappa;
      c[i] = -kappa;
    }
    Thomas(X, x, a, b, c);
    for (int i = 0; i < X; ++i) {
      accel_star[i] = x[i];
    }
  };

  auto AccumulateEstimate = [&](float dt) {
    for (int i = 0; i < n; ++i) {
      height[i] += dt * velocity_star[i + 1];
      velocity[i] += dt * accel_star[i + 1];
    }
  };

  // 1st step of RK
  for (int i = 0; i < kColumnCount; ++i) {  // velocity_star is just the velocity.
    velocity_star[i] = velocity_prev[i];
  }
  EstimateHeightStar(dt);
  EstimateAccelStar(dt);
  AccumulateEstimate(dt / 6.0);

  // 2nd step of RK
  EstimateVelStar(dt / 2.0);
  EstimateHeightStar(dt / 2.0);
  EstimateAccelStar(dt / 2.0);
  AccumulateEstimate(dt / 3.0);

  // 3rd step of RK
  EstimateVelStar(dt / 2.0);
  EstimateHeightStar(dt / 2.0);
  EstimateAccelStar(dt / 2.0);
  AccumulateEstimate(dt / 3.0);

  // 4th step of RK
  EstimateVelStar(dt);
  EstimateHeightStar(dt);
  EstimateAccelStar(dt);
  AccumulateEstimate(dt / 6.0);
}

std::span<const float> Wave1D::Amplitudes() const { return std::span(state).subspan(0, n); }
std::span<float> Wave1D::Amplitudes() { return std::span(state).subspan(0, n); }
std::span<float> Wave1D::Velocity() { return std::span(state).subspan(n); }

}  // namespace automat