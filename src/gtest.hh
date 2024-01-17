#include <gtest/gtest.h> // IWYU pragma: export

#pragma maf add link argument "-lgmock"
#pragma maf add link argument "-lgtest"
#pragma maf add run argument "--gtest_color=yes"
#pragma maf main