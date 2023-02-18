import <gtest/gtest.h>;
import <gmock/gmock.h>;
import <gmock/gmock-more-matchers.h>;
import <chrono>;
import <source_location>;
import backtrace;
import base;
import library;
import test_base;
import error;
import log;

using namespace automaton;
using namespace testing;

TEST(CounterTest, Count) {
  EnableBacktraceOnSIGSEGV();
  Handle root(nullptr);
  Machine &counter = *root.Create<Machine>();

  Handle &i = counter.Create<Integer>();
  Handle &inc = counter.Create<Increment>();
  inc.ConnectTo(i, "target");
  Handle &txt = counter.Create<Text>("Count");
  txt.ConnectTo(i, "target");
  Handle &btn = counter.Create<Button>("Increment");
  btn.ConnectTo(inc, "click");

  counter.AddToFrontPanel(txt);
  counter.AddToFrontPanel(btn);

  // Verify that the front panel contains two widgets
  ASSERT_EQ(counter.front.size(), 2);
  EXPECT_EQ(counter.front[0], &txt);
  EXPECT_EQ(counter.front[1], &btn);

  ASSERT_EQ(counter["Count"], &txt);
  ASSERT_EQ(counter["Increment"], &btn);

  RunLoop();

  EXPECT_EQ(counter["Count"]->GetText(), "0");

  counter["Increment"]->Run();
  RunLoop();

  EXPECT_EQ(counter["Count"]->GetText(), "1");
}

void ExpectHealthy(Machine &m) {
  std::vector<std::string> errors;
  m.Diagnostics([&](Handle *h, Error &e) { errors.push_back(e.text); });
  EXPECT_THAT(errors, IsEmpty());
}

void ExpectErrors(Machine &m, std::vector<std::string> errors) {
  std::vector<std::string> actual_errors;
  m.Diagnostics([&](Handle *h, Error &e) { actual_errors.push_back(e.text); });
  EXPECT_THAT(actual_errors, UnorderedElementsAreArray(errors));
}

void ExpectAlerts(Alert &a, std::vector<std::string> alerts) {
  EXPECT_THAT(a.alerts_for_tests, UnorderedElementsAreArray(alerts));
}

void ClearErrors(Machine &m) {
  for (auto c : m.children_with_errors) {
    c->ClearError();
  }
}

TEST(TemperatureConverterTest, Conversion) {
  EnableBacktraceOnSIGSEGV();
  Handle root(nullptr);
  Machine &converter = *root.Create<Machine>();

  Handle &c_txt = converter.Create<Text>("Celsius");
  Handle &f_txt = converter.Create<Text>("Fahrenheit");

  converter.AddToFrontPanel(c_txt);
  converter.AddToFrontPanel(f_txt);

  Handle &c = converter.Create<Integer>("C");
  Handle &f = converter.Create<Integer>("F");

  c_txt.ConnectTo(c, "target");
  f_txt.ConnectTo(f, "target");

  Handle &blackboard = converter.Create<Blackboard>();
  blackboard.SetText("F = C * 9 / 5 + 32");

  converter.Create<BlackboardUpdater>();

  converter["Celsius"]->SetText("5");
  RunLoop();
  EXPECT_EQ(converter["Fahrenheit"]->GetText(), "41");

  converter["Fahrenheit"]->SetText("50");
  RunLoop();
  EXPECT_EQ(converter["Celsius"]->GetText(), "10");

  ExpectHealthy(converter);
}

struct FlightBookerTest {
  Handle root;
  Machine &booker;

  Handle *c;  // ComboBox C
  Handle *t1; // Text T1
  Handle *t2; // Text T2
  Handle *b;  // Button B

  Handle *one_way;       // Text "one-way flight"
  Handle *return_flight; // Text "return flight"

  Handle *t1_date; // Date
  Handle *t2_date; // Date

  Handle *t2_enabled;     // EqualityTest
  Handle *t2_before_t1;   // LessThanTest
  Handle *all_test;       // AllTest
  Handle *health_test;    // HealthTest
  Handle *error_message;  // Text "Return flight date must be..."
  Handle *error_reporter; // ErrorReporter

