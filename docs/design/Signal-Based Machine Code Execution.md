# Signal-Based Machine Code Execution

Note for future: It's possible to execute machine code in a current thread and use thread-targetted signals (tgkill) to inspect the values of registers while it's executing.

This should be more efficient on Linux machines, but it's unclear whether a similar approach would work on other OSes.

The internal interface should be analogous to PtraceMachineCodeController.

Here is a reference for emitting machine code that takes care of its own registers:

```c++

  memset(machine_code.get(), 0x90, kMachineCodeSize);

  size_t machine_code_offset = 0;

  SmallVector<char, 256> epilogue_prologue;
  SmallVector<MCFixup, 4> epilogue_prologue_fixups;
  int64_t regs_addr = reinterpret_cast<int64_t>(regs.get());

  auto& llvm_asm = LLVM_Assembler::Get();
  auto& mc_code_emitter = llvm_asm.mc_code_emitter;
  auto& mc_subtarget_info = llvm_asm.mc_subtarget_info;

  auto MOVmRAX = [&](int64_t addr) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64o64a).addImm(addr).addReg(0),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };
  auto PUSHr = [&](unsigned reg) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(reg), epilogue_prologue,
                                       epilogue_prologue_fixups, *mc_subtarget_info);
  };
  auto POPr = [&](unsigned reg) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(reg), epilogue_prologue,
                                       epilogue_prologue_fixups, *mc_subtarget_info);
  };
  auto MOVri = [&](unsigned reg, uint64_t imm) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64ri).addReg(reg).addImm(imm),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };
  auto MOVmr = [&](unsigned addr_reg, unsigned addr_offset, unsigned reg) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64mr)
                                           .addReg(addr_reg)
                                           .addImm(1)
                                           .addReg(0)
                                           .addImm(addr_offset)
                                           .addReg(0)
                                           .addReg(reg),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };
  auto MOVrm = [&](unsigned reg, unsigned addr_reg, unsigned addr_offset) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64rm)
                                           .addReg(reg)
                                           .addReg(addr_reg)
                                           .addImm(1)
                                           .addReg(0)
                                           .addImm(addr_offset)
                                           .addReg(0),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };

  // # EPILOGUE:

  MOVmRAX(regs_addr);          // Store RAX at the start of Regs
  MOVri(X86::RAX, regs_addr);  // Put Regs address in RAX
#define SAVE(reg) MOVmr(X86::RAX, offsetof(Regs, reg), X86::reg);
  REGS(SAVE);  // Save all registers to Regs
#undef SAVE
  // MOVrm(X86::RSP, X86::RAX, offsetof(Regs, original_RSP));

  // Store the 64-bit address of the exit point in
  POPr(X86::RAX);

  // Restore callee-saved registers
  POPr(X86::R15);
  POPr(X86::R14);
  POPr(X86::R13);
  POPr(X86::R12);
  POPr(X86::RBP);
  POPr(X86::RBX);

  // Return
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::RET32), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);

  auto epilogue_size = epilogue_prologue.size();

  // # PROLOGUE: (goes right after the epilogue)
  // Save callee-saved registers:

  PUSHr(X86::RBX);
  PUSHr(X86::RBP);
  PUSHr(X86::R12);
  PUSHr(X86::R13);
  PUSHr(X86::R14);
  PUSHr(X86::R15);

  PUSHr(X86::RDI);  // Push the first argument (RDI) so that it can be used to "RET" into the right
                    // address

  MOVri(X86::RAX, regs_addr);
  // MOVmr(X86::RAX, offsetof(Regs, original_RSP), X86::RSP);
#define LOAD(reg) MOVrm(X86::reg, X86::RAX, offsetof(Regs, reg));
  REGS(LOAD);
  LOAD(RAX);  // load RAX last because it's used as a base for address
#undef LOAD

  // Jump to the first instruction (on top of the stack)
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::RET64), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);

  auto prologue_size = epilogue_prologue.size() - epilogue_size;

  prologue_fn = reinterpret_cast<PrologueFn>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                             kMachineCodeSize - prologue_size);

  // Copy epilogue/prologue at the end of machine_code.
  void* epilogue_prologue_dest = reinterpret_cast<void*>(
      reinterpret_cast<intptr_t>(machine_code.get()) + kMachineCodeSize - epilogue_prologue.size());
  memcpy(epilogue_prologue_dest, epilogue_prologue.data(), epilogue_size + prologue_size);
  int64_t epilogue_prologue_addr = reinterpret_cast<int64_t>(epilogue_prologue_dest);

  // Find all the x86 instructions
  auto here_ptr = here.lock();
  auto [begin, end] = here_ptr->incoming.equal_range(&assembler_arg);
  struct InstructionEntry {
    Location* loc;
    Instruction* inst;
    Connection* conn;
    Vec2 position;
  };
  Vec<InstructionEntry> instructions;
  for (auto it = begin; it != end; ++it) {
    auto& conn = *it;
    auto& inst_loc = conn->from;
    auto inst = inst_loc.As<Instruction>();
    instructions.push_back({&inst_loc, inst, conn, inst_loc.position});
  }

  for (auto& entry : instructions) {
    entry.inst->address = nullptr;
  }

  struct Fixup {
    size_t end_offset;  // place in machine_code where the fixup ends
    Instruction* target = nullptr;
    MCFixupKind kind = MCFixupKind::FK_PCRel_4;
  };

  Vec<Fixup> machine_code_fixups;

  auto EmitInstruction = [&](Location& loc, Instruction& inst) {
    SmallVector<char, 32> instruction_bytes;
    SmallVector<MCFixup, 4> instruction_fixups;
    mc_code_emitter->encodeInstruction(inst.mc_inst, instruction_bytes, instruction_fixups,
                                       *mc_subtarget_info);

    auto inst_size = instruction_bytes.size();

    void* dest = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                         machine_code_offset);
    memcpy(dest, instruction_bytes.data(), instruction_bytes.size());
    machine_code_offset += instruction_bytes.size();

    if (instruction_fixups.size() > 1) {
      ERROR << "Instructions with more than one fixup not supported!";
    } else if (instruction_fixups.size() == 1) {
      auto& fixup = instruction_fixups[0];

      Instruction* jump_inst = nullptr;
      if (auto it = loc.outgoing.find(&jump_arg); it != loc.outgoing.end()) {
        jump_inst = (*it)->to.As<Instruction>();
      }
      // We assume that the fixup is at the end of the instruction.
      machine_code_fixups.push_back({machine_code_offset, jump_inst, fixup.getKind()});
    }
  };

  auto Fixup1 = [&](size_t fixup_end, size_t target_offset) {
    ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
    char* code = machine_code.get();
    code[fixup_end - 1] = (pcrel & 0xFF);
  };

  auto Fixup4 = [&](size_t fixup_end, size_t target_offset) {
    ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
    char* code = machine_code.get();
    code[fixup_end - 4] = (pcrel & 0xFF);
    code[fixup_end - 3] = ((pcrel >> 8) & 0xFF);
    code[fixup_end - 2] = ((pcrel >> 16) & 0xFF);
    code[fixup_end - 1] = ((pcrel >> 24) & 0xFF);
  };

  auto EmitJump = [&](size_t target_offset) {
    SmallVector<char, 32> instruction_bytes;
    SmallVector<MCFixup, 4> instruction_fixups;
    // Save the current RIP and jump to epilogue
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::JMP_4).addImm(0), instruction_bytes,
                                       instruction_fixups, *mc_subtarget_info);
    void* dest = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                         machine_code_offset);
    memcpy(dest, instruction_bytes.data(), instruction_bytes.size());
    machine_code_offset += instruction_bytes.size();
    Fixup4(machine_code_offset, target_offset);
  };

  auto EmitExitPoint = [&]() {
    SmallVector<char, 32> instruction_bytes;
    SmallVector<MCFixup, 4> instruction_fixups;
    // Save the current RIP and jump to epilogue
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::CALL64pcrel32).addImm(0),
                                       instruction_bytes, instruction_fixups, *mc_subtarget_info);
    void* dest = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                         machine_code_offset);
    memcpy(dest, instruction_bytes.data(), instruction_bytes.size());
    machine_code_offset += instruction_bytes.size();
    size_t jmp_target = kMachineCodeSize - epilogue_prologue.size();
    Fixup4(machine_code_offset, jmp_target);
  };

  for (auto& entry : instructions) {
    Location* loc = entry.loc;
    Instruction* inst = entry.inst;
    if (inst->address) {
      continue;
    }

    while (inst) {
      if (inst->address) {
        // current instruction was already emitted - jump to it instead
        EmitJump(reinterpret_cast<size_t>(inst->address) -
                 reinterpret_cast<size_t>(machine_code.get()));
        break;
      }
      inst->address = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                              machine_code_offset);
      EmitInstruction(*loc, *inst);

      if (auto it = loc->outgoing.find(&next_arg); it != loc->outgoing.end()) {
        loc = &(*it)->to;
        inst = loc->As<Instruction>();
      } else {
        loc = nullptr;
        inst = nullptr;
      }
    }
    // TODO: don't emit the final exit point if the last instruction is a terminator
    EmitExitPoint();

    for (int fixup_i = 0; fixup_i < machine_code_fixups.size(); ++fixup_i) {
      auto& fixup = machine_code_fixups[fixup_i];
      size_t target_offset = 0;
      if (fixup.target) {                        // target is an instruction
        if (fixup.target->address == nullptr) {  // it wasn't emitted yet - keep the fixup around
          continue;
        }
        target_offset = reinterpret_cast<size_t>(fixup.target->address);
      } else {  // target is not an instruction - create a new exit point and jump there
        target_offset = machine_code_offset;
        EmitExitPoint();
      }
      if (fixup.kind == MCFixupKind::FK_PCRel_4) {
        Fixup4(fixup.end_offset, target_offset);
      } else if (fixup.kind == MCFixupKind::FK_PCRel_1) {
        Fixup1(fixup.end_offset, target_offset);
      } else {
        ERROR << "Unsupported fixup kind: " << fixup.kind;
      }
      machine_code_fixups.EraseIndex(fixup_i);
      --fixup_i;
    }
  }

  if (!machine_code_fixups.empty()) {
    ERROR << "Not all fixups were resolved!";
  }

```