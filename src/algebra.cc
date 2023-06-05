#include "algebra.h"

namespace algebra {

bool ParseToken(std::string token, std::string_view& text) {
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
  const char* start = &text[0];
  char* end;
  // Note: `strtod` will cross the end of (non-0-terminated) string_view.
  // Normally this would be dangerous but all parsed strings are
  // 0-terminated.
  double value = strtod(start, &end);
  int l = end - start;
  if (l > 0 && l <= text.size()) {
    text.remove_prefix(l);
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

std::unique_ptr<Expression> ParseExpression(std::string_view& text);

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
}  // namespace algebra