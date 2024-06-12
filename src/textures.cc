#include "textures.hh"

#include <include/core/SkData.h>

namespace automat {

sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset) {
  auto& content = asset.content;
  auto data = SkData::MakeWithoutCopy(content.data(), content.size());
  auto image = SkImages::DeferredFromEncodedData(data);
  return image;
}
}  // namespace automat