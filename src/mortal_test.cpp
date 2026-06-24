// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "mortal.hpp"

#include <memory>
#include <set>

#include "gtest.hpp"

using namespace automat;

namespace {

struct M {
  MortalCoil mortal_coil;
  int id = 0;
};

mortal_priv::Ref* HeadNode(M& m) { return mortal_priv::Untag(m.mortal_coil.head); }
int ListLength(M& m) {
  int n = 0;
  for (mortal_priv::Ref* r = mortal_priv::Untag(m.mortal_coil.head); r;
       r = mortal_priv::Untag(r->next))
    ++n;
  return n;
}

}  // namespace

TEST(MortalPtr, NullsOnDeath) {
  auto m = std::make_unique<M>();
  MortalPtr<M> p(*m);
  EXPECT_EQ(p.Get(), m.get());
  EXPECT_TRUE(static_cast<bool>(p));
  m.reset();
  EXPECT_EQ(p.Get(), nullptr);
  EXPECT_FALSE(static_cast<bool>(p));
}

TEST(MortalPtr, ManyAllNull) {
  auto m = std::make_unique<M>();
  MortalPtr<M> a(*m), b(*m), c(*m);
  EXPECT_EQ(ListLength(*m), 3);
  m.reset();
  EXPECT_FALSE(static_cast<bool>(a));
  EXPECT_FALSE(static_cast<bool>(b));
  EXPECT_FALSE(static_cast<bool>(c));
}

TEST(MortalPtr, UnlinkOnDestroy) {
  M m;
  {
    MortalPtr<M> p(m);
    EXPECT_EQ(ListLength(m), 1);
  }
  EXPECT_EQ(m.mortal_coil.head, nullptr);
}

TEST(MortalPtr, UnlinkMiddle) {
  M m;
  MortalPtr<M> a(m);
  auto b = std::make_unique<MortalPtr<M>>(m);
  MortalPtr<M> c(m);
  EXPECT_EQ(ListLength(m), 3);
  b.reset();  // remove a middle node
  EXPECT_EQ(ListLength(m), 2);
  EXPECT_TRUE(static_cast<bool>(a));
  EXPECT_TRUE(static_cast<bool>(c));
}

TEST(MortalPtr, CopyIsIndependent) {
  auto m = std::make_unique<M>();
  MortalPtr<M> a(*m);
  MortalPtr<M> b = a;
  EXPECT_EQ(b.Get(), m.get());
  EXPECT_EQ(ListLength(*m), 2);
  m.reset();
  EXPECT_FALSE(static_cast<bool>(a));
  EXPECT_FALSE(static_cast<bool>(b));
}

TEST(MortalPtr, MoveTransfers) {
  M m;
  MortalPtr<M> a(m);
  MortalPtr<M> b = std::move(a);
  EXPECT_EQ(b.Get(), &m);
  EXPECT_FALSE(static_cast<bool>(a));
  EXPECT_EQ(ListLength(m), 1);
  EXPECT_EQ(HeadNode(m), static_cast<mortal_priv::Ref*>(&b));
}

TEST(MortalPtr, Reassign) {
  M m1, m2;
  MortalPtr<M> p(m1);
  EXPECT_EQ(ListLength(m1), 1);
  p = &m2;
  EXPECT_EQ(ListLength(m1), 0);
  EXPECT_EQ(ListLength(m2), 1);
  EXPECT_EQ(p.Get(), &m2);
  p = nullptr;
  EXPECT_FALSE(static_cast<bool>(p));
  EXPECT_EQ(ListLength(m2), 0);
}

TEST(MortalPtr16, NullsOnDeath) {
  auto m = std::make_unique<M>();
  MortalPtr16<M> p(*m);
  EXPECT_EQ(p.Get(), m.get());
  m.reset();
  EXPECT_EQ(p.Get(), nullptr);
}

TEST(MortalPtr16, UnlinkMiddle) {
  M m;
  MortalPtr16<M> a(m);
  auto b = std::make_unique<MortalPtr16<M>>(m);
  MortalPtr16<M> c(m);
  EXPECT_EQ(ListLength(m), 3);
  b.reset();
  EXPECT_EQ(ListLength(m), 2);
  EXPECT_TRUE(static_cast<bool>(a));
  EXPECT_TRUE(static_cast<bool>(c));
}

