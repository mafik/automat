// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_assembler.hh"

#include <include/effects/SkGradientShader.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <thread>

#include "font.hh"
#include "svg.hh"
#include "thread_name.hh"

#if defined __linux__
#include <signal.h>
#include <sys/mman.h>  // For mmap related functions and constants
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#endif  // __linux__

#include "embedded.hh"
#include "global_resources.hh"
#include "library_instruction.hh"
#include "llvm_asm.hh"
#include "random.hh"
#include "status.hh"

#if defined _WIN32
#pragma comment(lib, "ntdll.lib")
#endif  // __linux__

using namespace llvm;
using namespace std;
using namespace maf;

namespace automat::library {

#if defined __linux__
void AssemblerSignalHandler(int sig, siginfo_t* si, struct ucontext_t* context) {
  // In Automat this handler will actually call Automat code to see what to do.
  // That code may block the current thread.
  // Response may be either to restart from scratch (longjmp), retry (return) or exit the thread
  // (longjmp).

  printf("\n*** Caught signal %d (%s) ***\n", sig, strsignal(sig));
  printf("Signal originated at address: %p\n", si->si_addr);
  printf("si_addr_lsb: %d\n", si->si_addr_lsb);
  __builtin_dump_struct(context, &printf);
  printf("gregs: ");
  auto& gregs = context->uc_mcontext.gregs;
  for (int i = 0; i < sizeof(gregs) / sizeof(gregs[0]); ++i) {
    printf("%lx ", gregs[i]);
  }
  printf("\n");

  // Print additional signal info
  printf("Signal code: %d\n", si->si_code);
  printf("Faulting process ID: %d\n", si->si_pid);
  printf("User ID of sender: %d\n", si->si_uid);

  exit(1);
}

static bool SetupSignalHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(struct sigaction));
  // sigemptyset(&sa.sa_mask);
  sigfillset(&sa.sa_mask);
  sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))AssemblerSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

  // if (sigaction(SIGTRAP, &sa, NULL) == -1) {
  //   ERROR << "Failed to set SIGTRAP handler: " << strerror(errno);
  //   return false;
  // }
  if (sigaction(SIGSEGV, &sa, NULL) == -1) {
    ERROR << "Failed to set SIGSEGV handler: " << strerror(errno);
    return false;
  }
  if (sigaction(SIGILL, &sa, NULL) == -1) {
    ERROR << "Failed to set SIGILL handler: " << strerror(errno);
    return false;
  }
  if (sigaction(SIGBUS, &sa, NULL) == -1) {
    ERROR << "Failed to set SIGBUS handler: " << strerror(errno);
    return false;
  }
  return true;
}

#else

// TODO: Implement signal handler on Windows
static bool SetupSignalHandler() { return true; }
#endif  // __linux__

// Note: We're not preserving RSP!
// CB(RSP)
#define REGS(CB) \
  CB(RBX)        \
  CB(RCX)        \
  CB(RDX)        \
  CB(RBP)        \
  CB(RSI)        \
  CB(RDI)        \
  CB(R8)         \
  CB(R9)         \
  CB(R10)        \
  CB(R11)        \
  CB(R12)        \
  CB(R13)        \
  CB(R14)        \
  CB(R15)

#define ALL_REGS(CB) \
  CB(RAX)            \
  REGS(CB)

struct Regs {
#define CB(reg) uint64_t reg = 0;
  ALL_REGS(CB);
  // CB(original_RSP);
#undef CB
  uint64_t operator[](int index) { return reinterpret_cast<uint64_t*>(this)[index]; }
};

