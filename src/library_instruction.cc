// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_instruction.hh"

#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include "assembler.hh"
#include "automat.hh"

using namespace std;
using namespace llvm;

namespace automat::library {

void Instruction::Relocate(Location* new_here) {
  if (auto h = here.lock()) {
    assembler->instructions.erase(
        std::remove(assembler->instructions.begin(), assembler->instructions.end(), this),
        assembler->instructions.end());
  }
  LiveObject::Relocate(new_here);
  if (auto h = here.lock()) {
    assembler->instructions.push_back(this);
  }
}

string_view Instruction::Name() const {
  return "Instruction";
  // "ADD64rr"
  // return assembler->mc_inst_printer->getOpcodeName(mc_inst.getOpcode());

  // Nicely formatted assembly:
  // std::string str;
  // raw_string_ostream os(str);
  // assembler->mc_inst_printer->printInst(&mc_inst, 0, "", *assembler->mc_subtarget_info, os);
  // os.flush();
  // printf("%s\n", str.c_str());
  // return str;

  // "add"
  // return assembler->mc_inst_printer->getMnemonic(&mc_inst).first;
}
shared_ptr<Object> Instruction::Clone() const { return make_shared<Instruction>(*this); }

void Instruction::SetupDevelopmentScenario() {
  auto inst = Instruction();
  inst.mc_inst = MCInstBuilder(X86::ADD64rr).addReg(X86::RAX).addReg(X86::RAX).addReg(X86::RBX);
  auto& new_inst = root_machine->Create(inst);

  assembler->UpdateMachineCode();
}

LongRunning* Instruction::OnRun(Location& here) {
  assembler->RunMachineCode(this);
  return nullptr;
}

Instruction::Widget::Widget(std::weak_ptr<Object> object) { this->object = object; }

std::string Instruction::Widget::Text() const {
  auto obj = object.lock();
  Instruction* inst = dynamic_cast<Instruction*>(obj.get());

  // Nicely formatted assembly:
  std::string str;
  raw_string_ostream os(str);
  assembler->mc_inst_printer->printInst(&inst->mc_inst, 0, "", *assembler->mc_subtarget_info, os);
  os.flush();
  for (auto& c : str) {
    if (c == '\t') {
      c = ' ';
    }
  }
  if (str.starts_with(' ')) {
    str.erase(0, 1);
  }
  return str;
}

}  // namespace automat::library
