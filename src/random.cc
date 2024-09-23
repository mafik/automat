#include "random.hh"
#include "int.hh"

#include <sys/random.h>

std::random_device rand_dev;
std::mt19937 generator(rand_dev());

namespace maf {

void RandomBytesSecure(Span<> out) {
  SSize n = getrandom(out.data(), out.size(), 0);
}

} // namespace maf