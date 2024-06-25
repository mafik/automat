#include "double_ptr.hh"

#include <vector>

#include "gtest.hh"
#include "optional.hh"

using namespace maf;
using namespace std;

class DoublePtrTest : public ::testing::Test {
 public:
  enum class Event {
    ValueCreated,
    ValueMoved,
    ValueDestroyed,
  };

  vector<Event> events;

 protected:
  void SetUp() override { events.clear(); }
};

using Event = DoublePtrTest::Event;

struct Value {
  DoublePtrTest* test;
  Value() : test(nullptr) {}
  Value(Value&& other) = delete;
  Value(const Value& other) = delete;
  void SetTest(DoublePtrTest* test) {
    this->test = test;
    if (test) {
      test->events.push_back(Event::ValueCreated);
    }
  }
  ~Value() {
    if (test) test->events.push_back(Event::ValueDestroyed);
  }
};

TEST_F(DoublePtrTest, BasicTest) {
  Optional<DoublePtr<Value>> ptr1;
  Optional<DoublePtr<Value>> ptr2;
  ptr1.emplace();
  ptr2.emplace();
  EXPECT_EQ(ptr1->Find(*ptr2), nullptr);
  EXPECT_THAT(events, testing::ElementsAre());
  ptr1->FindOrMake(*ptr2).first.SetTest(this);
  EXPECT_THAT(events, testing::ElementsAre(Event::ValueCreated));
  ptr2.reset();
  EXPECT_THAT(events, testing::ElementsAre(Event::ValueCreated, Event::ValueDestroyed));
  ptr1.reset();
  EXPECT_THAT(events, testing::ElementsAre(Event::ValueCreated, Event::ValueDestroyed));
}