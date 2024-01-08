#include "library_mouse_click.hh"

#include <include/core/SkImage.h>

#include "../build/generated/embedded.hh"
#include "argument.hh"
#include "drag_action.hh"
#include "include/core/SkSamplingOptions.h"
#include "library_macros.hh"

using namespace maf;

namespace automat::library {

DEFINE_PROTO(MouseLeftClick);

static sk_sp<SkImage>& MouseBaseImage() {
  static auto mouse_base_image = []() {
    auto& content = embedded::assets_mouse_base_webp.content;
    auto data = SkData::MakeWithoutCopy(content.data(), content.size());
    auto image = SkImages::DeferredFromEncodedData(data);
    return image;
  }();
  return mouse_base_image;
}

constexpr float kScale = 0.00005;

MouseLeftClick::MouseLeftClick() {}
string_view MouseLeftClick::Name() const { return "MouseLeftClick"sv; }
std::unique_ptr<Object> MouseLeftClick::Clone() const { return std::make_unique<MouseLeftClick>(); }
void MouseLeftClick::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto& mouse_base_image = MouseBaseImage();
  canvas.save();
  canvas.scale(kScale, -kScale);
  canvas.translate(0, -mouse_base_image->height());
  SkSamplingOptions sampling = SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
  canvas.drawImage(mouse_base_image, 0, 0, sampling);
  canvas.restore();
}
SkPath MouseLeftClick::Shape() const {
  auto& mouse_base_image = MouseBaseImage();
  return SkPath::Rect(SkRect::MakeXYWH(0, 0, mouse_base_image->width() * kScale,
                                       mouse_base_image->height() * kScale));
}
std::unique_ptr<Action> MouseLeftClick::ButtonDownAction(gui::Pointer& pointer,
                                                         gui::PointerButton btn) {
  if (btn != gui::PointerButton::kMouseLeft) {
    return nullptr;
  }
  auto& path = pointer.Path();
  if (path.size() < 2) {
    return nullptr;
  }
  auto* parent = path[path.size() - 2];
  Location* location = dynamic_cast<Location*>(parent);
  if (!location) {
    return nullptr;
  }
  std::unique_ptr<DragLocationAction> action = std::make_unique<DragLocationAction>(location);
  action->contact_point = pointer.PositionWithin(*this);
  LOG << "Action contact point is " << action->contact_point;
  return action;
}

void MouseLeftClick::Args(std::function<void(Argument&)> cb) { cb(then_arg); }

}  // namespace automat::library