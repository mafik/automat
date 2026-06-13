#include "make_object_option.hpp"

#include "drag_action.hpp"
#include "embedded.hpp"
#include "location.hpp"
#include "object.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "vm.hpp"
#include "widget.hpp"

namespace automat {
std::unique_ptr<ui::Widget> MakeObjectOption::MakeIcon(ui::Widget* parent) {
  auto new_icon = proto->MakeToy(parent);
  icon = new_icon.get();
  return new_icon;
}
std::unique_ptr<Action> MakeObjectOption::Activate(ui::Pointer& pointer) const {
  // Idea: reuse the `icon` widget.
  // The icon is the right widget type for the given proto, so theoretically it could be
  // reattached to the newly cloned object.
  // This could be even handled by the "Create" method - it could take existing widget to "adopt".
  auto loc = MAKE_PTR(Location, vm.root_location);
  auto obj = proto->Clone();
  pointer.root_widget.toys.FindOrMake(*obj, icon.get());
  loc->InsertHere(std::move(obj));
  audio::Play(embedded::assets_SFX_toolbar_pick_wav);
  auto action = std::make_unique<DragLocationAction>(pointer, std::move(loc));
  // Resetting the anchor makes the object dragged by the center point
  if (action->locations.front()->widget)
    action->locations.front()->widget->local_anchor = Vec2(0, 0);
  return action;
}
std::unique_ptr<Option> MakeObjectOption::Clone() const {
  return std::make_unique<MakeObjectOption>(proto, dir);
}
MakeObjectOption::MakeObjectOption(Ptr<Object> proto, Dir dir)
    : proto(proto), dir(dir), icon(nullptr) {}
}  // namespace automat