// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "format.hh"

namespace algebra {

struct Context {
  virtual double RetrieveVariable(std::string_view) = 0;
};

// A mathematical statement - formula, equation, or expression.
struct Statement {
  virtual ~Statement() = default;
  virtual std::unique_ptr<Statement> Clone() const = 0;
  virtual std::string GetText() const = 0;
  virtual void Children(std::function<void(Statement*)>) const = 0;
};

struct Expression : Statement {
  virtual double Eval(Context* context) const = 0;
};

struct Equation : Statement {
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
  Equation(std::unique_ptr<Expression>&& lhs_, std::unique_ptr<Expression>&& rhs_)
      : lhs(std::move(lhs_)), rhs(std::move(rhs_)) {}
  std::unique_ptr<Statement> Clone() const override {
    auto new_lhs = std::unique_ptr<Expression>(dynamic_cast<Expression*>(lhs->Clone().release()));
    auto new_rhs = std::unique_ptr<Expression>(dynamic_cast<Expression*>(rhs->Clone().release()));
    return std::unique_ptr<Statement>(new Equation(std::move(new_lhs), std::move(new_rhs)));
  }

  std::string GetText() const override {
    return automat::f("%s = %s", lhs->GetText().c_str(), rhs->GetText().c_str());
  }

  void Children(std::function<void(Statement*)> callback) const override {
    callback(lhs.get());
    callback(rhs.get());
  }
};

// Expresses a chain of additions & subtractions.
struct Sum : Expression {
  std::vector<std::unique_ptr<Expression>> terms;
  std::vector<bool> minus;
  double Eval(Context* context) const override {
    double result = 0;
    for (int i = 0; i < terms.size(); ++i) {
      double val = terms[i]->Eval(context);
      result += minus[i] ? -val : val;
    }
    return result;
  }
  std::unique_ptr<Statement> Clone() const override {
    auto clone = new Sum();
    clone->minus = minus;
    for (auto& term : terms) {
      clone->terms.emplace_back(dynamic_cast<Expression*>(term->Clone().release()));
    }
    return std::unique_ptr<Statement>(clone);
  }
  std::string GetText() const override {
    std::string result = "(";
    for (int i = 0; i < terms.size(); ++i) {
      if (i) {
        result += minus[i] ? " - " : " + ";
      } else {
        result += minus[i] ? "- " : "";
      }
      result += terms[i]->GetText();
    }
    return result + ")";
  }
  void Children(std::function<void(Statement*)> callback) const override {
    for (auto& term : terms) {
      callback(term.get());
    }
  }
};

// Expresses a chain of multiplications & divisions.
struct Product : Expression {
  std::vector<std::unique_ptr<Expression>> factors;
  std::vector<bool> divide;
  double Eval(Context* context) const override {
    double result = 1.0;
    for (int i = 0; i < factors.size(); ++i) {
      double val = factors[i]->Eval(context);
      if (divide[i]) {
        result /= val;
      } else {
        result *= val;
      }
    }
    return result;
  }
  std::unique_ptr<Statement> Clone() const override {
    auto clone = new Product();
    clone->divide = divide;
    for (auto& factor : factors) {
      clone->factors.emplace_back(dynamic_cast<Expression*>(factor->Clone().release()));
    }
    return std::unique_ptr<Statement>(clone);
  }
  std::string GetText() const override {
    std::string result = "(";
    for (int i = 0; i < factors.size(); ++i) {
      if (i) {
        result += divide[i] ? " / " : " * ";
      } else {
        result += divide[i] ? "1 / " : "";
      }
      result += factors[i]->GetText();
    }
    return result + ")";
  }
  void Children(std::function<void(Statement*)> callback) const override {
    for (auto& factor : factors) {
      callback(factor.get());
    }
  }
};

struct Constant : Expression {
  double value;
  Constant() : value(0.0) {}
  Constant(double value) : value(value) {}
  double Eval(Context* context) const override { return value; }
  std::unique_ptr<Statement> Clone() const override {
    auto clone = new Constant(*this);
    return std::unique_ptr<Statement>(clone);
  }
  std::string GetText() const override { return std::to_string(value); }
  void Children(std::function<void(Statement*)> callback) const override {}
};

struct Variable : Expression {
  std::string name;
  Variable() : name("") {}
  Variable(const std::string& name) : name(name) {}
  double Eval(Context* context) const override {
    auto ret = context->RetrieveVariable(name);
    return ret;
  }
  std::unique_ptr<Statement> Clone() const override {
    auto clone = new Variable(*this);
    return std::unique_ptr<Statement>(clone);
  }
  std::string GetText() const override { return name; }
  void Children(std::function<void(Statement*)> callback) const override {}
};

std::unique_ptr<Statement> ParseStatement(std::string_view& text);
std::vector<Variable*> ExtractVariables(Statement* statement);

}  // namespace algebra