// A slow node removed from in front of a fast node must repair the fast node's back-link.
TEST(MortalPtr, SlowRemovalRepairsFastSuccessor) {
  M m;
  MortalPtr<M> fast(m);    // inserted first -> ends up at the tail
  MortalPtr16<M> slow(m);  // inserted second -> ends up at the head, before `fast`
  EXPECT_EQ(ListLength(m), 2);
  slow = nullptr;  // O(n) removal in front of `fast`
  EXPECT_EQ(ListLength(m), 1);
  fast = nullptr;  // O(1) removal; back-link must be valid
  EXPECT_EQ(m.mortal_coil.head, nullptr);
}

TEST(MortalFn, FiresOnDeath) {
  auto m = std::make_unique<M>();
  int fired = 0;
  MortalFn<M> f(*m, [&] { ++fired; });
  EXPECT_EQ(fired, 0);
  m.reset();
  EXPECT_EQ(fired, 1);
}

TEST(MortalFn, CancelBeforeDeath) {
  auto m = std::make_unique<M>();
  int fired = 0;
  {
    MortalFn<M> f(*m, [&] { ++fired; });
  }
  EXPECT_EQ(ListLength(*m), 0);
  m.reset();
  EXPECT_EQ(fired, 0);
}

TEST(MortalFn, FiresWithPtrsInList) {
  auto m = std::make_unique<M>();
  int fired = 0;
  MortalPtr<M> p(*m);
  MortalFn<M> f(*m, [&] { ++fired; });
  MortalPtr16<M> q(*m);
  m.reset();
  EXPECT_EQ(fired, 1);
  EXPECT_FALSE(static_cast<bool>(p));
  EXPECT_FALSE(static_cast<bool>(q));
}

// A death callback that destroys another live reference into the same dying Mortal must not
// corrupt the walk.
TEST(MortalFn, ReentrantUnlinkDuringDeath) {
  auto m = std::make_unique<M>();
  auto victim = std::make_unique<MortalPtr<M>>(*m);
  int fired = 0;
  MortalFn<M> f(*m, [&] {
    ++fired;
    victim.reset();  // unlinks a sibling node mid-walk
  });
  MortalPtr<M> tail(*m);
  m.reset();
  EXPECT_EQ(fired, 1);
  EXPECT_FALSE(static_cast<bool>(tail));
}

// Exercises the shared MortalCallback::Steal: `a` is the head node, moved into `b`; the coil head
// must re-point at `b`, which then fires exactly once.
TEST(MortalFn, MoveTransfers) {
  auto m = std::make_unique<M>();
  int fired = 0;
  MortalFn<M> a(*m, [&] { ++fired; });
  MortalFn<M> b = std::move(a);
  EXPECT_EQ(ListLength(*m), 1);
  m.reset();
  EXPECT_EQ(fired, 1);
}

TEST(MortalFn40, MoveTransfers) {
  auto m = std::make_unique<M>();
  int fired = 0;
  MortalFn40<M> a(*m, [&] { ++fired; });
  MortalFn40<M> b = std::move(a);
  EXPECT_EQ(ListLength(*m), 1);
  m.reset();
  EXPECT_EQ(fired, 1);
}

TEST(MortalFnRef, FiresOnDeath) {
  auto m = std::make_unique<M>();
  int fired = 0;
  auto cb = [&] { ++fired; };  // outlives the MortalFnRef and the Mortal
  MortalFnRef<M> r(*m, cb);
  m.reset();
  EXPECT_EQ(fired, 1);
}

TEST(MortalFnRef, CancelBeforeDeath) {
  auto m = std::make_unique<M>();
  int fired = 0;
  auto cb = [&] { ++fired; };
  {
    MortalFnRef<M> r(*m, cb);
  }
  m.reset();
  EXPECT_EQ(fired, 0);
}

TEST(MortalFn40, FiresOnDeath) {
  auto m = std::make_unique<M>();
  int fired = 0;
  MortalFn40<M> f(*m, [&] { ++fired; });
  m.reset();
  EXPECT_EQ(fired, 1);
}

TEST(MortalFn40, CapturesByValue) {
  auto m = std::make_unique<M>();
  int result = 0;
  int captured = 42;
  MortalFn40<M> f(*m, [&result, captured] { result = captured; });
  m.reset();
  EXPECT_EQ(result, 42);
}

TEST(MortalList, AddAndIterate) {
  M a, b, c;
  MortalList<M> list;
  list.Add(a);
  list.Add(b);
  list.Add(c);
  EXPECT_EQ(list.size(), 3u);
  std::set<M*> seen;
  for (M& x : list) seen.insert(&x);
  EXPECT_EQ(seen, (std::set<M*>{&a, &b, &c}));
}

