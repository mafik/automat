#include "math.hh"

#include "format.hh"

using namespace maf;

std::string Vec2::ToStr() const { return f("Vec2(%f, %f)", x, y); }
std::string Vec3::ToStr() const { return f("Vec3(%f, %f, %f)", x, y, z); }
