// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/gpu/GrDirectContext.h>

#include "virtual_fs.hh"
#include "widget.hh"

namespace automat {

// Pass non-null DrawContext to create GPU-backed image (MUCH cheaper to draw).
sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset, gui::DrawContext*);

sk_sp<SkImage> CacheImage(gui::DrawContext& ctx, const maf::Str& key,
                          std::function<sk_sp<SkImage>()> generator);

constexpr static SkSamplingOptions kDefaultSamplingOptions =
    SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);

}  // namespace automat