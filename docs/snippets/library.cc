// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library.hh"

namespace automat {

Argument Delete::target_arg = Argument("target", Argument::kRequiresLocation);

Argument Set::value_arg = Argument("value", Argument::kRequiresObject);
Argument Set::target_arg = Argument("target", Argument::kRequiresLocation);

Argument TimerReset::timer_arg =
    Argument("timer", Argument::kRequiresConcreteType).RequireInstanceOf<Timer>();

LiveArgument EqualityTest::target_arg = LiveArgument("target", Argument::kRequiresObject);

LiveArgument LessThanTest::less_arg = LiveArgument("less", Argument::kRequiresObject);
LiveArgument LessThanTest::than_arg = LiveArgument("than", Argument::kRequiresObject);

LiveArgument StartsWithTest::starts_arg = LiveArgument("starts", Argument::kRequiresObject);
LiveArgument StartsWithTest::with_arg = LiveArgument("with", Argument::kRequiresObject);

LiveArgument AllTest::test_arg = LiveArgument("test", Argument::kRequiresObject);

LiveArgument Switch::target_arg = LiveArgument("target", Argument::kRequiresObject);

LiveArgument ErrorReporter::test_arg = LiveArgument("test", Argument::kRequiresObject);
LiveArgument ErrorReporter::message_arg = LiveArgument("message", Argument::kOptional);

Argument HealthTest::target_arg = Argument("target", Argument::kOptional);

Argument ErrorCleaner::target_arg = Argument("target", Argument::kOptional);

Argument Append::to_arg =
    Argument("to", Argument::kRequiresConcreteType).RequireInstanceOf<AbstractList>();
Argument Append::what_arg = Argument("what", Argument::kRequiresObject);

LiveArgument Filter::list_arg =
    LiveArgument("list", Argument::kRequiresConcreteType).RequireInstanceOf<AbstractList>();
LiveArgument Filter::element_arg =
    LiveArgument("element", Argument::kRequiresConcreteType).RequireInstanceOf<CurrentElement>();
LiveArgument Filter::test_arg("test", Argument::kRequiresObject);

LiveArgument CurrentElement::of_arg =
    LiveArgument("of", Argument::kRequiresConcreteType).RequireInstanceOf<Iterator>();

LiveArgument ComplexField::complex_arg =
    LiveArgument("complex", Argument::kRequiresConcreteType).RequireInstanceOf<Complex>();
LiveArgument ComplexField::label_arg("label", Argument::kRequiresObject);

LiveArgument Text::target_arg("target", Argument::kOptional);

Argument Button::enabled_arg("enabled", Argument::kOptional);

LiveArgument ComboBox::options_arg("option", Argument::kOptional);

LiveArgument Slider::min_arg("min", Argument::kOptional);
LiveArgument Slider::max_arg("max", Argument::kOptional);

LiveArgument ListView::list_arg =
    LiveArgument("list", Argument::kRequiresConcreteType).RequireInstanceOf<AbstractList>();

Argument BlackboardUpdater::const_arg = Argument("const", Argument::kOptional);

}  // namespace automat