#include "knob.hh"

#include <cmath>   // For std::abs, sqrtf, isinf, isnan
#include <limits>  // For numeric_limits

#include "math.hh"

using namespace maf;

namespace automat {

// Helper function to solve 3x3 linear system Ax = Y using Cramer's rule / Adjugate Matrix
// Returns true if successful, false if matrix is singular.
// A is a 3x3 matrix (row-major)
// Y is a 3x1 vector
// X is the 3x1 solution vector (output)
static bool Solve3x3(const float A[3][3], const float Y[3], float X[3]) {
  // Calculate determinant
  float det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
              A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
              A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);

  // Check for singularity (use a small epsilon)
  constexpr float kDeterminantEpsilon = 1e-9f;
  if (std::abs(det) < kDeterminantEpsilon) {
    return false;  // Matrix is singular or near-singular
  }

  float inv_det = 1.0f / det;

  // Calculate cofactors (transposed for adjugate)
  // adj(A)[i][j] = Cofactor(A)[j][i]
  float C[3][3];
  C[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]);
  C[1][0] = -(A[0][1] * A[2][2] - A[0][2] * A[2][1]);
  C[2][0] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]);
  C[0][1] = -(A[1][0] * A[2][2] - A[1][2] * A[2][0]);
  C[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]);
  C[2][1] = -(A[0][0] * A[1][2] - A[0][2] * A[1][0]);
  C[0][2] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
  C[1][2] = -(A[0][0] * A[2][1] - A[0][1] * A[2][0]);
  C[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]);

  // Calculate solution X = A^-1 * Y = (1/det) * adj(A) * Y
  X[0] = inv_det * (C[0][0] * Y[0] + C[1][0] * Y[1] + C[2][0] * Y[2]);
  X[1] = inv_det * (C[0][1] * Y[0] + C[1][1] * Y[1] + C[2][1] * Y[2]);
  X[2] = inv_det * (C[0][2] * Y[0] + C[1][2] * Y[1] + C[2][2] * Y[2]);

  return true;
}

// Fit a section of a circle to the given sequence of points. Output `tangent`, `radius` & `center`.
//
// If the points form  a line, `radius` will be set to infinity and `center` will be undefined.
static void FitArc(const std::deque<Vec2>& points, SinCos& tangent, float& radius, Vec2& center) {
  size_t n = points.size();

  // --- Handle Degenerate Cases ---
  if (n < 2) {
    // Not enough points to define direction or curvature
    tangent = 0_deg;  // Default tangent (rightwards)
    radius = std::numeric_limits<float>::infinity();
    return;
  }

  Vec2 p_last = points.back();
  Vec2 p_prev = points[n - 2];
  Vec2 last_segment = p_last - p_prev;
  float last_segment_len_sq = LengthSquared(last_segment);
  constexpr float kMinLenSq = 1e-9f;  // Threshold for coincident points

  if (n == 2) {
    // Only two points: treat as a straight line
    tangent = SinCos::FromVec2(last_segment, sqrtf(last_segment_len_sq));
    radius = std::numeric_limits<float>::infinity();
    return;
  }

  // --- n >= 3: Least-Squares Circle Fit (Kåsa method) ---
  // We want to fit (x - xc)^2 + (y - yc)^2 = r^2
  // Rearranging: 2*xc*x + 2*yc*y + (r^2 - xc^2 - yc^2) = x^2 + y^2
  // Let a = 2*xc, b = 2*yc, c = r^2 - xc^2 - yc^2
  // Solve the linear system: a*x + b*y + c = x^2 + y^2 for a, b, c

  // Use double for sums to improve numerical stability
  double sum_x = 0, sum_y = 0, sum_x2 = 0, sum_y2 = 0, sum_xy = 0;
  double sum_x_r2 = 0, sum_y_r2 = 0, sum_r2 = 0;

  for (const auto& p : points) {
    double x = p.x;
    double y = p.y;
    double x2 = x * x;
    double y2 = y * y;
    double r_sq_term = x2 + y2;  // x^2 + y^2 term

    sum_x += x;
    sum_y += y;
    sum_x2 += x2;
    sum_y2 += y2;
    sum_xy += x * y;
    sum_x_r2 += x * r_sq_term;
    sum_y_r2 += y * r_sq_term;
    sum_r2 += r_sq_term;
  }

  // Set up the matrix A for the system A * [a, b, c]^T = Y
  float A[3][3] = {{(float)sum_x2, (float)sum_xy, (float)sum_x},
                   {(float)sum_xy, (float)sum_y2, (float)sum_y},
                   {(float)sum_x, (float)sum_y, (float)n}};

  // Set up the vector Y
  float Y[3] = {(float)sum_x_r2, (float)sum_y_r2, (float)sum_r2};

  float solution[3];  // Solution [a, b, c]

  // --- Solve the System and Handle Collinear Case ---
  bool solved = Solve3x3(A, Y, solution);

  if (solved) {
    float a = solution[0];
    float b = solution[1];
    float c_param = solution[2];  // Renamed from 'c' to avoid conflict

    float xc = a / 2.0f;
    float yc = b / 2.0f;
    float r_sq = c_param + xc * xc + yc * yc;

    // Check for valid radius (non-negative and not excessively large)
    constexpr float kMinRadiusSq = 1e-12f;  // Min radius^2 to avoid huge curvature/division by zero
    constexpr float kMaxRadius = 1e6f;      // Max radius before treating as linear

    if (r_sq < kMinRadiusSq || std::isinf(r_sq) || std::isnan(r_sq)) {
      radius = std::numeric_limits<float>::infinity();
    } else {
      radius = sqrtf(r_sq);
      if (radius > kMaxRadius) {
        radius = std::numeric_limits<float>::infinity();
      } else {
        center = {xc, yc};
      }
    }
  } else {
    radius = std::numeric_limits<float>::infinity();
  }

  // --- Determine Tangent ---
  if (isinf(radius)) {
    tangent = SinCos::FromVec2(p_last - points.front());
  } else {
    tangent = SinCos::FromVec2(p_last - center) + 90_deg;
  }
}

