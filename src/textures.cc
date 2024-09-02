#include "textures.hh"

#include <include/core/SkData.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/SkImageGanesh.h>

namespace automat {

sk_sp<SkImage> MakeImageFromAsset(maf::fs::VFile& asset, GrDirectContext* gr_ctx) {
  auto& content = asset.content;
  auto data = SkData::MakeWithoutCopy(content.data(), content.size());
  auto image = SkImages::DeferredFromEncodedData(data);
  if (gr_ctx) {
    image = image->withDefaultMipmaps();
    image = SkImages::TextureFromImage(gr_ctx, image.get(), skgpu::Mipmapped::kYes);
  }
  return image;
}
}  // namespace automat