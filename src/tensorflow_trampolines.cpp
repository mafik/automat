// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// Trampolines for the TensorFlow C++ symbols automat references. Each is a
// hidden function carrying the library's mangled name; automat's call sites and
// the inline header code resolve to it, and it jumps to the address dlsym wrote
// into its slot (tf_bind_symbols, called once from tensorflow_runtime.cpp).
// This lets automat call the embedded libtensorflow.so without linking or
// naming it - the ELF twin of the Windows delay-load. The set is every symbol
// tensorflow_runtime.o leaves undefined that libtensorflow.so exports;
// regenerate it when tensorflow_runtime.cpp's TensorFlow API surface changes.

#include "tensorflow_trampolines.hpp"

#ifndef _WIN32  // Windows delay-loads the DLL instead (tensorflow_runtime.cpp).

#include <dlfcn.h>

#include <cstdio>

extern "C" {
__attribute__((visibility("hidden"))) void* tf_slot_0;
__attribute__((visibility("hidden"))) void* tf_slot_1;
__attribute__((visibility("hidden"))) void* tf_slot_2;
__attribute__((visibility("hidden"))) void* tf_slot_3;
__attribute__((visibility("hidden"))) void* tf_slot_4;
__attribute__((visibility("hidden"))) void* tf_slot_5;
__attribute__((visibility("hidden"))) void* tf_slot_6;
__attribute__((visibility("hidden"))) void* tf_slot_7;
__attribute__((visibility("hidden"))) void* tf_slot_8;
__attribute__((visibility("hidden"))) void* tf_slot_9;
__attribute__((visibility("hidden"))) void* tf_slot_10;
__attribute__((visibility("hidden"))) void* tf_slot_11;
__attribute__((visibility("hidden"))) void* tf_slot_12;
__attribute__((visibility("hidden"))) void* tf_slot_13;
__attribute__((visibility("hidden"))) void* tf_slot_14;
__attribute__((visibility("hidden"))) void* tf_slot_15;
__attribute__((visibility("hidden"))) void* tf_slot_16;
__attribute__((visibility("hidden"))) void* tf_slot_17;
__attribute__((visibility("hidden"))) void* tf_slot_18;
__attribute__((visibility("hidden"))) void* tf_slot_19;
__attribute__((visibility("hidden"))) void* tf_slot_20;
__attribute__((visibility("hidden"))) void* tf_slot_21;
__attribute__((visibility("hidden"))) void* tf_slot_22;
__attribute__((visibility("hidden"))) void* tf_slot_23;
__attribute__((visibility("hidden"))) void* tf_slot_24;
__attribute__((visibility("hidden"))) void* tf_slot_25;
__attribute__((visibility("hidden"))) void* tf_slot_26;
__attribute__((visibility("hidden"))) void* tf_slot_27;
__attribute__((visibility("hidden"))) void* tf_slot_28;
__attribute__((visibility("hidden"))) void* tf_slot_29;
}

