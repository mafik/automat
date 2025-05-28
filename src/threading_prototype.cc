// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
// The point of this code is to see:
// 1. Whether multi-threaded object execution is possible (no scheduler)
//    and which approach has lowest synchronization overhead.

// 2. How to interface with different computation models:

// Jump - use the current thread as long as possible, executing
//        new objects when they get messages, putting extra ones
//        on side queue
// Call - block the current thread until a network of "message
//        passing" objects finishes their work
// Reactive - a value of an object changed and all of its observers
//            should be notified

// We want multi-threading (for sure)
// This means that objects may be deleted in other threads.
// This means that they must be referenced using std::{shared,weak}_ptr.
// (or maybe hazard_ptr + dedicated thread that deletes objects?)

// The references to other objects could either be stored in:
// A) Machine
// B) Location
// C) Objects themselves
//
// The last option seems to be feasible and minimizes amount of locking necessary
// for object execution so we'll go with that.

// It's best if one object can directly jump into the next one at the end of execution.
// This is similar to direct threading (although C++ lacks the facilities to implement that).
// A sequence of code executed like that is often called "main-line" code.
// Objects may return an arbitrary "next" object, which will be immediately executed.
// Extra tasks ("side-line" code) may be may be added to a global queue and will be picked
// up by worker threads.

// Experiments:
//
// 1. Measure locking overhead of different versions: - DONE
//    - Object-level shared_mutex (fixed 16-byte overhead, one big lock)
//      RESULT: fine single-threaded
//              best scaling on multiple threads
//    - atomic<weak_ptr<Object>> (variable cache line padding, many small locks)
//      RESULT: worst single-threaded
//              fine scaling on multiple threads
//    - single-threaded queue (no extra memory)
//      RESULT: incredible single-threaded throughput (no surprise here)
//              worst multi-threaded throughput
//
// RAW RESULTS:
//  struct Atomic @ 1 threads:   9093415 it / s
//  struct Atomic @16 threads:  24789444 it / s
//   struct Mutex @ 1 threads:  10459657 it / s
//   struct Mutex @16 threads:  27570291 it / s
//   struct Queue @ 1 threads: 129434289 it / s
//   struct Queue @16 threads:   7322927 it / s
//
// CONCLUSION:
//   - This test measured overhead of synchronization primitive. Real-world use will
//     involve much slower functions (see kSlowIncrement) where multi-threaded solutions
//     blow the single-threaded out of the water.
//   - Throughput seems to be best with mutexes. This could be down to less locking.
//   - Memory overhead ironically also seems to be better with mutexes. Tiny atomic values
//     must be aligned to 16 bytes which actually makes them worse, especially when multiple
//     small atomics are used.
//   - It seems like Object should just include a shared_mutex, BUT there might be some objects
//     that don't really have any state that should be synchronized (for example a function call to
//     some argument-free syscall) or some rare instances of objects where atomic is a better
//     fit. It might be good to defer the locking approach to the specific object subclass.
//   - However, given the superior throughput of mutexes, facilities should be provided to make
//     them easiest to use.
//   - It would be cool if it was possible to somehow switch to "queue-based" mode for simple
//     applications (no GUI).
//
// 2. Can we fill pointers to objects? - YES
//      weak_ptr<Object> Object::*member_ptr = (weak_ptr<Object> Object::*)&Printer::integer;
//      printf("Addr of integer pointer: %p\n", member_ptr);
//
// 3. Figure out how to interface message passing with call model
//
//  These notes are about handling "external" calls. We should also think about
//  objects calling other objects. Calling should be stack-free - so concurrent
//  calls will just have to wait until all of the other jobs are finished.
//
//    Arguments may be passed by setting values of some objects
//    Execution may start by scheduling some "start" object
//    Return value may be read from some "final" object
//
//
//    Idea 1: When running an object, attach a "token" identifier to its task
//            propagate it to subsequent tasks & syncs (main & side).
//            Block and wait until the token arrives at the destination object.
//
//            - this could be used to return as quickly as possible, when some signal
//              propagates to its destination
//
//    Idea 2: When calling an object create a counter that is incremented whenever
//            another, secondary task is queued and decremented when its completed
//            return from the call when the counter reaches 0
//
//            - this could be used to ensure that all of the related work is done
//              before the function finishes
//
//    Idea 3: The calling thread begins the processing loop directly executes the
//            main-line code, until it's finished
//
//   Optimization idea: add a bool template argument to Run - to control the locking
//                      behavior of objects. Now we can count how many threads are
//                      executing at the same time. When this number is == 1, then
//                      we can execute the non-synchronized version of Run! Downside
//                      is that to safely start a new thread we have to wait for the
//                      current task to finish.
//
//                      This idea might also work nicely with hazard pointers for
//                      objects
//
// 4. Figure out how to interface message passing with sync model

