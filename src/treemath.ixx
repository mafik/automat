export module treemath;

import <vector>;
import <unordered_set>;
import <memory>;
import algebra;

// Treemath is a library for manipulating algebraic expressions that exploits
// their tree-like structure.
//
// A tree representation is based on equality of connected nodes. Nodes of
// the tree can be "cut" to create an equation. Leaf nodes such as Constant &
// Variable can be cut to produce equations like "C = ..." & "[x] = ...".
// Non-leaf nodes can usually be cut in many ways, which produce different
// variations of the same basic equality (for example "x - y = z + v" for the
// Sum node & "x / y = z * v" for the Product node).
export namespace treemath {

struct Node {
  virtual ~Node() = default;
  virtual std::unique_ptr<algebra::Expression>
  DeriveExpression(Node *parent) = 0;
  virtual void SetEquals(Node *other) = 0;
};

struct Sum : Node {
  std::unordered_set<Node *> a;
  std::unordered_set<Node *> b;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node *parent) override;
  void SetEquals(Node *other) override;
};

struct Product : Node {
  std::unordered_set<Node *> a;
  std::unordered_set<Node *> b;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node *parent) override;
  void SetEquals(Node *other) override;
};

struct Constant : Node {
  double value = 0;
  Node *x = nullptr;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node *parent) override;
  void SetEquals(Node *other) override;
};

struct Variable : Node {
  std::string name = "";
  Node *x = nullptr;
  std::unique_ptr<algebra::Expression> DeriveExpression(Node *parent) override;
  void SetEquals(Node *other) override;
};

struct Tree {
  std::unordered_set<std::unique_ptr<Node>> nodes;

  Tree(algebra::Equation &eq);

  Variable *FindVariable(std::string_view name);

  // Convert the given algebra::Expression into a treemath::Node. The new nodes
  // are owned by this Tree and will be freed with it.
  Node *Convert(algebra::Expression &expr);
};

} // namespace treemath


// End of header

namespace treemath {

std::unique_ptr<algebra::Expression> Sum::DeriveExpression(Node *parent) {
  algebra::Sum *sum = new algebra::Sum();
  auto ptr = std::unique_ptr<algebra::Expression>(sum);
  bool a_side = a.find(parent) != a.end();
  bool b_side = b.find(parent) != b.end();
  if (a_side == b_side) {
    // `src` must be present on exactly one side of the sum.
    return nullptr;
  }
  for (auto x : a) {
    if (x == parent)
      continue;
    sum->terms.emplace_back(x->DeriveExpression(this));
    sum->minus.emplace_back(a_side);
  }
  for (auto x : b) {
    if (x == parent)
      continue;
    sum->terms.emplace_back(x->DeriveExpression(this));
    sum->minus.emplace_back(b_side);
  }
  return std::move(ptr);
}

void Sum::SetEquals(Node *other) { b.insert(other); }

std::unique_ptr<algebra::Expression> Product::DeriveExpression(Node *parent) {
  algebra::Product *product = new algebra::Product();
  auto ptr = std::unique_ptr<algebra::Expression>(product);
  bool a_side = a.find(parent) != a.end();
  bool b_side = b.find(parent) != b.end();
  if (a_side == b_side) {
    // `src` must be present on exactly one side of the product.
    return nullptr;
  }
  for (auto x : a) {
    if (x == parent)
      continue;
    product->factors.emplace_back(x->DeriveExpression(this));
    product->divide.emplace_back(a_side);
  }
  for (auto x : b) {
    if (x == parent)
      continue;
    product->factors.emplace_back(x->DeriveExpression(this));
    product->divide.emplace_back(b_side);
  }
  return std::move(ptr);
}

void Product::SetEquals(Node *other) { b.insert(other); }

std::unique_ptr<algebra::Expression> Constant::DeriveExpression(Node *parent) {
  if (parent == x) {
    return std::unique_ptr<algebra::Expression>(new algebra::Constant(value));
  }
  return x->DeriveExpression(this);
}

void Constant::SetEquals(Node *other) { x = other; }

std::unique_ptr<algebra::Expression> Variable::DeriveExpression(Node *parent) {
  if (parent == x) {
    return std::unique_ptr<algebra::Expression>(new algebra::Variable(name));
  }
  return x->DeriveExpression(this);
}

void Variable::SetEquals(Node *other) { x = other; }

Tree::Tree(algebra::Equation &eq) {
  Node *lhs = Convert(*eq.lhs);
  Node *rhs = Convert(*eq.rhs);
  lhs->SetEquals(rhs);
  rhs->SetEquals(lhs);
}

Variable* Tree::FindVariable(std::string_view name) {
  for (auto& node : nodes) {
    if (auto variable = dynamic_cast<Variable*>(node.get())) {
      if (variable->name == name) {
        return variable;
      }
    }
  }
  return nullptr;
}

Node *Tree::Convert(algebra::Expression &expr) {
  if (auto constant = dynamic_cast<algebra::Constant *>(&expr)) {
    auto node = new Constant();
    node->value = constant->value;
    nodes.emplace(node);
    return node;
  } else if (auto variable = dynamic_cast<algebra::Variable *>(&expr)) {
    auto node = new Variable();
    node->name = variable->name;
    nodes.emplace(node);
    return node;
  } else if (auto sum = dynamic_cast<algebra::Sum *>(&expr)) {
    auto node = new Sum();
    for (int i = 0; i < sum->terms.size(); ++i) {
      Node* term = Convert(*sum->terms[i]);
      term->SetEquals(node);
      if (sum->minus[i]) {
        node->b.emplace(term);
      } else {
        node->a.emplace(term);
      }
    }
    nodes.emplace(node);
    return node;
  } else if (auto product = dynamic_cast<algebra::Product *>(&expr)) {
    auto node = new Product();
    for (int i = 0; i < product->factors.size(); ++i) {
      Node* factor = Convert(*product->factors[i]);
      factor->SetEquals(node);
      if (product->divide[i]) {
        node->b.emplace(factor);
      } else {
        node->a.emplace(factor);
      }
    }
    nodes.emplace(node);
    return node;
  }
  return nullptr;
}

} // namespace treemath