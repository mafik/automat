// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_assembler.hh"

#include <include/effects/SkGradientShader.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include "font.hh"
#include "svg.hh"

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

void Assembler::UpdateMachineCode() {}

void Assembler::RunMachineCode(library::Instruction* entry_point) {}

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
