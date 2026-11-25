#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <ankerl/unordered_dense.h>

#include "log.hpp"
#include "vec.hpp"

namespace automat {

// Triplets are one of the very rare & prized "universal representations" of knowledge. Within a
// triplet store every entity, relation & type is represented as a triplet: (a, type, b). The `a`,
// `b` & `type` describe the "ends" of some relation & the kind of this relation. There are a few
// different formalizations but the most general one allows triplets to have an identity and to be
// repeated within triplet store (a simpler one allows another triplet to describe the number of
// copies).
//
// Because triplets can be the ends of other triplets, and also can be their types, this
// representation is extremely extensible & uniform. "Everything is a triplet" & "you can always
// describe something better by adding more triplets".
//
// Further extensions allow fuzzy knowledge to be encoded & processed in a consistent fashion.
//
// Blobs of data are typically associated with triplets and stored in some hash table on the side.
//
// Pros:
// - infinitely extensible
// - extremely uniform
// - very "surgical"
//
// Cons of this abstraction are:
// - slowness
// - high memory consumption
// - arbitrality of representation
// - hard to "constrain"
//
// The first two come from lack of a good, low-level implementation. Good low-level implementation
// (do not confuse with O(1) one - it must be competitive with arrays) might not be feasible as
// there are eight different queries that must be answered efficiently: xyz, xy?, x?z, ?yz, x??,
// ?y?, ??z, ???. Plus ability to add/remove triples.
//
// Nonetheless this structure might be useful for some cases:
// - data that is rarely changed but accessed frequently
// - serialization / deserialization
//
// Some open questions that might make it even more useful:
// - can it be "synchronized" efficiently?
//   => if so - then it could be used for replicating VM-world objects across machines
// - does it parallelize efficiently on multi-core machines?
//   => if so - then it could be used as a VM-level representation (rather than Arc-linked objects)
struct TripletStore {
  template <class K, class T>
  using map = ankerl::unordered_dense::map<K, T>;

  template <class K>
  using set = ankerl::unordered_dense::set<K>;

  int id_next = 1;
  Vec<int> id_free;
  int AllocId() {
    if (!id_free.empty()) {
      auto x = id_free.back();
      id_free.pop_back();
      return x;
    }
    return id_next++;
  }
  void FreeId(int id) {
    if (id == id_next - 1) {
      --id_next;
    } else {
      id_free.push_back(id_next);
    };
  }

  struct Triplet {
    int x, y, z;
  };
  struct Pair {
    int x, y;
  };

  Vec<Triplet> store;  // may have gaps!

  map<Triplet, set<int>> index_full;
  map<Pair, set<int>> index_ab;
  map<Pair, set<int>> index_ac;
  map<Pair, set<int>> index_bc;
  map<int, set<int>> index_a;
  map<int, set<int>> index_b;
  map<int, set<int>> index_c;
  set<int> index_any;

  int Add(int a, int b, int c) {
    int id = AllocId();
    if (store.size() <= id) {
      store.resize(id + 1);
    }
    index_full[{a, b, c}].insert(id);
    index_ab[{a, b}].insert(id);
    index_ac[{a, c}].insert(id);
    index_bc[{b, c}].insert(id);
    index_a[a].insert(id);
    index_b[b].insert(id);
    index_c[c].insert(id);
    index_any.insert(id);
    return id;
  }

  void Remove(int id) {
    auto [a, b, c] = store[id];
    index_full[{a, b, c}].erase(id);
    index_ab[{a, b}].erase(id);
    index_ac[{a, c}].erase(id);
    index_bc[{b, c}].erase(id);
    index_a[a].erase(id);
    index_b[b].erase(id);
    index_c[c].erase(id);
    index_any.erase(id);
    FreeId(id);
  }
};

inline void TripletDemo() {
  TripletStore store;

  int person = store.Add(0, 0, 0);
  int alice = store.Add(0, person, 0);
  int bob = store.Add(0, person, 0);

  int letter = store.Add(0, 0, 0);  // a letter instance
  int sends_something = store.Add(0, 0, 0);
  int sent_attachment = store.Add(0, 0, 0);

  int send_event = store.Add(alice, sends_something, bob);
  store.Add(send_event, sent_attachment, letter);

  for (int person : store.index_b[person]) {
    LOG << person << " is a person";
  }
}

}  // namespace automat