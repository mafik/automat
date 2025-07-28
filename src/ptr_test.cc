// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "ptr.hh"

#include "gtest.hh"

using namespace automat;

enum Event {
  Allocate,
  Deallocate,
  Construct,
  Destruct,
};

std::vector<Event> event_log;

struct PtrTest : testing::Test {
  struct Entity : ReferenceCounted {
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
  auto ptr = MAKE_PTR(Entity);
  auto raw = ptr.Get();
  EXPECT_LOG(Allocate, Construct);
  EXPECT_REF_COUNT(raw, 1, 1);
  {
    auto weak = WeakPtr<Entity>(ptr);
    EXPECT_REF_COUNT(raw, 1, 2);
    EXPECT_LOG(Allocate, Construct);
    ptr.Reset();
    EXPECT_REF_COUNT(raw, 0, 1);
    EXPECT_LOG(Allocate, Construct, Destruct);
  }
  EXPECT_LOG(Allocate, Construct, Destruct, Deallocate);
}

TEST_F(PtrTest, CopyConstructor) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto raw = ptr1.Get();
  EXPECT_LOG(Allocate, Construct);
  EXPECT_REF_COUNT(raw, 1, 1);

  // Copy constructor
  auto ptr2 = ptr1;
  EXPECT_REF_COUNT(raw, 2, 1);
  EXPECT_LOG(Allocate, Construct);

  // Both pointers should point to the same object
  EXPECT_EQ(ptr1.Get(), ptr2.Get());
}

TEST_F(PtrTest, MoveConstructor) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto raw = ptr1.Get();
  EXPECT_LOG(Allocate, Construct);
  EXPECT_REF_COUNT(raw, 1, 1);

  // Move constructor
  auto ptr2 = std::move(ptr1);
  EXPECT_REF_COUNT(raw, 1, 1);
  EXPECT_LOG(Allocate, Construct);

  // Original pointer should be null, new one should have the object
  EXPECT_EQ(ptr1.Get(), nullptr);
  EXPECT_EQ(ptr2.Get(), raw);
}

TEST_F(PtrTest, CopyAssignment) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto ptr2 = MAKE_PTR(Entity);
  auto raw1 = ptr1.Get();
  auto raw2 = ptr2.Get();
  EXPECT_LOG(Allocate, Construct, Allocate, Construct);

  // Copy assignment
  ptr2 = ptr1;
  EXPECT_REF_COUNT(raw1, 2, 1);
  EXPECT_LOG(Allocate, Construct, Allocate, Construct, Destruct, Deallocate);

  // Both pointers should point to the first object
  EXPECT_EQ(ptr1.Get(), ptr2.Get());
  EXPECT_EQ(ptr1.Get(), raw1);
}

TEST_F(PtrTest, MoveAssignment) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto ptr2 = MAKE_PTR(Entity);
  auto raw1 = ptr1.Get();
  auto raw2 = ptr2.Get();
  EXPECT_LOG(Allocate, Construct, Allocate, Construct);

  // Move assignment
  ptr2 = std::move(ptr1);
  EXPECT_LOG(Allocate, Construct, Allocate, Construct, Destruct, Deallocate);

  // ptr1 should be null, ptr2 should point to first object
  EXPECT_EQ(ptr1.Get(), nullptr);
  EXPECT_EQ(ptr2.Get(), raw1);
}

TEST_F(PtrTest, NullptrAssignment) {
  EXPECT_LOG();
  auto ptr = MAKE_PTR(Entity);
  auto raw = ptr.Get();
  EXPECT_LOG(Allocate, Construct);

  // Assign nullptr
  ptr = nullptr;
  EXPECT_LOG(Allocate, Construct, Destruct, Deallocate);

  // Pointer should be null
  EXPECT_EQ(ptr.Get(), nullptr);
}

TEST_F(PtrTest, SwapFunction) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto ptr2 = MAKE_PTR(Entity);
  auto raw1 = ptr1.Get();
  auto raw2 = ptr2.Get();
  EXPECT_LOG(Allocate, Construct, Allocate, Construct);

  // Swap pointers
  swap(ptr1, ptr2);

  // Pointers should be swapped
  EXPECT_EQ(ptr1.Get(), raw2);
  EXPECT_EQ(ptr2.Get(), raw1);
  EXPECT_LOG(Allocate, Construct, Allocate, Construct);
}

TEST_F(PtrTest, SwapMethod) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto ptr2 = MAKE_PTR(Entity);
  auto raw1 = ptr1.Get();
  auto raw2 = ptr2.Get();
  EXPECT_LOG(Allocate, Construct, Allocate, Construct);

  // Swap pointers using method
  ptr1.Swap(ptr2);

  // Pointers should be swapped
  EXPECT_EQ(ptr1.Get(), raw2);
  EXPECT_EQ(ptr2.Get(), raw1);
  EXPECT_LOG(Allocate, Construct, Allocate, Construct);
}

TEST_F(PtrTest, ComparisonOperators) {
  auto ptr1 = MAKE_PTR(Entity);
  auto ptr2 = MAKE_PTR(Entity);
  auto ptr3 = ptr1;

  // Equality operators
  EXPECT_TRUE(ptr1 == ptr3);
  EXPECT_FALSE(ptr1 == ptr2);
  EXPECT_TRUE(ptr1 != ptr2);
  EXPECT_FALSE(ptr1 != ptr3);

  // nullptr comparison
  EXPECT_FALSE(ptr1 == nullptr);
  EXPECT_TRUE(ptr1 != nullptr);
  EXPECT_FALSE(nullptr == ptr1);
  EXPECT_TRUE(nullptr != ptr1);

  // After reset
  ptr1.Reset();
  EXPECT_TRUE(ptr1 == nullptr);
  EXPECT_FALSE(ptr1 != nullptr);
  EXPECT_TRUE(nullptr == ptr1);
  EXPECT_FALSE(nullptr != ptr1);
}

