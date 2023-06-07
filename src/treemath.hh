#pragma once

#include <memory>
#include <unordered_set>
#include <vector>

#include "algebra.h"

// Treemath is a library for manipulating algebraic expressions that exploits
// their tree-like structure.
//
// A tree representation is based on equality of connected nodes. Nodes of
// the tree can be "cut" to create an equation. Leaf nodes such as Constant &
// Variable can be cut to produce equations like "C = ..." & "[x] = ...".
// Non-leaf nodes can usually be cut in many ways, which produce different
// variations of the same basic equality (for example "x - y = z + v" for the
// Sum node & "x / y = z * v" for the Product node).
namespace treemath {

struct Node {
  virtual ~Node() = default;
  virtual std::unique_ptr<algebra::Expression> DeriveExpression(Node* parent) = 0;
  virtual void SetEquals(Node* other) = 0;
};

struct Sum : Node {
  std::unordered_set<Node*> a;
  std::unordered_set<Node*> b;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node* parent) override;
  void SetEquals(Node* other) override;
};

struct Product : Node {
  std::unordered_set<Node*> a;
  std::unordered_set<Node*> b;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node* parent) override;
  void SetEquals(Node* other) override;
};

struct Constant : Node {
  double value = 0;
  Node* x = nullptr;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node* parent) override;
  void SetEquals(Node* other) override;
};

struct Variable : Node {
  std::string name = "";
  Node* x = nullptr;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node* parent) override;
  void SetEquals(Node* other) override;
};

struct Tree {
  std::unordered_set<std::unique_ptr<Node>> nodes;

  Tree(algebra::Equation& eq);

  Variable* FindVariable(std::string_view name);

  // Convert the given algebra::Expression into a treemath::Node. The new nodes
  // are owned by this Tree and will be freed with it.
  Node* Convert(algebra::Expression& expr);
};

}  // namespace treemath
