#pragma once

#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>

#include "virtual_fs.hh"

namespace automat {

sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset);

constexpr static SkSamplingOptions kDefaultSamplingOptions =
    SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);

}  // namespace automat