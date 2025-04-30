#include "knob.hh"

#include "math.hh"

using namespace maf;

namespace automat {

// Fit a section of a circle to the given sequence of points. Output `tangent` and `curvature`.
void FitArc(const std::deque<Vec2>& points, SinCos& tangent, float& curvature) {
  // TODO: Implement this.
}

void Knob::Update(Vec2 position) {
  history.push_back(position);

  // How much distance do we have in history?
  float history_length = 0;
  for (size_t i = 1; i < history.size(); ++i) {
    history_length += Length(history[i] - history[i - 1]);
  }

  // How much history do we want to keep to track the gesture?
  float min_values = 5;
  float min_length = min_values * unit_distance;
  if (curvature != 0) {
    float min_arc_length = min_values * unit_angle.ToRadiansPositive() / curvature;
    min_length = std::min(min_length, min_arc_length);
  }

  if (history.size() >= 2) {
    SinCos new_tangent;
    float new_curvature;
    FitArc(history, new_tangent, new_curvature);
    float a = history_length / min_length;
    if (a < 1) {
      tangent = new_tangent * a;
    } else {
      tangent = new_tangent;
    }
  }

  // Advance the `value` according to the last two points and current `tangent` and `curvature`.
  if (history.size() >= 2) {
    Vec2 curr = history.back();
    Vec2 prev = history[history.size() - 2];
    Vec2 vec_diff = curr - prev;
    Vec2 vec_unit = Vec2::Polar(tangent, unit_distance);
    float value_diff = VectorProjection(vec_unit, vec_diff);
    if (curvature != 0) {
      float radius = 1 / curvature;
      Vec2 center = curr + Vec2::Polar(tangent + 90_deg, radius);
      SinCos angle_curr = SinCos::FromVec2(curr - center, radius);
      SinCos angle_prev = SinCos::FromVec2(prev - center, radius);
      SinCos angle_diff = angle_curr - angle_prev;
      float angle_diff_rad = angle_diff.ToRadiansPositive();
      float angle_diff_value = angle_diff_rad / unit_angle.ToRadiansPositive();
      if (fabs(angle_diff_value) < fabs(value_diff)) {
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
