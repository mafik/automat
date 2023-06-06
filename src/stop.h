#include <optional>

namespace automat {

struct Stop {};

using MaybeStop = std::optional<Stop>;

}  // namespace automat