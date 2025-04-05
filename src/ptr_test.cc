// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "ptr.hh"

#include "gtest.hh"

enum Event {
  Allocate,
  Deallocate,
  Construct,
  Destruct,
};

std::vector<Event> event_log;

struct PtrTest : testing::Test {
  struct Entity : ReferenceCounted<Entity> {
    Entity() { event_log.push_back(Construct); }
    ~Entity() { event_log.push_back(Destruct); }

    void* operator new(size_t size) {
      event_log.push_back(Allocate);
      return ::operator new(size);
    }
    void operator delete(void* ptr) {
      event_log.push_back(Deallocate);
      ::operator delete(ptr);
    }
  };

  void TearDown() override { event_log.clear(); }
};

#define EXPECT_LOG(expected_vec...) \
  EXPECT_THAT(event_log, testing::ElementsAreArray(std::initializer_list<Event>{expected_vec}));

#define EXPECT_REF_COUNT(ptr, expected_owning, expected_weak) \
  EXPECT_EQ(ptr->owning_refs, expected_owning);               \
  EXPECT_EQ(ptr->weak_refs, expected_weak);

TEST_F(PtrTest, KeyEvents) {
  EXPECT_LOG();
  auto ptr = MakePtr<Entity>();
  auto raw = ptr.get();
  EXPECT_LOG(Allocate, Construct);
  EXPECT_REF_COUNT(raw, 1, 1);
  {
    auto weak = WeakPtr<Entity>(ptr);
    EXPECT_REF_COUNT(raw, 1, 2);
    EXPECT_LOG(Allocate, Construct);
    ptr.reset();
    EXPECT_REF_COUNT(raw, 0, 1);
    EXPECT_LOG(Allocate, Construct, Destruct);
  }
  EXPECT_LOG(Allocate, Construct, Destruct, Deallocate);
}
