#pragma once

#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/gpu/GrDirectContext.h>

#include "virtual_fs.hh"

namespace automat {

// Pass non-null GrDirectContext to create GPU-backed image (MUCH cheaper to draw).
sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset, GrDirectContext*);

constexpr static SkSamplingOptions kDefaultSamplingOptions =
    SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);

}  // namespace automat