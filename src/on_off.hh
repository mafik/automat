#pragma once

namespace automat {

struct OnOff {
  virtual ~OnOff() = default;

  virtual bool IsOn() const = 0;
  virtual void On() = 0;
  virtual void Off() = 0;

  void Toggle() {
    if (IsOn())
      Off();
    else
      On();
  }
};

}  // namespace automat