asm(R"(
  .text
  .globl _ZN10tensorflow10NewSessionERKNS_14SessionOptionsE
  .hidden _ZN10tensorflow10NewSessionERKNS_14SessionOptionsE
  .type _ZN10tensorflow10NewSessionERKNS_14SessionOptionsE,@function
_ZN10tensorflow10NewSessionERKNS_14SessionOptionsE:
  jmp *tf_slot_0(%rip)
  .globl _ZN10tensorflow10OpRegistry6GlobalEv
  .hidden _ZN10tensorflow10OpRegistry6GlobalEv
  .type _ZN10tensorflow10OpRegistry6GlobalEv,@function
_ZN10tensorflow10OpRegistry6GlobalEv:
  jmp *tf_slot_1(%rip)
  .globl _ZN10tensorflow11ConfigProtoD1Ev
  .hidden _ZN10tensorflow11ConfigProtoD1Ev
  .type _ZN10tensorflow11ConfigProtoD1Ev,@function
_ZN10tensorflow11ConfigProtoD1Ev:
  jmp *tf_slot_2(%rip)
  .globl _ZN10tensorflow11NodeBuilder5InputEPNS_4NodeEi
  .hidden _ZN10tensorflow11NodeBuilder5InputEPNS_4NodeEi
  .type _ZN10tensorflow11NodeBuilder5InputEPNS_4NodeEi,@function
_ZN10tensorflow11NodeBuilder5InputEPNS_4NodeEi:
  jmp *tf_slot_3(%rip)
  .globl _ZN10tensorflow11NodeBuilder8FinalizeEPNS_5GraphEPPNS_4NodeEb
  .hidden _ZN10tensorflow11NodeBuilder8FinalizeEPNS_5GraphEPPNS_4NodeEb
  .type _ZN10tensorflow11NodeBuilder8FinalizeEPNS_5GraphEPPNS_4NodeEb,@function
_ZN10tensorflow11NodeBuilder8FinalizeEPNS_5GraphEPPNS_4NodeEb:
  jmp *tf_slot_4(%rip)
  .globl _ZN10tensorflow11NodeBuilderC1ESt17basic_string_viewIcSt11char_traitsIcEES4_PKNS_19OpRegistryInterfaceEPKNS_13NodeDebugInfoE
  .hidden _ZN10tensorflow11NodeBuilderC1ESt17basic_string_viewIcSt11char_traitsIcEES4_PKNS_19OpRegistryInterfaceEPKNS_13NodeDebugInfoE
  .type _ZN10tensorflow11NodeBuilderC1ESt17basic_string_viewIcSt11char_traitsIcEES4_PKNS_19OpRegistryInterfaceEPKNS_13NodeDebugInfoE,@function
_ZN10tensorflow11NodeBuilderC1ESt17basic_string_viewIcSt11char_traitsIcEES4_PKNS_19OpRegistryInterfaceEPKNS_13NodeDebugInfoE:
  jmp *tf_slot_5(%rip)
  .globl _ZN10tensorflow14NodeDefBuilder4AttrESt17basic_string_viewIcSt11char_traitsIcEENS_8DataTypeE
  .hidden _ZN10tensorflow14NodeDefBuilder4AttrESt17basic_string_viewIcSt11char_traitsIcEENS_8DataTypeE
  .type _ZN10tensorflow14NodeDefBuilder4AttrESt17basic_string_viewIcSt11char_traitsIcEENS_8DataTypeE,@function
_ZN10tensorflow14NodeDefBuilder4AttrESt17basic_string_viewIcSt11char_traitsIcEENS_8DataTypeE:
  jmp *tf_slot_6(%rip)
  .globl _ZN10tensorflow14SessionOptionsC1Ev
  .hidden _ZN10tensorflow14SessionOptionsC1Ev
  .type _ZN10tensorflow14SessionOptionsC1Ev,@function
_ZN10tensorflow14SessionOptionsC1Ev:
  jmp *tf_slot_7(%rip)
  .globl _ZN10tensorflow14TensorShapeRep12SlowCopyFromERKS0_
  .hidden _ZN10tensorflow14TensorShapeRep12SlowCopyFromERKS0_
  .type _ZN10tensorflow14TensorShapeRep12SlowCopyFromERKS0_,@function
_ZN10tensorflow14TensorShapeRep12SlowCopyFromERKS0_:
  jmp *tf_slot_8(%rip)
  .globl _ZN10tensorflow14TensorShapeRep19DestructorOutOfLineEv
  .hidden _ZN10tensorflow14TensorShapeRep19DestructorOutOfLineEv
  .type _ZN10tensorflow14TensorShapeRep19DestructorOutOfLineEv,@function
_ZN10tensorflow14TensorShapeRep19DestructorOutOfLineEv:
  jmp *tf_slot_9(%rip)
  .globl _ZN10tensorflow15TensorShapeBaseINS_11TensorShapeEEC2EN4absl12lts_202501274SpanIKlEE
  .hidden _ZN10tensorflow15TensorShapeBaseINS_11TensorShapeEEC2EN4absl12lts_202501274SpanIKlEE
  .type _ZN10tensorflow15TensorShapeBaseINS_11TensorShapeEEC2EN4absl12lts_202501274SpanIKlEE,@function
_ZN10tensorflow15TensorShapeBaseINS_11TensorShapeEEC2EN4absl12lts_202501274SpanIKlEE:
  jmp *tf_slot_10(%rip)
  .globl _ZN10tensorflow5GraphC1EPKNS_19OpRegistryInterfaceE
  .hidden _ZN10tensorflow5GraphC1EPKNS_19OpRegistryInterfaceE
  .type _ZN10tensorflow5GraphC1EPKNS_19OpRegistryInterfaceE,@function
_ZN10tensorflow5GraphC1EPKNS_19OpRegistryInterfaceE:
  jmp *tf_slot_11(%rip)
  .globl _ZN10tensorflow5GraphD1Ev
  .hidden _ZN10tensorflow5GraphD1Ev
  .type _ZN10tensorflow5GraphD1Ev,@function
_ZN10tensorflow5GraphD1Ev:
  jmp *tf_slot_12(%rip)
  .globl _ZN10tensorflow6TensorC1ENS_8DataTypeERKNS_11TensorShapeE
  .hidden _ZN10tensorflow6TensorC1ENS_8DataTypeERKNS_11TensorShapeE
  .type _ZN10tensorflow6TensorC1ENS_8DataTypeERKNS_11TensorShapeE,@function
_ZN10tensorflow6TensorC1ENS_8DataTypeERKNS_11TensorShapeE:
  jmp *tf_slot_13(%rip)
  .globl _ZN10tensorflow6TensorC1Ev
  .hidden _ZN10tensorflow6TensorC1Ev
  .type _ZN10tensorflow6TensorC1Ev,@function
_ZN10tensorflow6TensorC1Ev:
  jmp *tf_slot_14(%rip)
  .globl _ZN10tensorflow6TensorD1Ev
  .hidden _ZN10tensorflow6TensorD1Ev
  .type _ZN10tensorflow6TensorD1Ev,@function
_ZN10tensorflow6TensorD1Ev:
  jmp *tf_slot_15(%rip)
  .globl _ZN10tensorflow7NodeDefD1Ev
  .hidden _ZN10tensorflow7NodeDefD1Ev
  .type _ZN10tensorflow7NodeDefD1Ev,@function
_ZN10tensorflow7NodeDefD1Ev:
  jmp *tf_slot_16(%rip)
  .globl _ZN10tensorflow8GraphDefC2EPN6google8protobuf5ArenaE
  .hidden _ZN10tensorflow8GraphDefC2EPN6google8protobuf5ArenaE
  .type _ZN10tensorflow8GraphDefC2EPN6google8protobuf5ArenaE,@function
_ZN10tensorflow8GraphDefC2EPN6google8protobuf5ArenaE:
  jmp *tf_slot_17(%rip)
  .globl _ZN10tensorflow8GraphDefD1Ev
  .hidden _ZN10tensorflow8GraphDefD1Ev
  .type _ZN10tensorflow8GraphDefD1Ev,@function
_ZN10tensorflow8GraphDefD1Ev:
  jmp *tf_slot_18(%rip)
  .globl _ZN3tsl8internal15LogMessageFatalC1EPKci
  .hidden _ZN3tsl8internal15LogMessageFatalC1EPKci
  .type _ZN3tsl8internal15LogMessageFatalC1EPKci,@function
_ZN3tsl8internal15LogMessageFatalC1EPKci:
  jmp *tf_slot_19(%rip)
  .globl _ZN3tsl8internal15LogMessageFatalD1Ev
  .hidden _ZN3tsl8internal15LogMessageFatalD1Ev
  .type _ZN3tsl8internal15LogMessageFatalD1Ev,@function
_ZN3tsl8internal15LogMessageFatalD1Ev:
  jmp *tf_slot_20(%rip)
  .globl _ZN3tsl8internal21CheckOpMessageBuilder7ForVar2Ev
  .hidden _ZN3tsl8internal21CheckOpMessageBuilder7ForVar2Ev
  .type _ZN3tsl8internal21CheckOpMessageBuilder7ForVar2Ev,@function
_ZN3tsl8internal21CheckOpMessageBuilder7ForVar2Ev:
  jmp *tf_slot_21(%rip)
  .globl _ZN3tsl8internal21CheckOpMessageBuilder9NewStringB5cxx11Ev
  .hidden _ZN3tsl8internal21CheckOpMessageBuilder9NewStringB5cxx11Ev
  .type _ZN3tsl8internal21CheckOpMessageBuilder9NewStringB5cxx11Ev,@function
_ZN3tsl8internal21CheckOpMessageBuilder9NewStringB5cxx11Ev:
  jmp *tf_slot_22(%rip)
  .globl _ZN3tsl8internal21CheckOpMessageBuilderC1EPKc
  .hidden _ZN3tsl8internal21CheckOpMessageBuilderC1EPKc
  .type _ZN3tsl8internal21CheckOpMessageBuilderC1EPKc,@function
_ZN3tsl8internal21CheckOpMessageBuilderC1EPKc:
  jmp *tf_slot_23(%rip)
  .globl _ZN3tsl8internal21CheckOpMessageBuilderD1Ev
  .hidden _ZN3tsl8internal21CheckOpMessageBuilderD1Ev
  .type _ZN3tsl8internal21CheckOpMessageBuilderD1Ev,@function
_ZN3tsl8internal21CheckOpMessageBuilderD1Ev:
  jmp *tf_slot_24(%rip)
  .globl _ZNK10tensorflow15TensorShapeBaseINS_11TensorShapeEE8dim_sizeEi
  .hidden _ZNK10tensorflow15TensorShapeBaseINS_11TensorShapeEE8dim_sizeEi
  .type _ZNK10tensorflow15TensorShapeBaseINS_11TensorShapeEE8dim_sizeEi,@function
_ZNK10tensorflow15TensorShapeBaseINS_11TensorShapeEE8dim_sizeEi:
  jmp *tf_slot_25(%rip)
  .globl _ZNK10tensorflow4Node4nameB5cxx11Ev
  .hidden _ZNK10tensorflow4Node4nameB5cxx11Ev
  .type _ZNK10tensorflow4Node4nameB5cxx11Ev,@function
_ZNK10tensorflow4Node4nameB5cxx11Ev:
  jmp *tf_slot_26(%rip)
  .globl _ZNK10tensorflow5Graph10ToGraphDefEPNS_8GraphDefEbb
  .hidden _ZNK10tensorflow5Graph10ToGraphDefEPNS_8GraphDefEbb
  .type _ZNK10tensorflow5Graph10ToGraphDefEPNS_8GraphDefEbb,@function
_ZNK10tensorflow5Graph10ToGraphDefEPNS_8GraphDefEbb:
  jmp *tf_slot_27(%rip)
  .globl _ZNK10tensorflow6Tensor21CheckTypeAndIsAlignedENS_8DataTypeE
  .hidden _ZNK10tensorflow6Tensor21CheckTypeAndIsAlignedENS_8DataTypeE
  .type _ZNK10tensorflow6Tensor21CheckTypeAndIsAlignedENS_8DataTypeE,@function
_ZNK10tensorflow6Tensor21CheckTypeAndIsAlignedENS_8DataTypeE:
  jmp *tf_slot_28(%rip)
  .globl _ZNK4absl12lts_2025012715status_internal9StatusRep5UnrefEv
  .hidden _ZNK4absl12lts_2025012715status_internal9StatusRep5UnrefEv
  .type _ZNK4absl12lts_2025012715status_internal9StatusRep5UnrefEv,@function
_ZNK4absl12lts_2025012715status_internal9StatusRep5UnrefEv:
  jmp *tf_slot_29(%rip)
)");

