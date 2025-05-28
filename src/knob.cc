#include "knob.hh"

#include <cmath>   // For std::abs, sqrtf, isinf, isnan
#include <limits>  // For numeric_limits

#include "math.hh"

// See: https://people.cas.uab.edu/~mosya/cl/CPPcircle.html
namespace automat {

using reals = float;

// Class for Data
// A data has 5 fields:
//       n (of type int), the number of data points
//       X and Y (arrays of type reals), arrays of x- and y-coordinates
//       meanX and meanY (of type reals), coordinates of the centroid (x and y sample means)

class Data {
 public:
  int n;
  reals* X;  // space is allocated in the constructors
  reals* Y;  // space is allocated in the constructors
  reals meanX, meanY;

  // constructors
  Data() {
    n = 0;
    X = new reals[n];
    Y = new reals[n];
    for (int i = 0; i < n; i++) {
      X[i] = 0.;
      Y[i] = 0.;
    }
  }
  Data(int N) {
    n = N;
    X = new reals[n];
    Y = new reals[n];

    for (int i = 0; i < n; i++) {
      X[i] = 0.;
      Y[i] = 0.;
    }
  }
  Data(int N, reals dataX[], reals dataY[]) {
    n = N;
    X = new reals[n];
    Y = new reals[n];

    for (int i = 0; i < n; i++) {
      X[i] = dataX[i];
      Y[i] = dataY[i];
    }
  }

  // Routine that computes the x- and y- sample means (the coordinates of the centeroid)
  void means(void) {
    meanX = 0.;
    meanY = 0.;

    for (int i = 0; i < n; i++) {
      meanX += X[i];
      meanY += Y[i];
    }
    meanX /= n;
    meanY /= n;
  }

  // Routine that centers the data set (shifts the coordinates to the centeroid)
  void center(void) {
    reals sX = 0., sY = 0.;
    int i;

    for (i = 0; i < n; i++) {
      sX += X[i];
      sY += Y[i];
    }
    sX /= n;
    sY /= n;

    for (i = 0; i < n; i++) {
      X[i] -= sX;
      Y[i] -= sY;
    }
    meanX = 0.;
    meanY = 0.;
  }
  // Routine that scales the coordinates (makes them of order one)
  void scale(void) {
    reals sXX = 0., sYY = 0., scaling;
    int i;

    for (i = 0; i < n; i++) {
      sXX += X[i] * X[i];
      sYY += Y[i] * Y[i];
    }
    scaling = sqrt((sXX + sYY) / n / 2);

    for (i = 0; i < n; i++) {
      X[i] /= scaling;
      Y[i] /= scaling;
    }
  }

  // destructors
  ~Data() {
    delete[] X;
    delete[] Y;
  }
};

// Class for Circle
// A circle has 7 fields:
//     a, b, r (of type reals), the circle parameters
//     s (of type reals), the estimate of sigma (standard deviation)
//     g (of type reals), the norm of the gradient of the objective function
//     i and j (of type int), the iteration counters (outer and inner, respectively)
class Circle {
 public:
  // The fields of a Circle
  reals a, b, r, s, g, Gx, Gy;
  int i, j;

  // constructors
  Circle() {
    a = 0.;
    b = 0.;
    r = 1.;
    s = 0.;
    i = 0;
    j = 0;
  }
  Circle(reals aa, reals bb, reals rr);

  // routines
  void print(void);

