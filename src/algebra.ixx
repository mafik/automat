export module algebra;

import "fmt/format.h";
import <memory>;
import <cctype>;
import <functional>;
import <string>;
import <string_view>;
import <vector>;
import tree_algorithms;
import log;

using namespace automaton;

export namespace algebra {

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
  virtual double Eval(Context *context) const = 0;
};

struct Equation : Statement {
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;
  Equation(std::unique_ptr<Expression> &&lhs_,
           std::unique_ptr<Expression> &&rhs_)
      : lhs(std::move(lhs_)), rhs(std::move(rhs_)) {}
  std::unique_ptr<Statement> Clone() const override {
    auto new_lhs = std::unique_ptr<Expression>(dynamic_cast<Expression*>(lhs->Clone().release()));
    auto new_rhs = std::unique_ptr<Expression>(dynamic_cast<Expression*>(rhs->Clone().release()));
    return std::unique_ptr<Statement>(new Equation(std::move(new_lhs), std::move(new_rhs)));
  }

  std::string GetText() const override {
    return fmt::format("{} = {}", lhs->GetText(), rhs->GetText());
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
  double Eval(Context *context) const override {
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
    for (auto &term : terms) {
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
    for (auto &term : terms) {
      callback(term.get());
    }
  }
};

// Expresses a chain of multiplications & divisions.
struct Product : Expression {
  std::vector<std::unique_ptr<Expression>> factors;
  std::vector<bool> divide;
  double Eval(Context *context) const override {
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
    for (auto &factor : factors) {
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
    for (auto &factor : factors) {
      callback(factor.get());
    }
  }
};

struct Constant : Expression {
  double value;
  Constant() : value(0.0) {}
  Constant(double value) : value(value) {}
  double Eval(Context *context) const override { return value; }
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
  Variable(const std::string &name) : name(name) {}
  double Eval(Context *context) const override {
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

// End of header

std::unique_ptr<Expression> ParseExpression(std::string_view& text);

bool ParseToken(std::string token, std::string_view &text) {
  std::string_view initial_text = text;
  while (text.starts_with(" ")) {
    text.remove_prefix(1);
  }
  if (text.starts_with(token)) {
    text.remove_prefix(token.size());
    return true;
  }
  text = initial_text;
  return false;
}

std::unique_ptr<Expression> ParseConstant(std::string_view& text) {
  std::string_view initial_text = text;
  while (text.starts_with(" ")) {
    text.remove_prefix(1);
  }
  const char *start = &text[0];
  char *end;
  // Note: `strtod` will cross the end of (non-0-terminated) string_view.
  // Normally this would be dangerous but all parsed strings are
  // 0-terminated.
  double value = strtod(start, &end);
  if (end > start && end <= text.end()) {
    text.remove_prefix(end - start);
    auto c = new Constant();
    c->value = value;
    return std::unique_ptr<Expression>(c);
  }
  text = initial_text;
  return nullptr;
}

std::unique_ptr<Expression> ParseVariable(std::string_view& text) {
  std::string_view initial_text = text;
  while (text.starts_with(" ")) {
    text.remove_prefix(1);
  }
  if (text.size() > 0 && isalpha(text[0])) {
    auto var = new Variable();
    while (text.size() > 0 && isalpha(text[0])) {
      var->name += text[0];
      text.remove_prefix(1);
    }
    return std::unique_ptr<Expression>(var);
  }
  text = initial_text;
  return nullptr;
}

std::unique_ptr<Expression> ParseValue(std::string_view& text) {
  std::string_view initial_text = text;
  if (auto number = ParseConstant(text)) {
    return number;
  }
  if (auto variable = ParseVariable(text)) {
    return variable;
  }
  if (ParseToken("(", text)) {
    if (auto expr = ParseExpression(text)) {
      if (ParseToken(")", text)) {
        return expr;
      }
    }
  }
  text = initial_text;
  return nullptr;
}

std::unique_ptr<Expression> ParseProduct(std::string_view& text) {
  std::string_view initial_text = text;
  Product* product = new Product();
  bool first = true;
  while (true) {
    bool mul = ParseToken("*", text);
    bool div = ParseToken("/", text);
    if (first && (mul || div)) {
      // First term cannot be preceded by * or / signs.
      text = initial_text;
      return nullptr;
    }
    if (!first && !mul && !div) {
      // Second term requires * or / sign.
      if (product->factors.empty()) {
        text = initial_text;
        return nullptr;
      } else if (product->factors.size() == 1) {
        // Only one factor, return it.
        return std::unique_ptr<Expression>(std::move(product->factors[0]));
      } else {
        return std::unique_ptr<Expression>(product);
      }
    }
    if (auto value = ParseValue(text)) {
      product->factors.emplace_back(std::move(value));
      product->divide.emplace_back(div);
    }
    first = false;
  }
}

std::unique_ptr<Expression> ParseSum(std::string_view& text) {
  std::string_view initial_text = text;
  Sum* sum = new Sum();
  bool first = true;
  while (true) {
    bool plus = ParseToken("+", text);
    bool minus = ParseToken("-", text);
    if (!first && !plus && !minus) {
      // Second term requires + or - sign.
      if (sum->terms.empty()) {
        text = initial_text;
        return nullptr;
      } else if (sum->terms.size() == 1) {
        // Single term, no need to wrap it in Sum.
        return std::move(sum->terms[0]);
      } else {
        return std::unique_ptr<Expression>(sum);
      }
    }
    if (auto product = ParseProduct(text)) {
      sum->terms.emplace_back(std::move(product));
      sum->minus.emplace_back(minus);
    }
    first = false;
  }
}

std::unique_ptr<Expression> ParseExpression(std::string_view& text) {
  std::string_view initial_text = text;
  if (auto sum = ParseSum(text)) {
    return sum;
  }
  return nullptr;
}

std::unique_ptr<Equation> ParseEquation(std::string_view& text) {
  std::string_view initial_text = text;
  if (auto left = ParseExpression(text)) {
    if (ParseToken("=", text)) {
      if (auto right = ParseExpression(text)) {
        return std::make_unique<Equation>(std::move(left), std::move(right));
      }
    }
  }
  text = initial_text;
  return nullptr;
}

std::unique_ptr<Statement> ParseStatement(std::string_view& text) {
  std::string_view initial_text = text;
  if (auto eq = ParseEquation(text)) {
    return std::unique_ptr<Statement>(std::move(eq));
  }
  text = initial_text;
  if (auto eq = ParseExpression(text)) {
    return std::unique_ptr<Statement>(std::move(eq));
  }
  text = initial_text;
  return nullptr;
}

std::vector<algebra::Variable*> ExtractVariables(algebra::Statement* statement) {
  std::vector<algebra::Variable*> variables;
  WalkDFS<algebra::Statement>(statement, [&](algebra::Statement* statement) {
    if (algebra::Variable* variable = dynamic_cast<algebra::Variable*>(statement)) {
      variables.push_back(variable);
    }
  });
  return variables;
}

} // namespace algebra