TEST_F(PtrTest, ReleaseMethod) {
  EXPECT_LOG();
  auto ptr = MAKE_PTR(Entity);
  auto raw = ptr.Get();
  EXPECT_LOG(Allocate, Construct);

  // Release pointer
  auto released = ptr.Release();

  // Pointer should be null, but object should not be destroyed
  EXPECT_EQ(ptr.Get(), nullptr);
  EXPECT_EQ(released, raw);
  EXPECT_LOG(Allocate, Construct);

  // Manually handle the reference
  released->DecrementOwningRefs();
  // DecrementOwningRefs triggers both Destruct and Deallocate
  EXPECT_LOG(Allocate, Construct, Destruct, Deallocate);
}

TEST_F(PtrTest, WeakPtrExpiredAndLock) {
  EXPECT_LOG();
  WeakPtr<Entity> weak(nullptr);

  {
    auto ptr = MAKE_PTR(Entity);
    auto raw = ptr.Get();
    EXPECT_LOG(Allocate, Construct);

    // Create weak pointer
    weak = WeakPtr<Entity>(ptr);
    EXPECT_REF_COUNT(raw, 1, 2);

    // Lock should return valid pointer
    auto locked = weak.Lock();
    EXPECT_FALSE(weak.IsExpired());
    EXPECT_EQ(locked.Get(), raw);
    EXPECT_REF_COUNT(raw, 2, 2);
  }

  // After ptr goes out of scope, weak should be expired
  EXPECT_TRUE(weak.IsExpired());

  // Lock should return null pointer
  auto locked = weak.Lock();
  EXPECT_EQ(locked.Get(), nullptr);

  // Check final event log
  EXPECT_LOG(Allocate, Construct, Destruct);
}

TEST_F(PtrTest, WeakPtrReset) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto ptr2 = MAKE_PTR(Entity);
  auto raw1 = ptr1.Get();
  auto raw2 = ptr2.Get();

  // Create weak pointer
  auto weak = WeakPtr<Entity>(ptr1);
  EXPECT_REF_COUNT(raw1, 1, 2);

  // Reset to another pointer
  weak.Reset(raw2);
  EXPECT_REF_COUNT(raw1, 1, 1);
  EXPECT_REF_COUNT(raw2, 1, 2);

  // Lock should return second object
  auto locked = weak.Lock();
  EXPECT_EQ(locked.Get(), raw2);
  EXPECT_REF_COUNT(raw2, 2, 2);

  // Release the locked pointer to restore ref counts
  locked.Reset();
  EXPECT_REF_COUNT(raw2, 1, 2);

  // Reset to nullptr
  weak.Reset();
  EXPECT_REF_COUNT(raw2, 1, 1);

  // Lock should return null
  locked = weak.Lock();
  EXPECT_EQ(locked.Get(), nullptr);
}

TEST_F(PtrTest, WeakPtrAssignment) {
  EXPECT_LOG();
  auto ptr1 = MAKE_PTR(Entity);
  auto ptr2 = MAKE_PTR(Entity);
  auto raw1 = ptr1.Get();
  auto raw2 = ptr2.Get();

  // Create weak pointers
  auto weak1 = WeakPtr<Entity>(ptr1);
  auto weak2 = WeakPtr<Entity>(ptr2);
  EXPECT_REF_COUNT(raw1, 1, 2);
  EXPECT_REF_COUNT(raw2, 1, 2);

  // Copy assignment
  weak2 = weak1;
  EXPECT_REF_COUNT(raw1, 1, 3);
  EXPECT_REF_COUNT(raw2, 1, 1);

  // Both should lock to first object
  auto locked1 = weak1.Lock();
  auto locked2 = weak2.Lock();
  EXPECT_EQ(locked1.Get(), raw1);
  EXPECT_EQ(locked2.Get(), raw1);
  EXPECT_REF_COUNT(raw1, 3, 3);

  // Reset locked pointers to restore ref counts
  locked1.Reset();
  locked2.Reset();
  EXPECT_REF_COUNT(raw1, 1, 3);

  // Move assignment
  auto weak3 = WeakPtr<Entity>(ptr2);
  EXPECT_REF_COUNT(raw2, 1, 2);

  weak2 = std::move(weak3);
  EXPECT_REF_COUNT(raw1, 1, 2);
  EXPECT_REF_COUNT(raw2, 1, 2);

  // weak2 should now lock to second object
  locked2 = weak2.Lock();
  EXPECT_EQ(locked2.Get(), raw2);
  EXPECT_REF_COUNT(raw2, 2, 2);

  // weak3 should be empty (moved from)
  auto locked3 = weak3.Lock();
  EXPECT_EQ(locked3.Get(), nullptr);
}

TEST_F(PtrTest, BoolConversionOperator) {
  Ptr<Entity> null_ptr;
  auto valid_ptr = MAKE_PTR(Entity);

  // Bool conversion operator
  EXPECT_FALSE(static_cast<bool>(null_ptr));
  EXPECT_TRUE(static_cast<bool>(valid_ptr));

  // Implicit conversion in if statement
  if (null_ptr) {
    FAIL() << "null_ptr evaluated as true";
  }

  if (!valid_ptr) {
    FAIL() << "valid_ptr evaluated as false";
  }
}

TEST_F(PtrTest, DereferenceOperators) {
  auto ptr = MAKE_PTR(Entity);
  auto raw = ptr.Get();

  // Arrow operator
  EXPECT_EQ(ptr->owning_refs.load(), 1);

  // Dereference operator
  EXPECT_EQ(&(*ptr), raw);
}
