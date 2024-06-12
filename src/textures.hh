#pragma once

#include <include/core/SkImage.h>

#include "virtual_fs.hh"

namespace automat {

sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset);

}  // namespace automat