#include <functional>
#pragma maf main

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "format.hh"
#include "log.hh"

using namespace std;
using namespace automat;

// Whether to simulate slow tasks or near-instant ones.
constexpr bool kSlowIncrement = true;
constexpr bool kPrintTasks = false;
constexpr int kIterations = 100'000'000;
// constexpr bool kSlowIncrement = true;
// constexpr int kIterations = 100;

struct Object;

using WeakPtr = weak_ptr<Object>;
using SharedPtr = Ptr<Object>;

struct Task {
  WeakPtr target;
  Task(WeakPtr target_) : target(target_) {}
  virtual ~Task() {}
  virtual unique_ptr<Task> Execute(SharedPtr) { return nullptr; }
};

struct Object : enable_shared_from_this<Object> {
  atomic<weak_ptr<Object>> next;
  virtual ~Object() {}

  virtual unique_ptr<Task> Call(Ptr<Object> self, weak_ptr<Object> caller) { return nullptr; }
  virtual unique_ptr<Task> Return(Ptr<Object> self, Ptr<Object> from) {
    from->next = WeakPtr();
    from->next.notify_one();
    return nullptr;
  }

  virtual unique_ptr<Task> Run(Ptr<Object> self) { return nullptr; }
};

struct RunTask : Task {
  RunTask(WeakPtr target_) : Task(target_) {}
  unique_ptr<Task> Execute(SharedPtr self) override { return self->Run(std::move(self)); }
};

struct CallTask : Task {
  WeakPtr caller;
  CallTask(WeakPtr target_, WeakPtr caller_) : Task(target_), caller(caller_) {}
  unique_ptr<Task> Execute(SharedPtr self) override { return self->Call(std::move(self), caller); }
};

struct ReturnTask : Task {
  SharedPtr from;
  ReturnTask(WeakPtr target_, SharedPtr from_) : Task(target_), from(from_) {}
  unique_ptr<Task> Execute(SharedPtr self) override { return self->Return(std::move(self), from); }
};

void RunMainLine(unique_ptr<Task> t) {
  while (auto shared = t->target.lock()) {
    auto ptr = shared.get();
    if constexpr (kPrintTasks) {
      LOG << "<" << typeid(*t).name() << "> => " << typeid(*ptr).name() << "@" << f("%p", ptr);
    }
    t = t->Execute(std::move(shared));
    if (t == nullptr) {
      break;
    }
  }
}

struct Integer : Object {
  atomic<int> i;
  Integer(int i_) : i(i_) {}
};

struct Squarer : Object {
  shared_mutex m;
  int result;
  weak_ptr<Object> number;

  // Different states that the Squarer could be in:
  // 1. (Ready) to be called
  //    - `callback` is nullptr
  //    - <Call> causes
  //      - if the target is a Number:
  //        - squaring
  //        - transition to (Returning)
  //        - <Return>=>`callback`
  //      - otherwise
  //        - transition to (Waiting)
  //        - <Call>=>`number`
  // 2. (Waiting) for `number` to be computed
  //    - `callback` is not nullptr
  //    - <Call> blocks until `callback` is nullptr
  //    - <Return> causes:
  //      - squaring
  //      - transition to (Returning)
  //      - <Done>=>`number` (maybe this could be executed on the main line?)
  //      - <Return>=>`callback`
  // 3. (Returning) the result
  //    - `callback` is not nullptr
  //    - `result` has a proper value
  //    - <Call> blocks until `callback` is nullptr
  //    - <Done> causes
  //      - transition to (Ready)
  //      - `callback` being cleared
  //      - waiting threads notified
  //

  // Ideas to consider:
  // - is it possible to unify Call & Run? (by putting result in external object)

  // C-like wrapper, shows how to execute a call
  int GetNumber() {
    // This should execute the processing loop and execute it
    // until the squerer computes its result.
    // Then it should return.
    auto dummy_object = MakePtr<Object>();
    RunMainLine(make_unique<CallTask>(weak_from_this(), dummy_object));
    // dummy_object::Return should be called now
    auto ret = result;
    result = 0;
    return ret;
  }

  unique_ptr<Task> Return(Ptr<Object> self, Ptr<Object> from) override {
    auto squarer = dynamic_cast<Squarer*>(from.get());
    int i = squarer->result;
    {
      if constexpr (kSlowIncrement) {
        this_thread::sleep_for(1ms);
      }
      auto l = unique_lock(m);
      result = i * i;
    }
    from->next = WeakPtr();
    from->next.notify_one();
    return make_unique<ReturnTask>(next, self);
  }

