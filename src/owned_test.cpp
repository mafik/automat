// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <thread>

#include "board.hpp"
#include "gtest.hpp"
#include "location.hpp"
#include "object.hpp"

using namespace automat;

namespace {

struct Thing : Object {
  Ptr<Object> Clone() const override { return MAKE_PTR(Thing); }
};

struct Holder : Object {
  deque<Owned<Object>> held;
  Ptr<Object> Clone() const override { return MAKE_PTR(Holder); }
};

Vec<Object*> OwnersOf(Object& object) {
  Vec<Object*> result;
  for (auto& owner : object.Owners()) result.push_back(owner.Get());
  return result;
}

TEST(OwnedTest, OwnersAreListedInInsertionOrder) {
  auto thing = MAKE_PTR(Thing);
  auto a = MAKE_PTR(Holder);
  auto b = MAKE_PTR(Holder);
  EXPECT_TRUE(OwnersOf(*thing).empty());
  a->held.emplace_back(*a, thing);
  b->held.emplace_back(*b, thing);
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{a.Get(), b.Get()}));
  a->held.clear();
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{b.Get()}));
  b->held.clear();
  EXPECT_TRUE(OwnersOf(*thing).empty());
}

TEST(OwnedTest, ReleaseAndResetUnlink) {
  auto thing = MAKE_PTR(Thing);
  auto holder = MAKE_PTR(Holder);
  Owned<Object> link(*holder, thing);
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{holder.Get()}));
  Ptr<Object> released = link.Release();
  EXPECT_EQ(released.Get(), thing.Get());
  EXPECT_FALSE(link);
  EXPECT_TRUE(OwnersOf(*thing).empty());
  link = thing;
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{holder.Get()}));
  link.Reset();
  EXPECT_TRUE(OwnersOf(*thing).empty());
}

TEST(OwnedTest, MovedLinksStayRegistered) {
  auto thing = MAKE_PTR(Thing);
  auto holder = MAKE_PTR(Holder);
  Vec<Owned<Object>> links;
  for (int i = 0; i < 3; ++i) links.emplace_back(*holder, thing);
  EXPECT_EQ(OwnersOf(*thing).size(), 3);
  links.erase(links.begin() + 1);
  EXPECT_EQ(OwnersOf(*thing).size(), 2);
  Owned<Object> moved = std::move(links[0]);
  EXPECT_FALSE(links[0]);
  EXPECT_EQ(OwnersOf(*thing).size(), 2);
  links.clear();
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{holder.Get()}));
  moved.Reset();
  EXPECT_TRUE(OwnersOf(*thing).empty());
}

TEST(OwnedTest, DyingOwnerIsSkipped) {
  auto thing = MAKE_PTR(Thing);
  auto holder = MAKE_PTR(Holder);
  holder->held.emplace_back(*holder, thing);
  Holder* holder_raw = holder.Get();
  thing->owners_lock.lock();
  std::thread dropper([&] { holder = nullptr; });
  while (holder_raw->owning_refs.load() != 0) std::this_thread::yield();
  EXPECT_NE(thing->owners, nullptr);
  EXPECT_FALSE(holder_raw->IncrementOwningRefsNonZero());
  thing->owners_lock.unlock();
  dropper.join();
  EXPECT_TRUE(OwnersOf(*thing).empty());
}

TEST(OwnedTest, LocationAndBoardLookups) {
  auto board = MAKE_PTR(Board);
  auto thing = MAKE_PTR(Thing);
  Location& loc = board->Insert(Ptr<Object>(thing));
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{&loc}));
  EXPECT_EQ(thing->HomeLocation(), &loc);
  EXPECT_EQ(board->LocationOrNull(*thing), &loc);

  auto other = MAKE_PTR(Board);
  EXPECT_EQ(other->LocationOrNull(*thing), nullptr);
  Location& second = other->Insert(Ptr<Object>(thing));
  EXPECT_EQ(thing->HomeLocation(), &loc);
  EXPECT_EQ(other->LocationOrNull(*thing), &second);

  Ptr<Location> extracted = board->Extract(loc);
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{&loc, &second}));
  EXPECT_EQ(board->LocationOrNull(*thing), nullptr);
  EXPECT_EQ(thing->HomeLocation(), &loc);
  extracted = nullptr;
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{&second}));
  EXPECT_EQ(thing->HomeLocation(), &second);
}

TEST(OwnedTest, MakeHomeRotatesOwners) {
  auto thing = MAKE_PTR(Thing);
  auto a = MAKE_PTR(Board);
  auto b = MAKE_PTR(Board);
  auto c = MAKE_PTR(Board);
  Location& in_a = a->Insert(Ptr<Object>(thing));
  Location& in_b = b->Insert(Ptr<Object>(thing));
  Location& in_c = c->Insert(Ptr<Object>(thing));
  EXPECT_EQ(thing->HomeLocation(), &in_a);
  thing->MakeHome(in_c);
  EXPECT_EQ(thing->HomeLocation(), &in_c);
  EXPECT_EQ(OwnersOf(*thing), (Vec<Object*>{&in_c, &in_a, &in_b}));
  thing->MakeHome(*a);
  EXPECT_EQ(thing->HomeLocation(), &in_c);
}

TEST(OwnedTest, HomeLocationResolvesThroughContainers) {
  auto board = MAKE_PTR(Board);
  auto holder = MAKE_PTR(Holder);
  auto thing = MAKE_PTR(Thing);
  EXPECT_EQ(thing->HomeLocation(), nullptr);
  holder->held.emplace_back(*holder, thing);
  EXPECT_EQ(thing->HomeLocation(), nullptr);
  Location& holder_loc = board->Insert(Ptr<Object>(holder));
  EXPECT_EQ(thing->HomeLocation(), &holder_loc);
}

}  // namespace
