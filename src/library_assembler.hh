// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>

#include "library_instruction.hh"
#include "object.hh"
#include "status.hh"

namespace automat::library {

constexpr size_t kMachineCodeSize = 1024 * 4;

struct DeleteWithMunmap {
  void operator()(void* ptr) const;
};

struct Regs;

// Combines functions of Assembler and Thread.
// Assembler part takes care of emitting machine code to an executable memory region.
// Thread part maintains register state across executions.
struct Assembler : LiveObject {
  std::unique_ptr<char, DeleteWithMunmap> machine_code;

  std::unique_ptr<Regs> regs;

  std::shared_ptr<Object> Clone() const override;

  using PrologueFn = uintptr_t (*)(void*);

  PrologueFn prologue_fn = nullptr;

  Assembler(maf::Status&);
  ~Assembler();

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point);

  struct Widget : FallbackWidget {
    Widget(std::weak_ptr<Object> object) : FallbackWidget() { this->object = object; }
    float Width() const override { return 6_cm; }
    std::string Text() const override;
  };

  std::shared_ptr<gui::Widget> MakeWidget() override { return std::make_shared<Widget>(WeakPtr()); }
};

}  // namespace automat::library