TEST(MortalList, DropsDeadTargets) {
  auto a = std::make_unique<M>();
  M b;
  MortalList<M> list;
  list.Add(*a);
  list.Add(b);
  EXPECT_EQ(list.size(), 2u);
  a.reset();  // target dies -> its entry is erased from the list
  EXPECT_EQ(list.size(), 1u);
  for (M& x : list) EXPECT_EQ(&x, &b);
}

TEST(MortalList, HolderDeathDetaches) {
  M a;
  {
    MortalList<M> list;
    list.Add(a);
    EXPECT_EQ(ListLength(a), 1);
  }
  // List destroyed first: 'a' must no longer reference the freed entry.
  EXPECT_EQ(a.mortal_coil.head, nullptr);
}

TEST(Mortal, AllKindsInOneCoil) {
  auto m = std::make_unique<M>();
  int fn = 0, fnref = 0, fn40 = 0;
  auto ref_cb = [&] { ++fnref; };
  MortalPtr<M> p(*m);
  MortalPtr16<M> q(*m);
  MortalFn<M> a(*m, [&] { ++fn; });
  MortalFnRef<M> b(*m, ref_cb);
  MortalFn40<M> c(*m, [&] { ++fn40; });
  MortalList<M> list;
  list.Add(*m);
  EXPECT_EQ(ListLength(*m), 6);
  m.reset();
  EXPECT_FALSE(static_cast<bool>(p));
  EXPECT_FALSE(static_cast<bool>(q));
  EXPECT_EQ(fn, 1);
  EXPECT_EQ(fnref, 1);
  EXPECT_EQ(fn40, 1);
  EXPECT_EQ(list.size(), 0u);
}

// Moving a Mortal relocates its pointer referers to the new address.
TEST(MortalCoil, MoveConstructRelocatesPointers) {
  auto m1 = std::make_unique<M>();
  m1->id = 7;
  MortalPtr<M> p(*m1);
  MortalPtr16<M> q(*m1);
  M m2 = std::move(*m1);
  m1.reset();  // destroy the moved-from object; referers must already point at m2
  EXPECT_EQ(p.Get(), &m2);
  EXPECT_EQ(q.Get(), &m2);
  EXPECT_EQ(p->id, 7);
}

// A MortalList entry follows its target across a move.
TEST(MortalCoil, MoveConstructRelocatesListEntry) {
  MortalList<M> list;
  auto m1 = std::make_unique<M>();
  list.Add(*m1);
  M m2 = std::move(*m1);
  m1.reset();
  ASSERT_EQ(list.size(), 1u);
  for (M& x : list) EXPECT_EQ(&x, &m2);
}

// A death callback fires for the relocated object, not the moved-from husk.
TEST(MortalCoil, MoveRelocatesCallback) {
  int fired = 0;
  auto m2 = std::make_unique<M>();
  auto cb = std::make_unique<MortalFn<M>>();
  {
    auto m1 = std::make_unique<M>();
    *cb = MortalFn<M>(*m1, [&] { ++fired; });
    *m2 = std::move(*m1);
    m1.reset();
    EXPECT_EQ(fired, 0);
  }
  EXPECT_EQ(fired, 0);
  m2.reset();
  EXPECT_EQ(fired, 1);
}

// Move-assignment destroys the overwritten destination's old value first (its callbacks fire, its
// pointers null), then relocates the source's referers into it quietly.
TEST(MortalCoil, MoveAssignDestroysDestinationThenRelocatesSource) {
  auto dst = std::make_unique<M>();
  auto src = std::make_unique<M>();
  int dst_fired = 0, src_fired = 0;
  MortalFn<M> dst_cb(*dst, [&] { ++dst_fired; });
  MortalPtr<M> dst_p(*dst);
  MortalFn<M> src_cb(*src, [&] { ++src_fired; });
  MortalPtr<M> src_p(*src);

  *dst = std::move(*src);

  // The overwritten destination's old value died: its callback fired and its pointer nulled.
  EXPECT_EQ(dst_fired, 1);
  EXPECT_FALSE(static_cast<bool>(dst_p));
  // The source merely relocated: its callback did NOT fire and its pointer retargeted to dst.
  EXPECT_EQ(src_fired, 0);
  EXPECT_EQ(src_p.Get(), dst.get());
  EXPECT_EQ(ListLength(*dst), 2);  // only the source's two referers remain

  src.reset();  // the moved-from husk has an empty coil
  EXPECT_EQ(src_fired, 0);

  dst.reset();  // the relocated callback fires when its Mortal really dies
  EXPECT_EQ(src_fired, 1);
  EXPECT_FALSE(static_cast<bool>(src_p));
}