  // no destructor we didn't allocate memory by hand.
};

// Constructor with assignment of the circle parameters only

Circle::Circle(reals aa, reals bb, reals rr) {
  a = aa;
  b = bb;
  r = rr;
}

template <typename T>
inline T SQR(T t) {
  return t * t;
};

reals Sigma(Data& data, Circle& circle) {
  reals sum = 0., dx, dy;

  for (int i = 0; i < data.n; i++) {
    dx = data.X[i] - circle.a;
    dy = data.Y[i] - circle.b;
    sum += SQR(sqrt(dx * dx + dy * dy) - circle.r);
  }
  return sqrt(sum / data.n);
}

/*
      Circle fit to a given set of data points (in 2D)

      This is an algebraic fit based on the journal article

      A. Al-Sharadqah and N. Chernov, "Error analysis for circle fitting algorithms",
      Electronic Journal of Statistics, Vol. 3, pages 886-911, (2009)

      It is an algebraic circle fit with "hyperaccuracy" (with zero essential bias).
      The term "hyperaccuracy" first appeared in papers by Kenichi Kanatani around 2006

      Input:  data     - the class of data (contains the given points):

              data.n   - the number of data points
              data.X[] - the array of X-coordinates
              data.Y[] - the array of Y-coordinates

     Output:
               circle - parameters of the fitting circle:

               circle.a - the X-coordinate of the center of the fitting circle
               circle.b - the Y-coordinate of the center of the fitting circle
               circle.r - the radius of the fitting circle
               circle.s - the root mean square error (the estimate of sigma)
               circle.j - the total number of iterations

     This method combines the Pratt and Taubin fits to eliminate the essential bias.

     It works well whether data points are sampled along an entire circle or
     along a small arc.

     Its statistical accuracy is theoretically higher than that of the Pratt fit
     and Taubin fit, but practically they all return almost identical circles
     (unlike the Kasa fit that may be grossly inaccurate).

     It provides a very good initial guess for a subsequent geometric fit.

       Nikolai Chernov  (September 2012)

*/
Circle CircleFitByHyper(Data& data) {
  int i, iter, IterMAX = 99;

  float Xi, Yi, Zi;
  float Mz, Mxy, Mxx, Myy, Mxz, Myz, Mzz, Cov_xy, Var_z;
  float A0, A1, A2, A22;
  float Dy, xnew, x, ynew, y;
  float DET, Xcenter, Ycenter;

  Circle circle;

  data.means();  // Compute x- and y- sample means (via a function in the class "data")

  //     computing moments

  Mxx = Myy = Mxy = Mxz = Myz = Mzz = 0.;

  for (i = 0; i < data.n; i++) {
    Xi = data.X[i] - data.meanX;  //  centered x-coordinates
    Yi = data.Y[i] - data.meanY;  //  centered y-coordinates
    Zi = Xi * Xi + Yi * Yi;

    Mxy += Xi * Yi;
    Mxx += Xi * Xi;
    Myy += Yi * Yi;
    Mxz += Xi * Zi;
    Myz += Yi * Zi;
    Mzz += Zi * Zi;
  }
  Mxx /= data.n;
  Myy /= data.n;
  Mxy /= data.n;
  Mxz /= data.n;
  Myz /= data.n;
  Mzz /= data.n;

  //    computing the coefficients of the characteristic polynomial

  Mz = Mxx + Myy;
  Cov_xy = Mxx * Myy - Mxy * Mxy;
  Var_z = Mzz - Mz * Mz;

  A2 = 4 * Cov_xy - 3 * Mz * Mz - Mzz;
  A1 = Var_z * Mz + 4 * Cov_xy * Mz - Mxz * Mxz - Myz * Myz;
  A0 = Mxz * (Mxz * Myy - Myz * Mxy) + Myz * (Myz * Mxx - Mxz * Mxy) - Var_z * Cov_xy;
  A22 = A2 + A2;

  //    finding the root of the characteristic polynomial
  //    using Newton's method starting at x=0
  //     (it is guaranteed to converge to the right root)

  for (x = 0., y = A0, iter = 0; iter < IterMAX; iter++)  // usually, 4-6 iterations are enough
  {
    Dy = A1 + x * (A22 + 16. * x * x);
    xnew = x - y / Dy;
    if ((xnew == x) || (!isfinite(xnew))) break;
    ynew = A0 + xnew * (A1 + xnew * (A2 + 4 * xnew * xnew));
    if (abs(ynew) >= abs(y)) break;
    x = xnew;
    y = ynew;
  }

  //    computing paramters of the fitting circle

  DET = x * x - x * Mz + Cov_xy;
  Xcenter = (Mxz * (Myy - x) - Myz * Mxy) / DET / 2;
  Ycenter = (Myz * (Mxx - x) - Mxz * Mxy) / DET / 2;

  //       assembling the output

  circle.a = Xcenter + data.meanX;
  circle.b = Ycenter + data.meanY;
  circle.r = sqrt(Xcenter * Xcenter + Ycenter * Ycenter + Mz - x - x);
  circle.s = Sigma(data, circle);
  circle.i = 0;
  circle.j = iter;  //  return the number of iterations, too

  return circle;
}

// Fit a section of a circle to the given sequence of points. Output `tangent`, `radius` & `center`.
//
// If the points form  a line, `radius` will be set to infinity and `center` will be undefined.
static void FitArc(const std::deque<Vec2>& points, SinCos& tangent, float& radius, Vec2& center) {
  size_t n = points.size();

  Data data(n);
  for (size_t i = 0; i < n; ++i) {
    data.X[i] = points[i].x;
    data.Y[i] = points[i].y;
  }
  Circle circle = CircleFitByHyper(data);
  center.x = circle.a;
  center.y = circle.b;
  radius = circle.r;

  // --- Determine Tangent ---
  if (isinf(radius) || std::isnan(radius) || fabsf(radius) < 0.5_mm) {
    tangent = SinCos::FromVec2(points.back() - points.front());
    radius = std::numeric_limits<float>::infinity();
  } else {
    auto curr_dir = SinCos::FromVec2(points.back() - center);
    auto prev_dir = SinCos::FromVec2(points[points.size() - 2] - center);
    auto dir_diff = curr_dir - prev_dir;
    tangent = curr_dir + 90_deg;
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
  bool reverse_winding = false;

  if (history.size() >= 2) {
    SinCos new_tangent;
    float new_radius;
    Vec2 new_center;
    FitArc(history, new_tangent, new_radius, new_center);
    if (!isinf(radius)) {
      float min_arc_length = 2 * M_PI * radius;
      min_length = std::min(min_length, min_arc_length);
    }
    if ((tangent - new_tangent).cos < 0) {
      new_tangent = new_tangent.Opposite();
      reverse_winding = true;
    }
    tangent = new_tangent;
    radius = new_radius;
    center = new_center;
  }

  // Advance the `value` according to the last two points and current `tangent` and `curvature`.
  if (history.size() >= 2) {
    Vec2 curr = history.back();
    Vec2 prev = history[history.size() - 2];
    Vec2 vec_diff = curr - prev;
    Vec2 vec_unit = Vec2::Polar(tangent, unit_distance);
    float value_diff = VectorProjection(vec_unit, vec_diff);
    if (!isinf(radius)) {
      SinCos angle_curr = SinCos::FromVec2(curr - center);
      SinCos angle_prev = SinCos::FromVec2(prev - center);
      SinCos angle_diff = angle_curr - angle_prev;
      float angle_diff_rad = angle_diff.ToRadians();
      float angle_diff_value = angle_diff_rad / unit_angle.ToRadiansPositive();
      if (fabs(angle_diff_value) > fabs(value_diff)) {
        if (reverse_winding) {
          value_diff = -angle_diff_value;
        } else {
          value_diff = angle_diff_value;
        }
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
