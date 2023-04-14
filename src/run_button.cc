#include "run_button.h"

#include "base.h"
#include "generated/assets.h"
#include "log.h"

namespace automaton::gui {

RunButton::RunButton(Location *parent)
    : Button(parent, skottie::Animation::Make(assets::play_json,
                                              assets::play_json_size)) {}

void RunButton::Activate() { LOG() << "RunButton::Activate"; }

} // namespace automaton::gui