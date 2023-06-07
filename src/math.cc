#include "math.hh"

#include "format.hh"

std::string Vec2::LoggableString() const { return f("Vec2(%f, %f)", x, y); }