  Handle *alert;
  Handle *switch_;
  Handle *one_way_message;
  Handle *return_flight_message;

  FlightBookerTest() : root(nullptr), booker(*root.Create<Machine>()) {
    c = &booker.Create<ComboBox>("C");
    t1 = &booker.Create<Text>("T1");
    t2 = &booker.Create<Text>("T2");
    b = &booker.Create<Button>("B");
    b->SetText("Book");

    one_way = &booker.Create<Text>("one-way flight");
    one_way->SetText("one-way flight");
    return_flight = &booker.Create<Text>("return flight");
    return_flight->SetText("return flight");

    c->ConnectTo(*one_way, "option");
    c->ConnectTo(*return_flight, "option");

    booker.AddToFrontPanel(*c);
    booker.AddToFrontPanel(*t1);
    booker.AddToFrontPanel(*t2);
    booker.AddToFrontPanel(*b);

    // T2 is enabled iff C’s value is “return flight”.
    t2_enabled = &booker.Create<EqualityTest>("T2 enabled");
    t2_enabled->ConnectTo(*c, "target");
    t2_enabled->ConnectTo(*return_flight, "target");
    t2->ConnectTo(*t2_enabled, "enabled");

    // When there is an error, B is disabled.
    health_test = &booker.Create<HealthTest>();
    b->ConnectTo(*health_test, "enabled");

    // Report an error when C’s value is “return flight” & T2’s date is strictly
    // before T1.
    t1_date = &booker.Create<Date>("T1");
    t1->ConnectTo(*t1_date, "target");
    t2_date = &booker.Create<Date>("T2");
    t2->ConnectTo(*t2_date, "target");
    t2_before_t1 = &booker.Create<LessThanTest>();
    t2_before_t1->ConnectTo(*t2_date, "less");
    t2_before_t1->ConnectTo(*t1_date, "than");
    all_test = &booker.Create<AllTest>();
    all_test->ConnectTo(*t2_before_t1, "test");
    all_test->ConnectTo(*t2_enabled, "test");
    error_message = &booker.Create<Text>("Error message");
    error_message->SetText("Return flight date must be after departure date.");
    error_reporter = &booker.Create<ErrorReporter>();
    error_reporter->ConnectTo(*error_message, "message");
    error_reporter->ConnectTo(*all_test, "test");
    error_reporter->ConnectTo(*t2, "target");

    // When a non-disabled textfield T has an ill-formatted date then T is
    // colored red and B is disabled.

    // When clicking B a message is displayed informing the user of his
    // selection (e.g. “You have booked a one-way flight on 04.04.2014.”). (B)
    // -then-> (alert) -message-> (switch) -target-> (C)
    //                                     \-{one-way flight}-> Formatter
    //                                      \-{return flight}-> Formatter
    alert = &booker.Create<Alert>();
    b->ConnectTo(*alert, "click");
    switch_ = &booker.Create<Switch>();
    alert->ConnectTo(*switch_, "message");
    switch_->ConnectTo(*c, "target");
    one_way_message = &booker.Create<Text>();
    one_way_message->SetText("You have booked a one-way flight on {T1}.");
    switch_->ConnectTo(*one_way_message, one_way->GetText());
    return_flight_message = &booker.Create<Text>();
    return_flight_message->SetText(
        "You have booked a return flight on {T1} and {T2}.");
    switch_->ConnectTo(*return_flight_message, return_flight->GetText());

    // Initially, C has the value “one-way flight” and T1 as well as T2 have the
    // same (arbitrary) date (it is implied that T2 is disabled).
    c->SetText(one_way->GetText());
    t1->SetText("2014-04-04");
    t2->SetText("2014-04-04");

    RunLoop();
    ExpectHealthy(booker);
  }

  Alert &GetAlert() { return *alert->As<Alert>(); }
};

TEST(FlightBookerTest, DefaultValues) {
  EnableBacktraceOnSIGSEGV();
  FlightBookerTest x;

  // Initial values should produce "You have booked a one-way flight on
  // 2014-04-04."
  x.b->ScheduleRun();
  RunLoop();
  ExpectHealthy(x.booker);
  ExpectAlerts(x.GetAlert(),
               {"You have booked a one-way flight on 2014-04-04."});
}