  unique_ptr<Task> Call(Ptr<Object> self, weak_ptr<Object> caller) override {
    {  // Wait for `next` to be nullptr & change it to `caller`
      while (true) {
        auto next_copy = next.load();
        while (next_copy.lock().get() != nullptr) {
          next.wait(next_copy);
          next_copy = next.load();
        }
        bool success = next.compare_exchange_strong(next_copy, caller);
        if (success) {
          break;
        }
      }
    }
    // TODO: could we somehow merge this with Run?
    Ptr<Object> n_ptr;
    {
      auto l = shared_lock(m);
      n_ptr = number.lock();
    }
    if (n_ptr) {
      if (auto number_integer = dynamic_cast<Integer*>(n_ptr.get())) {
        auto i = number_integer->i.load();
        if constexpr (kSlowIncrement) {
          this_thread::sleep_for(1ms);
        }
        auto l = unique_lock(m);
        result = i * i;
      } else if (auto number_squarer = dynamic_cast<Squarer*>(n_ptr.get())) {
        return make_unique<CallTask>(number.lock(), self);
      }
    } else {
      result = 0;
    }
    return make_unique<ReturnTask>(next, self);
  }
};

struct Incrementer : Object {
  shared_mutex m;
  weak_ptr<Object> integer;

  unique_ptr<Task> Run(Ptr<Object> self) override {
    auto l1 = shared_lock(m);
    if (auto i_ptr = integer.lock()) {
      auto integer = static_cast<Integer*>(i_ptr.get());
      if constexpr (kSlowIncrement) {
        this_thread::sleep_for(1ms);
      }
      if (++(integer->i) >= 0) {
        return nullptr;
      }
    }
    return make_unique<RunTask>(next);
  }
};

double IncrementTest(int n_threads = 1) {
  auto ints = vector<Ptr<Integer>>();
  auto incs = vector<Ptr<Incrementer>>();
  for (int i = 0; i < n_threads; ++i) {
    ints.emplace_back(MakePtr<Integer>(-kIterations));
    incs.emplace_back(MakePtr<Incrementer>());
    incs.back()->integer = ints.back();
    incs.back()->next = incs.back();
  }
  auto l = latch(n_threads + 1);
  auto threads = vector<thread>();
  for (auto& inc : incs) {
    threads.emplace_back([&l, weak = weak_ptr<Object>(inc)]() mutable {
      l.arrive_and_wait();
      unique_ptr<Task> t = make_unique<RunTask>(weak);
      RunMainLine(std::move(t));
    });
  }
  this_thread::sleep_for(100ms);
  using duration = chrono::duration<double>;
  using time_point = chrono::time_point<chrono::steady_clock, duration>;
  time_point start = chrono::steady_clock::now();
  l.count_down();
  for (auto& t : threads) {
    t.join();
  }
  time_point end = chrono::steady_clock::now();
  return (double)kIterations * n_threads / (end - start).count();
}

void IncrementTestSuite() {
  for (int n_threads : {1, 16}) {
    auto single = IncrementTest(n_threads);
    printf("Increment test @%2d threads: %9.0lf it / s\n", n_threads, single);
  }
}

void CallTest() {
  // Once the correctness test is passing, we could switch to
  // API improvements.
  auto integer = MakePtr<Integer>(2);
  auto squarer = MakePtr<Squarer>();
  squarer->number = integer;
  vector<Ptr<Squarer>> squarers;
  vector<weak_ptr<Object>> leaf_squarers;
  function<void(int, weak_ptr<Object>)> Split = [&](int levels, weak_ptr<Object> number) {
    auto a = MakePtr<Squarer>();
    auto b = MakePtr<Squarer>();
    a->number = number;
    b->number = number;
    squarers.push_back(a);
    squarers.push_back(b);
    if (levels - 1 > 0) {
      Split(levels - 1, a);
      Split(levels - 1, b);
    } else {
      leaf_squarers.push_back(a);
      leaf_squarers.push_back(b);
    }
  };
  Split(4, squarer);
  latch l(leaf_squarers.size() + 1);
  vector<thread> threads;
  for (auto& leaf : leaf_squarers) {
    threads.emplace_back([&l, weak = leaf]() mutable {
      l.arrive_and_wait();
      auto squarer = dynamic_pointer_cast<Squarer>(weak.lock());
      squarer->GetNumber();
    });
  }
  this_thread::sleep_for(100ms);
  l.count_down();
  for (auto& t : threads) {
    t.join();
  }
  LOG << "Done!";
  LOG << leaf_squarers.size();
}

int main() {
  // IncrementTestSuite();
  CallTest();
}
