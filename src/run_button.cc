#include "run_button.h"

#include "base.h"
#include "gui_shape_widget.h"
#include "log.h"

namespace automaton::gui {

constexpr char kPlayShape[] =
    "M-5-8C-5.8-6-5.7 6-5 8-3 7.7 7.5 1.5 9 0 7.5-1.5-3-7.7-5-8Z";

RunButton::RunButton(Location *parent)
    : Button(parent, MakeShapeWidget(kPlayShape, 0xa0ffffff)) {}

void RunButton::Activate() { LOG() << "RunButton::Activate"; }

} // namespace automaton::gui