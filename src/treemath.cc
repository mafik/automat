#include "treemath.h"

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

Variable *Tree::FindVariable(std::string_view name) {
  for (auto &node : nodes) {
    if (auto variable = dynamic_cast<Variable *>(node.get())) {
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
      Node *term = Convert(*sum->terms[i]);
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
      Node *factor = Convert(*product->factors[i]);
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