void Knob::Update(Vec2 position) {
  history.push_back(position);

  // How much distance do we have in history?
  float history_length = 0;
  for (size_t i = 1; i < history.size(); ++i) {
    history_length += Length(history[i] - history[i - 1]);
  }

  // How much history do we want to keep to track the gesture?
  float min_values = 3;
  float min_length = min_values * unit_distance;

  if (history.size() >= 2) {
    SinCos new_tangent;
    float new_radius;
    Vec2 new_center;
    FitArc(history, new_tangent, new_radius, new_center);
    if (!isinf(radius)) {
      float min_arc_length = 2 * M_PI * radius;
      min_length = std::min(min_length, min_arc_length);
    }
    float a = std::min(1.0f, history_length / min_length);
    a = 1;
    tangent = new_tangent * a;
    radius = new_radius * a;
    center = new_center * a;
  }

  // Advance the `value` according to the last two points and current `tangent` and `curvature`.
  if (history.size() >= 2) {
    Vec2 curr = history.back();
    Vec2 prev = history[history.size() - 2];
    Vec2 vec_diff = curr - prev;
    Vec2 vec_unit = Vec2::Polar(tangent, unit_distance);
    float value_diff = VectorProjection(vec_unit, vec_diff);
    if (!isinf(radius)) {
      SinCos angle_curr = SinCos::FromVec2(curr - center, radius);
      SinCos angle_prev = SinCos::FromVec2(prev - center, radius);
      SinCos angle_diff = angle_curr - angle_prev;
      float angle_diff_rad = angle_diff.ToRadiansPositive();
      float angle_diff_value = angle_diff_rad / unit_angle.ToRadiansPositive();
      if (fabs(angle_diff_value) > fabs(value_diff)) {
        value_diff = angle_diff_value;
      }
    }
    value += value_diff;
  }

  while (history.size() > 2) {
    float head_length = Length(history[0] - history[1]);
    float new_length = history_length - head_length;
    if (new_length > min_length) {
      history.pop_front();
      history_length -= head_length;
    } else {
      break;
    }
  }
}

}  // namespace automat
