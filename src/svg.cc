// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "svg.hh"

#include <include/core/SkStream.h>
#include <include/utils/SkParsePath.h>
// #include <modules/skshaper/utils/FactorHelpers.h>
#include <modules/svg/include/SkSVGDOM.h>

#include "font.hh"
#include "log.hh"

#pragma maf add link argument "-lsvg"

namespace automat {

SkPath PathFromSVG(const char svg[], SVGUnit unit) {
  SkPath path;
  if (!SkParsePath::FromSVGString(svg, &path)) {
    LOG << "Failed to parse SVG path: " << svg;
  }
  constexpr float kScalePixels96DPI = 0.0254f / 96;
  constexpr float kScaleMilimeters = 0.001f;
  if (unit == SVGUnit_Pixels96DPI) {
    path = path.makeScale(kScalePixels96DPI, -kScalePixels96DPI);
  } else if (unit == SVGUnit_Millimeters) {
    path = path.makeScale(kScaleMilimeters, -kScaleMilimeters);
  }
  path.updateBoundsCache();
  return path;
}

sk_sp<SkSVGDOM> SVGFromAsset(StrView svg_contents) {
  SkMemoryStream stream = SkMemoryStream(svg_contents.data(), svg_contents.size());

  // Viewer should have already registered the codecs necessary for DataURIResourceProviderProxy
  auto predecode = skresources::ImageDecodeStrategy::kPreDecode;
  SkString dir = SkString(".");
  auto rp = skresources::DataURIResourceProviderProxy::Make(
      skresources::FileResourceProvider::Make(dir, predecode), predecode, gui::GetFontMgr());

  auto dom = SkSVGDOM::Builder()
                 .setFontManager(gui::GetFontMgr())
                 .setResourceProvider(std::move(rp))
                 //.setTextShapingFactory(SkShapers::BestAvailable())
                 .make(stream);

  return dom;
}

}  // namespace automat