TEST(FlightBookerTest, ReturnFlight) {
  EnableBacktraceOnSIGSEGV();
  FlightBookerTest x;
  // Change C to "return flight" and T2 to "2014-04-05".
  x.c->SetText(x.return_flight->GetText());
  x.t2->SetText("2014-04-05");
  x.b->ScheduleRun();
  RunLoop();
  ExpectHealthy(x.booker);
  ExpectAlerts(
      x.GetAlert(),
      {"You have booked a return flight on 2014-04-04 and 2014-04-05."});
}

TEST(FlightBookerTest, TimeTravelError) {
  EnableBacktraceOnSIGSEGV();
  FlightBookerTest x;
  // Change T2 to "2014-04-03".
  x.c->SetText(x.return_flight->GetText());
  x.t2->SetText("2014-04-03");
  RunLoop();
  x.b->ScheduleRun();
  RunLoop();
  ExpectErrors(x.booker, {"Return flight date must be after departure date.",
                          "Button is disabled."});
  ExpectAlerts(x.GetAlert(), {});
}

TEST(FlightBookerTest, OneWayTimeTravelOk) {
  EnableBacktraceOnSIGSEGV();
  FlightBookerTest x;

  // Change C to "one-way flight".
  x.t2->SetText("2014-04-03");
  x.b->ScheduleRun();
  RunLoop();
  ExpectHealthy(x.booker);
  ExpectAlerts(x.GetAlert(),
               {"You have booked a one-way flight on 2014-04-04."});
}

TEST(FlightBookerTest, BadDateFormat) {
  EnableBacktraceOnSIGSEGV();
  FlightBookerTest x;

  // Change T1 to "2014-04-04-".
  x.t1->SetText("2014-04-04-");
  x.b->ScheduleRun();
  RunLoop();
  ExpectErrors(x.booker,
               {"Invalid date format. The Date object expects dates in the "
                "format YYYY-MM-DD. The provided date was: 2014-04-04-.",
                "Button is disabled."});
  ExpectAlerts(x.GetAlert(), {});
}

TEST(TimerTest, DurationChange) {
  EnableBacktraceOnSIGSEGV();
  using namespace std::chrono_literals;
  Handle root(nullptr);
  Machine &m = *root.Create<Machine>();

  Handle &min = m.Create<Number>("min");
  min.SetNumber(5);
  Handle &max = m.Create<Number>("max");
  max.SetNumber(15);

  Handle &duration = m.Create<Slider>("duration");
  duration.SetNumber(10);

  Handle &timer = m.Create<Timer>("T");
  FakeTime fake_time;
  fake_time.SetNow(TimePoint(0s));
  timer.As<Timer>()->fake_time = &fake_time;

  Handle &timer_reset = m.Create<TimerReset>();
  timer_reset.ConnectTo(timer, "timer");
  Handle &reset_button = m.Create<Button>("reset");
  reset_button.ConnectTo(timer_reset, "click");

  Handle &progress_bar = m.Create<ProgressBar>("progress");

  Handle &blackboard = m.Create<Blackboard>();
  blackboard.SetText("progress = T / duration");
  blackboard.ConnectTo(timer, "const");
  blackboard.ConnectTo(duration, "const");
  m.Create<BlackboardUpdater>();

  reset_button.ScheduleRun();
  RunLoop();
  EXPECT_EQ(0, progress_bar.GetNumber()) << "Initial progress is wrong";

  fake_time.SetNow(TimePoint(5s));
  RunLoop();
  EXPECT_EQ(0.5, progress_bar.GetNumber())
      << "Progress after 5 seconds is wrong";

  duration.SetNumber(5);
  RunLoop();
  EXPECT_EQ(1, progress_bar.GetNumber())
      << "Progress after reducing duration is wrong";

  ExpectHealthy(m);
}

