#include "library.h"

#include "library_macros.h"

namespace automat {

DEFINE_PROTO(Integer);

DEFINE_PROTO(Delete);
Argument Delete::target_arg = Argument("target", Argument::kRequiresLocation);

DEFINE_PROTO(Set);
Argument Set::value_arg = Argument("value", Argument::kRequiresObject);
Argument Set::target_arg = Argument("target", Argument::kRequiresLocation);

DEFINE_PROTO(Date);

DEFINE_PROTO(Timer);

DEFINE_PROTO(TimerReset);
Argument TimerReset::timer_arg =
    Argument("timer", Argument::kRequiresConcreteType).RequireInstanceOf<Timer>();

DEFINE_PROTO(EqualityTest);
LiveArgument EqualityTest::target_arg = LiveArgument("target", Argument::kRequiresObject);

DEFINE_PROTO(LessThanTest);
LiveArgument LessThanTest::less_arg = LiveArgument("less", Argument::kRequiresObject);
LiveArgument LessThanTest::than_arg = LiveArgument("than", Argument::kRequiresObject);

DEFINE_PROTO(StartsWithTest);
LiveArgument StartsWithTest::starts_arg = LiveArgument("starts", Argument::kRequiresObject);
LiveArgument StartsWithTest::with_arg = LiveArgument("with", Argument::kRequiresObject);

DEFINE_PROTO(AllTest);
LiveArgument AllTest::test_arg = LiveArgument("test", Argument::kRequiresObject);

DEFINE_PROTO(Switch);
LiveArgument Switch::target_arg = LiveArgument("target", Argument::kRequiresObject);

DEFINE_PROTO(ErrorReporter);
LiveArgument ErrorReporter::test_arg = LiveArgument("test", Argument::kRequiresObject);
LiveArgument ErrorReporter::message_arg = LiveArgument("message", Argument::kOptional);

DEFINE_PROTO(Parent);

DEFINE_PROTO(HealthTest);
Argument HealthTest::target_arg = Argument("target", Argument::kOptional);

DEFINE_PROTO(ErrorCleaner);
Argument ErrorCleaner::target_arg = Argument("target", Argument::kOptional);

DEFINE_PROTO(Append);
Argument Append::to_arg =
    Argument("to", Argument::kRequiresConcreteType).RequireInstanceOf<AbstractList>();
Argument Append::what_arg = Argument("what", Argument::kRequiresObject);

DEFINE_PROTO(List);

DEFINE_PROTO(Filter);
LiveArgument Filter::list_arg =
    LiveArgument("list", Argument::kRequiresConcreteType).RequireInstanceOf<AbstractList>();
LiveArgument Filter::element_arg =
    LiveArgument("element", Argument::kRequiresConcreteType).RequireInstanceOf<CurrentElement>();
LiveArgument Filter::test_arg("test", Argument::kRequiresObject);

DEFINE_PROTO(CurrentElement);
LiveArgument CurrentElement::of_arg =
    LiveArgument("of", Argument::kRequiresConcreteType).RequireInstanceOf<Iterator>();

DEFINE_PROTO(Complex);

DEFINE_PROTO(ComplexField);
LiveArgument ComplexField::complex_arg =
    LiveArgument("complex", Argument::kRequiresConcreteType).RequireInstanceOf<Complex>();
LiveArgument ComplexField::label_arg("label", Argument::kRequiresObject);

DEFINE_PROTO(Text);
LiveArgument Text::target_arg("target", Argument::kOptional);

DEFINE_PROTO(Button);
Argument Button::enabled_arg("enabled", Argument::kOptional);

DEFINE_PROTO(ComboBox);
LiveArgument ComboBox::options_arg("option", Argument::kOptional);

DEFINE_PROTO(Slider);
LiveArgument Slider::min_arg("min", Argument::kOptional);
LiveArgument Slider::max_arg("max", Argument::kOptional);

DEFINE_PROTO(ProgressBar);

DEFINE_PROTO(ListView);
LiveArgument ListView::list_arg =
    LiveArgument("list", Argument::kRequiresConcreteType).RequireInstanceOf<AbstractList>();

DEFINE_PROTO(Blackboard);

DEFINE_PROTO(BlackboardUpdater);

}  // namespace automat