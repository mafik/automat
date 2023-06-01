#include "base.h"
#include "library.h"
#include "test_base.h"
#include <gtest/gtest.h>
#include <memory>

using namespace automat;

struct ListTest : TestBase {
  Location &list = machine.Create<List>();

  ListTest() {
    auto l = list.As<List>();
    for (int i = 0; i < 10; ++i) {
      std::unique_ptr<Object> obj = Create<Integer>();
      dynamic_cast<Integer *>(obj.get())->i = i;
      l->objects.emplace_back(std::move(obj));
    }
  }
};

TEST_F(ListTest, Filter) {
  Location &filter = machine.Create<Filter>();
  filter.ConnectTo(list, "list");
  Location &test = machine.Create<LessThanTest>();
  filter.ConnectTo(test, "test");
  Location &treshold = machine.Create<Integer>();
  treshold.As<Integer>()->i = 5;
  test.ConnectTo(treshold, "than");
  Location &element = machine.Create<CurrentElement>();
  test.ConnectTo(element, "less");
  element.ConnectTo(filter, "of");
  filter.ConnectTo(element, "element");
  RunLoop();
  EXPECT_EQ(5, filter.As<Filter>()->objects.size());
}