// CRUD
// ====
// Challenges: separating the domain and presentation logic, managing mutation,
// building a non-trivial layout.
//
// The task is to build a frame containing the following elements: a textfield
// Tprefix, a pair of textfields Tname and Tsurname, a listbox L, buttons BC, BU
// and BD and the three labels as seen in the screenshot. L presents a view of
// the data in the database that consists of a list of names. At most one entry
// can be selected in L at a time. By entering a string into Tprefix the user
// can filter the names whose surname start with the entered prefix—this should
// happen immediately without having to submit the prefix with enter. Clicking
// BC will append the resulting name from concatenating the strings in Tname and
// Tsurname to L. BU and BD are enabled iff an entry in L is selected. In
// contrast to BC, BU will not append the resulting name but instead replace the
// selected entry with the new name. BD will remove the selected entry. The
// layout is to be done like suggested in the screenshot. In particular, L must
// occupy all the remaining space.
//
// CRUD (Create, Read, Update and Delete) represents a typical graphical
// business application. The primary challenge is the separation of domain and
// presentation logic in the source code that is more or less forced on the
// implementer due to the ability to filter the view by a prefix. Traditionally,
// some form of MVC pattern is used to achieve the separation of domain and
// presentation logic. Also, the approach to managing the mutation of the list
// of names is tested. A good solution will have a good separation between the
// domain and presentation logic without much overhead (e.g. in the form of
// toolkit specific concepts or language/paradigm concepts), a mutation
// management that is fast but not error-prone and a natural representation of
// the layout (layout builders are allowed, of course, but would increase the
// overhead).

struct CrudTest : public TestBase {
  Handle &list = machine.Create<List>("list");

  Handle &first_name_label = machine.Create<Text>("First name label");
  Handle &last_name_label = machine.Create<Text>("Last name label");

  Handle &text_prefix = machine.Create<Text>("Prefix");
  Handle &starts_with_test = machine.Create<StartsWithTest>();
  Handle &field_for_test = machine.Create<ComplexField>("Field for test");
  Handle &element = machine.Create<CurrentElement>();
  Handle &filter = machine.Create<Filter>();

  Handle &list_view = machine.Create<ListView>();
  Handle &deleter = machine.Create<Delete>();
  Handle &button_delete = machine.Create<Button>();

  Handle &first_name_selected_field =
      machine.Create<ComplexField>("First name selected");
  Handle &last_name_selected_field =
      machine.Create<ComplexField>("Last name selected");

  Handle &set_first_name = machine.Create<Set>("Set first name");
  Handle &set_last_name = machine.Create<Set>("Set last name");
  Handle &button_update = machine.Create<Button>("Update");

  Handle &first_name_complex_field =
      machine.Create<ComplexField>("First name complex");
  Handle &last_name_complex_field =
      machine.Create<ComplexField>("Last name complex");
  Handle &complex = machine.Create<Complex>();

  Handle &set_complex = machine.Create<Set>("Set complex");
  Handle &button_create = machine.Create<Button>("Create");
  Handle &append_target = machine.CreateEmpty();
  Handle &append = machine.Create<Append>();

  CrudTest() {
    EnableBacktraceOnSIGSEGV();

    first_name_label.SetText("First Name");
    last_name_label.SetText("Last Name");

    filter.ConnectTo(list, "list");
    filter.ConnectTo(starts_with_test, "test");
    filter.ConnectTo(element, "element");
    filter.ObserveUpdates(text_prefix);

    element.ConnectTo(filter, "of");

    field_for_test.ConnectTo(element, "complex");
    field_for_test.ConnectTo(last_name_label, "label");
    starts_with_test.ConnectTo(field_for_test, "starts");
    starts_with_test.ConnectTo(text_prefix, "with");

    list_view.ConnectTo(filter, "list");

    deleter.ConnectTo(list_view, "target");
    button_delete.ConnectTo(deleter, "click");

    first_name_selected_field.ConnectTo(list_view, "complex");
    first_name_selected_field.ConnectTo(first_name_label, "label");
    last_name_selected_field.ConnectTo(list_view, "complex");
    last_name_selected_field.ConnectTo(last_name_label, "label");

    set_first_name.ConnectTo(first_name_selected_field, "target");
    set_first_name.ConnectTo(first_name_complex_field, "value");
    set_last_name.ConnectTo(last_name_selected_field, "target");
    set_last_name.ConnectTo(last_name_complex_field, "value");
    button_update.ConnectTo(set_first_name, "click");
    button_update.ConnectTo(set_last_name, "click");

    first_name_complex_field.ConnectTo(complex, "complex");
    first_name_complex_field.ConnectTo(first_name_label, "label");
    last_name_complex_field.ConnectTo(complex, "complex");
    last_name_complex_field.ConnectTo(last_name_label, "label");
    first_name_complex_field.Put(Create<Text>());
    last_name_complex_field.Put(Create<Text>());

    button_create.ConnectTo(set_complex, "click");
    set_complex.ConnectTo(complex, "value");
    set_complex.ConnectTo(append_target, "target");
    set_complex.ConnectTo(append, "then");

    append.ConnectTo(append_target, "what");
    append.ConnectTo(list, "to");

    RunLoop();
  }