namespace {
const struct {
  const char* name;
  void** slot;
} kSymbols[] = {
    {"_ZN10tensorflow10NewSessionERKNS_14SessionOptionsE", &tf_slot_0},
    {"_ZN10tensorflow10OpRegistry6GlobalEv", &tf_slot_1},
    {"_ZN10tensorflow11ConfigProtoD1Ev", &tf_slot_2},
    {"_ZN10tensorflow11NodeBuilder5InputEPNS_4NodeEi", &tf_slot_3},
    {"_ZN10tensorflow11NodeBuilder8FinalizeEPNS_5GraphEPPNS_4NodeEb", &tf_slot_4},
    {"_ZN10tensorflow11NodeBuilderC1ESt17basic_string_viewIcSt11char_traitsIcEES4_PKNS_"
     "19OpRegistryInterfaceEPKNS_13NodeDebugInfoE",
     &tf_slot_5},
    {"_ZN10tensorflow14NodeDefBuilder4AttrESt17basic_string_viewIcSt11char_traitsIcEENS_8DataTypeE",
     &tf_slot_6},
    {"_ZN10tensorflow14SessionOptionsC1Ev", &tf_slot_7},
    {"_ZN10tensorflow14TensorShapeRep12SlowCopyFromERKS0_", &tf_slot_8},
    {"_ZN10tensorflow14TensorShapeRep19DestructorOutOfLineEv", &tf_slot_9},
    {"_ZN10tensorflow15TensorShapeBaseINS_11TensorShapeEEC2EN4absl12lts_202501274SpanIKlEE",
     &tf_slot_10},
    {"_ZN10tensorflow5GraphC1EPKNS_19OpRegistryInterfaceE", &tf_slot_11},
    {"_ZN10tensorflow5GraphD1Ev", &tf_slot_12},
    {"_ZN10tensorflow6TensorC1ENS_8DataTypeERKNS_11TensorShapeE", &tf_slot_13},
    {"_ZN10tensorflow6TensorC1Ev", &tf_slot_14},
    {"_ZN10tensorflow6TensorD1Ev", &tf_slot_15},
    {"_ZN10tensorflow7NodeDefD1Ev", &tf_slot_16},
    {"_ZN10tensorflow8GraphDefC2EPN6google8protobuf5ArenaE", &tf_slot_17},
    {"_ZN10tensorflow8GraphDefD1Ev", &tf_slot_18},
    {"_ZN3tsl8internal15LogMessageFatalC1EPKci", &tf_slot_19},
    {"_ZN3tsl8internal15LogMessageFatalD1Ev", &tf_slot_20},
    {"_ZN3tsl8internal21CheckOpMessageBuilder7ForVar2Ev", &tf_slot_21},
    {"_ZN3tsl8internal21CheckOpMessageBuilder9NewStringB5cxx11Ev", &tf_slot_22},
    {"_ZN3tsl8internal21CheckOpMessageBuilderC1EPKc", &tf_slot_23},
    {"_ZN3tsl8internal21CheckOpMessageBuilderD1Ev", &tf_slot_24},
    {"_ZNK10tensorflow15TensorShapeBaseINS_11TensorShapeEE8dim_sizeEi", &tf_slot_25},
    {"_ZNK10tensorflow4Node4nameB5cxx11Ev", &tf_slot_26},
    {"_ZNK10tensorflow5Graph10ToGraphDefEPNS_8GraphDefEbb", &tf_slot_27},
    {"_ZNK10tensorflow6Tensor21CheckTypeAndIsAlignedENS_8DataTypeE", &tf_slot_28},
    {"_ZNK4absl12lts_2025012715status_internal9StatusRep5UnrefEv", &tf_slot_29},
};
}  // namespace

extern "C" bool tf_bind_symbols(void* handle) {
  bool ok = true;
  for (const auto& s : kSymbols) {
    *s.slot = dlsym(handle, s.name);
    if (*s.slot == nullptr) {
      fprintf(stderr, "TensorFlow does not export %s\n", s.name);
      ok = false;
    }
  }
  return ok;
}

#endif  // _WIN32
