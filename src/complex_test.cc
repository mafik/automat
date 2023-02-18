import <gtest/gtest.h>;
import test_base;
import base;
import library;

using namespace automaton;

struct ComplexTest : TestBase {
  Handle &label = machine.Create<Text>();
  Handle &complex = machine.Create<Complex>();
  Handle &field = machine.Create<ComplexField>();

  ComplexTest() {
    label.SetText("X");
    field.ConnectTo(label, "label");
    field.ConnectTo(complex, "complex");
    field.Put(Create<Text>());
  }
};

TEST_F(ComplexTest, FollowField) {
  ASSERT_EQ(complex.As<Complex>()->objects.size(), 1);
  EXPECT_EQ(complex.As<Complex>()->objects.begin()->second.get(), field.Follow());
}

TEST_F(ComplexTest, TakeField) {
  ASSERT_EQ(complex.As<Complex>()->objects.size(), 1);

  auto taken = field.Take();
  EXPECT_NE(taken.get(), nullptr);
  EXPECT_EQ(complex.As<Complex>()->objects.size(), 0);
}

TEST_F(ComplexTest, CloneWithField) {
  field.Follow()->SetText(field, "Hello, world!");

  Handle &clone = machine.Create(*complex.object, "Clone");
  EXPECT_NE(&complex, &clone);
  auto &[clone_label, clone_member] = *clone.As<Complex>()->objects.begin();
  EXPECT_NE(field.Follow(), clone_member.get());
  EXPECT_EQ(clone_label, "X");
  EXPECT_EQ(clone_member->GetText(), "Hello, world!");
}