  std::vector<std::pair<std::string, std::string>> FilterContents() {
    auto objects = filter.As<Filter>()->objects;
    std::vector<std::pair<std::string, std::string>> result;
    for (Object *o : objects) {
      Complex *c = dynamic_cast<Complex *>(o);
      if (c == nullptr) {
        continue;
      }
      Object *first_name_object = c->objects["First Name"].get();
      Object *last_name_object = c->objects["Last Name"].get();
      if (first_name_object == nullptr || last_name_object == nullptr) {
        continue;
      }
      std::string first_name = first_name_object->GetText();
      std::string last_name = last_name_object->GetText();
      result.push_back(std::make_pair(first_name, last_name));
    }
    return result;
  }

  void AddEntry(const std::string &first_name, const std::string &last_name) {
    first_name_complex_field.SetText(first_name);
    last_name_complex_field.SetText(last_name);
    button_create.ScheduleRun();
    RunLoop();
  }
};

TEST_F(CrudTest, Filter) {

  // Add two entries & verify that they appear in the filtered list.

  AddEntry("John", "Doe");
  AddEntry("Marek", "Rogalski");

  EXPECT_THAT(FilterContents(),
              ElementsAre(Pair("John", "Doe"), Pair("Marek", "Rogalski")));

  // Change filter prefix & verify that only one entry remains in the filtered
  // list.

  text_prefix.SetText("Rog");
  RunLoop();

  EXPECT_THAT(FilterContents(), ElementsAre(Pair("Marek", "Rogalski")));

  field_for_test.ClearError();
  ExpectHealthy(machine);
}

TEST_F(CrudTest, Delete) {
  // Add two entries & verify that they appear in the filtered list.
  AddEntry("John", "Doe");
  AddEntry("Marek", "Rogalski");
  EXPECT_THAT(FilterContents(),
              ElementsAre(Pair("John", "Doe"), Pair("Marek", "Rogalski")));

  // Select the first entry (just like a user would do in a GUI).
  ListView *lv = list_view.ThisAs<ListView>();
  ASSERT_NE(lv, nullptr);
  lv->Select(0);

  // Click the delet button & verify that the entry is gone.
  deleter.ScheduleRun();
  RunLoop();
  EXPECT_THAT(FilterContents(), ElementsAre(Pair("Marek", "Rogalski")));
}

TEST_F(CrudTest, Update) {
  AddEntry("Foo", "Bar");
  EXPECT_THAT(FilterContents(), ElementsAre(Pair("Foo", "Bar")));

  // Text view for selected last name is empty before an element is selected.
  EXPECT_EQ(last_name_selected_field.GetText(), "");

  // After selecting the first element, the text view is updated
  list_view.ThisAs<ListView>()->Select(0);
  EXPECT_EQ(last_name_selected_field.GetText(), "Bar");

  // After typing a new last name in the temp object, the seleceted last name is still the same.
  last_name_complex_field.SetText("Baz");
  EXPECT_EQ(last_name_selected_field.GetText(), "Bar");

  // After clicking the update button, the selected last name is updated.
  button_update.ScheduleRun();
  RunLoop();
  EXPECT_EQ(last_name_selected_field.GetText(), "Baz");
}