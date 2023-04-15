#include "run_button.h"

#include "base.h"
#include "gui_shape_widget.h"
#include "log.h"

namespace automaton::gui {

constexpr char kPlayShape[] = "m-4.5-7.8c-.65 1.7-.53 14 0 16 1.8-.48 12-6.3 "
                              "14-7.8-1.3-1.4-12-7.5-14-7.8z";

RunButton::RunButton(Location *parent)
    : Button(parent, MakeShapeWidget(kPlayShape, 0xa0ffffff)) {}

void RunButton::Activate() { LOG() << "RunButton::Activate"; }

} // namespace automaton::gui