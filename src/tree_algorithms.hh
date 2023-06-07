#pragma once

#include <functional>
#include <vector>

namespace automat {

template <class C>
void WalkDFS(C* root, std::function<void(C*)> callback) {
  std::vector<C*> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    C* current = stack.back();
    stack.pop_back();
    callback(current);
    current->Children([&](C* child) { stack.push_back(child); });
  }
}

}  // namespace automat