Assembler::Assembler(Status& status) {
  static bool signal_handler_initialized =
      SetupSignalHandler();  // unused, ensures that initialization happens once

#if defined __linux__
  machine_code.reset((char*)mmap((void*)0x10000, kMachineCodeSize, PROT_READ | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#else
  // TODO: Implement on Windows
  machine_code.reset(new char[kMachineCodeSize]);
#endif  // __linux__

  regs = std::make_unique<Regs>();
  regs->RAX = 0x123456789abcdef0ull;  // dummy value for debuggin
}

Assembler::~Assembler() {}

std::shared_ptr<Object> Assembler::Clone() const {
  Status status;
  auto obj = std::make_shared<Assembler>(status);
  if (OK(status)) {
    return obj;
  }
  return nullptr;
}

void DeleteWithMunmap::operator()(void* ptr) const {
#if defined __linux__
  munmap(ptr, kMachineCodeSize);
#else
  // TODO: Implement on Windows
  delete[] (char*)ptr;
#endif  // __linux__
}

void Assembler::UpdateMachineCode() {
#if defined __linux__
  mprotect(machine_code.get(), kMachineCodeSize, PROT_READ | PROT_WRITE);
#else
  // TODO: Implement on Windows
#endif  // __linux__

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

  if constexpr (false) {
    string machine_code_str;
    for (int i = 0; i < machine_code_offset; i++) {
      machine_code_str += f("%02x ", machine_code.get()[i]);
      if (i % 16 == 15) {
        machine_code_str += "\n";
      }
    }
    LOG << machine_code_str;
  }

  if constexpr (false) {
    string epilogue_prologue_str;
    for (int i = 0; i < epilogue_prologue.size(); i++) {
      epilogue_prologue_str += f("%02x ", epilogue_prologue[i]);
      if (i % 16 == 15) {
        epilogue_prologue_str += "\n";
      }
    }
    LOG << epilogue_prologue_str;
  }

#if defined __linux__
  mprotect(machine_code.get(), kMachineCodeSize, PROT_READ | PROT_EXEC);
#else
  // TODO: Implement on Windows
#endif  // __linux__
}

static void GetRegs(const std::weak_ptr<Object>& object, Regs& regs) {
  auto obj = object.lock();
  if (!obj) {
    ERROR << "Assembler was released";
    return;
  }
  auto assembler = std::dynamic_pointer_cast<Assembler>(obj);
  if (!assembler) {
    ERROR << "Assembler Widget cannot display non-assembler objects";
    return;
  }
  pid_t pid = assembler->pid;
  if (pid > 0 && assembler->is_running) {
    int ret = ptrace(PTRACE_SEIZE, pid, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);
    if (ret != 0) {
      ERROR << "PTRACE_SEIZE failed: " << strerror(errno);
      return;
    }
    ret = ptrace(PTRACE_INTERRUPT, pid, 0, 0);
    if (ret != 0) {
      ERROR << "PTRACE_INTERRUPT failed: " << strerror(errno);
      return;
    }
    int status;
    ret = waitpid(pid, &status, WSTOPPED);
    if (ret == -1) {
      ERROR << "waitpid failed: " << strerror(errno);
      return;
    }
    user_regs_struct linux_regs;
    ret = ptrace(PTRACE_GETREGS, pid, 0, &linux_regs);
    if (ret != 0) {
      ERROR << "PTRACE_GETREGS failed: " << strerror(errno);
      return;
    }
    regs.RAX = linux_regs.rax;
    // ret = ptrace(PTRACE_CONT, pid, 0, 0);
    // if (ret != 0) {
    //   ERROR << "PTRACE_CONT failed: " << strerror(errno);
    //   return "PTRACE_CONT failed";
    // }
    ret = ptrace(PTRACE_DETACH, pid, 0, 0);
    if (ret != 0) {
      ERROR << "PTRACE_DETACH failed: " << strerror(errno);
      return;
    }
  } else {
    regs = *assembler->regs;
  }
}

void Assembler::RunMachineCode(library::Instruction* entry_point) {
#if defined __linux__
  struct ThreadArg {
    Assembler* assembler;
    void* entry_point;
  };
  auto Thread = [](void* void_arg) {
    SetThreadName("Assembler");
    printf("Thread started\n");
    ThreadArg* arg = (ThreadArg*)void_arg;
    auto& assembler = *arg->assembler;
    assembler.is_running = true;
    auto ret = assembler.prologue_fn(arg->entry_point);
    assembler.is_running = false;
    ret -= (uintptr_t)assembler.machine_code.get();
    printf("Assembler returned: %lx\n", ret);
    return 0;
  };
  ThreadArg arg = {this, entry_point->address};

  int STACK_SIZE = 8 * 1024 * 1024;
  std::vector<char> stack;
  stack.resize(STACK_SIZE);
  if (clone(Thread, (void*)(stack.data() + STACK_SIZE),
            CLONE_PARENT_SETTID | CLONE_SIGHAND | CLONE_FILES | CLONE_FS | CLONE_IO | CLONE_VM,
            &arg, &pid) == -1) {
    ERROR << "failed to spawn child task: " << strerror(errno);
    return;
  }

  printf("t1_pid: %d\n", pid);
#else
  // TODO: Implement on Windows
#endif  // __linux__
}

AssemblerWidget::AssemblerWidget(std::weak_ptr<Object> object) {
  this->object = object;
  children.emplace_back(std::make_shared<RegisterWidget>(object, 0));
}

std::string_view AssemblerWidget::Name() const { return "Assembler"; }
SkPath AssemblerWidget::Shape() const { return SkPath::RRect(kRRect.sk); }

void AssemblerWidget::Draw(SkCanvas& canvas) const {
  static constexpr float kFlatBorderWidth = 3_mm;
  static constexpr RRect kBorderMidRRect = kRRect.Outset(-kFlatBorderWidth);
  static constexpr RRect kInnerRRect = kBorderMidRRect.Outset(-kFlatBorderWidth);
  float one_pixel = 1.0f / canvas.getTotalMatrix().getScaleX();
  SkPaint flat_border_paint;
  flat_border_paint.setColor("#9b252a"_color);
  canvas.drawDRRect(kRRect.sk, kBorderMidRRect.sk, flat_border_paint);
  SkPaint bevel_border_paint;
  bevel_border_paint.setColor("#7d2627"_color);
  canvas.drawDRRect(kBorderMidRRect.sk, kInnerRRect.sk, bevel_border_paint);
  SkPaint bg_paint = [&]() {
    static auto builder =
        resources::RuntimeEffectBuilder(embedded::assets_assembler_stars_rt_sksl.content);

    builder->uniform("uv_to_pixel") = canvas.getTotalMatrix();

    auto shader = builder->makeShader();
    SkPaint paint;
    paint.setShader(shader);
    return paint;
  }();
  canvas.drawRRect(kInnerRRect.Outset(one_pixel).sk, bg_paint);

  Regs regs;
  GetRegs(object, regs);
  DrawChildren(canvas);
}

void AssemblerWidget::FillChildren(maf::Vec<std::shared_ptr<gui::Widget>>& children) {
  for (auto& child : this->children) {
    children.push_back(child);
  }
}

std::unique_ptr<Action> AssemblerWidget::FindAction(gui::Pointer& p, gui::ActionTrigger btn) {
  if (btn == gui::PointerButton::Left) {
    auto* location = Closest<Location>(*p.hover);
    auto* machine = Closest<Machine>(*p.hover);
    if (location && machine) {
      auto contact_point = p.PositionWithin(*this);
      auto a = std::make_unique<DragLocationAction>(p, machine->Extract(*location));
      a->contact_point = contact_point;
      return a;
    }
  }
  return nullptr;
}

void AssemblerWidget::TransformUpdated() { WakeAnimation(); }

SkPath RegisterWidget::Shape() const { return SkPath::Rect(kBoundingRect.sk); }
std::string_view RegisterWidget::Name() const { return "Register"; }

static const SkPath kFlagPole = PathFromSVG(
    "m-.5-.7c-1.8-7.1-2.3-14.5-2.5-21.9-.3.2-.8.3-1.3.4.7-1 1.4-1.8 1.8-3 .3 1.2.8 2 1.6 2.9-.4 "
    "0-.7-.1-1.2-.3 0 7.4 1 14.7 2.5 21.9.5.2.8.5.9.7h-2.5c.1-.2.3-.5.7-.7z");

static const SkPath kFlag = PathFromSVG(
    R"(m-3.5-21.7c.2-.5 3.1 1 4.6.9 1.6-.1 3.1-1.4 4.7-1.3 1.5.1 2.6 1.8 4.1 1.9 2 .2 3.9-1.4 6-1.5 2.7-.1 8 1.2 8 1.2s-6.7 1-9.7 2.5c-1.8.8-2.8 3-4.7 3.6-1.3.4-2.6-.7-3.9-.4-1.7.4-2.8 2.2-4.4 2.8-1.3.5-4.1.9-4.2.5-.4-3.4-.8-6.6-.6-10.2z)");

static constexpr float kBitPositionFontSize = RegisterWidget::kCellHeight * 0.42;

static gui::Font& BitPositionFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetGrenzeRegular(), kBitPositionFontSize);
  return *font;
}

static constexpr float kByteValueFontSize = 3_mm;  // RegisterWidget::kCellHeight * 1;

static gui::Font& ByteValueFont() {
  static auto font = gui::Font::MakeV2(gui::Font::GetHeavyData(), kByteValueFontSize);
  return *font;
}

// Shift the byte values up so that they're vertically centered with their rows
static constexpr float kByteValueFontShiftUp =
    (RegisterWidget::kCellHeight - kByteValueFontSize) / 2;

// Shift the font up, so that its top is aligned with the middle of the cell
static constexpr float kBitPositionFontShiftUp =
    RegisterWidget::kCellHeight / 2 - kBitPositionFontSize;

void RegisterWidget::Draw(SkCanvas& canvas) const {
  auto object_shared = object.lock();
  auto assembler = static_cast<Assembler*>(object_shared.get());
  uint64_t reg_value = (*assembler->regs)[register_index];

  SkPaint dark_paint;
  dark_paint.setColor("#dcca85"_color);
  canvas.drawRect(kBaseRect.sk, dark_paint);
  SkPaint light_paint;
  light_paint.setColor("#fefdfb"_color);

  auto& bit_position_font = BitPositionFont();
  auto& byte_value_font = ByteValueFont();
  for (int row = 0; row < 8; ++row) {
    float bottom = kInnerRect.bottom + kCellHeight * row;
    float top = bottom + kCellHeight;
    int byte_value = (reg_value >> (row * 8)) & 0xFF;
    canvas.save();
    canvas.translate(kBaseRect.right + 0.5_mm, bottom + kByteValueFontShiftUp);
    auto byte_value_str = f("%X", byte_value);
    byte_value_font.DrawText(canvas, byte_value_str, dark_paint);
    canvas.restore();
    for (int bit = 0; bit < 8; ++bit) {
      float right = kInnerRect.right - kCellWidth * bit;
      float left = right - kCellWidth;
      SkPaint* cell_paint = &light_paint;
      if (bit % 2 == row % 2) {
        // light cell
        canvas.drawRect(SkRect::MakeLTRB(left, bottom, right, top), light_paint);
        cell_paint = &dark_paint;
      }

      int position = row * 8 + bit;
      std::string position_str = f("%d", position);
      float position_text_width = bit_position_font.MeasureText(position_str);
      canvas.save();
      canvas.translate(left + (kCellWidth - position_text_width) * 0.5,
                       bottom + kBitPositionFontShiftUp);
      bit_position_font.DrawText(canvas, position_str, *cell_paint);
      canvas.restore();

      SkPaint pole_paint;
      SkPaint flag_paint;
      SkPoint points[2] = {SkPoint::Make(-kCellWidth * 0.2, 0),
                           SkPoint::Make(kCellWidth * 1.2, kCellHeight * 0.1)};
      SkColor colors[5] = {"#ff0000"_color, "#800000"_color, "#ff0000"_color, "#800000"_color,
                           "#ff0000"_color};
      flag_paint.setShader(
          SkGradientShader::MakeLinear(points, colors, nullptr, 5, SkTileMode::kClamp));
      if (reg_value & (1ULL << position)) {
        canvas.save();
        canvas.translate(left + kCellWidth * 0.2, bottom);
        canvas.scale(0.5, 0.5);
        canvas.drawPath(kFlagPole, pole_paint);
        canvas.drawPath(kFlag, flag_paint);
        canvas.restore();
      }
    }
  }

  canvas.save();

  canvas.translate(-kRegisterIconWidth / 2, kBaseRect.top - kRegisterIconWidth * 0.15);
  kRegisters[register_index].image.draw(canvas);
  canvas.restore();
}

}  // namespace automat::library
