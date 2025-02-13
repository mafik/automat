# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

# The job of this extension is to generate `include/automat/x86.hh` with all the LLVM x86 instructions
# that Automat supports.
#
# There are many classes of instructions which are not supported. In all cases this is due to lack of
# time & proper design. Nonetheless it's important to keep track of why a particular group of
# instructions is not supported. Accurate tracking will enable progressive expansion of the covered
# instruction set, as the design is extended.
#
# To facilitate tracking, the script groups unsupported instructions. Each instruction
# must be assigned to a group, otherwise a warning will be shown. Similarly, each group must be
# marked as supported or not (by inclusion in a Category). This structure means that once a new
# aspect of x86 is added to the design, all of the newly possible instructions can be enabled.

import build
import make
import src
import json
import re
from functools import partial
from pathlib import Path

llvm = src.load_extension('llvm')

def llvm_tblgen_path(build_type : build.BuildType):
  return (build_type.PREFIX() / 'bin' / 'llvm-tblgen').with_suffix(build.binary_extension)

UNSUPPORTED_REGISTER_CLASSES = set([
  'FR16X', 'FR32X', 'FR64X',
  'i8mem', 'i16mem', 'i32mem', 'i64mem', 'i128mem',
  'i512mem_GR32', 'i512mem_GR64',
  'f64mem', 'lea64_32mem', 'lea64mem', 
  'RSTi',
  'RFP32', 'RFP64', 'RFP80',
  'FR16', 'FR32', 'FR64',
  'VR64', 'VR128', 'VR256', 'VR512',
  'VR128X', 'VR256X',
  'VK1', 'VK2', 'VK4', 'VK8', 'VK16', 'VK32', 'VK64',
  'SEGMENT_REG','CONTROL_REG', 'TILE',
  'ssmem', 'opaquemem', 'anymem', 'sibmem'
])

# Fields that are not necessary and decrease the readability
IRRELEVANT_FIELDS = ['!anonymous', '!fields', '!name', 'AddedComplexity', 'ExeDomain', 'AdSize',
                     'AdSizeBits', 'CD8_EltSize', 'CD8_Form', 'CD8_Scale', 'FPForm', 'Form',
                     'FormBits', 'Itinerary', 'OpEncBits', 'OpMap', 'OpMapBits', 'OpPrefix',
                     'OpPrefixBits', 'OpSizeBits', 'Opcode', 'TSFlags', 'VectSize',
                     'explicitOpPrefix', 'explicitOpPrefixBits', 'AsmMatchConverter']

def gen_x86_hh(x86_json, x86_hh):
  x86_hh.parent.mkdir(parents=True, exist_ok=True)

  x = json.load(x86_json.open())

  skip_opcodes = set([
    'JMP_2', 'JCC_2', # 16-bit jumps don't work in 64-bit mode
    'JMP64r_REX', 'JMP64r_NT', 'JMP64r', # memory is not supported yet
    ])

  # Step 1: Filter out opcodes that we don't support
  for opcode_name, opcode in x.items():
    skip = opcode_name.startswith('!') or 'Instruction' not in opcode['!superclasses']
    skip = skip or opcode['isPseudo'] or opcode['mayStore'] or opcode['mayLoad'] or opcode['isCall']
    if not skip:
      for predicate in opcode['Predicates']:
        if predicate['kind'] == 'def' and predicate['def'] in ('In32BitMode', 'Not64BitMode', 'HasX87', 'HasMMX', 'HasLWP', 'HasAMXCOMPLEX', 'HasAMXTILE', 'HasAMXINT8', 'HasAMXFP16', 'HasTBM'):
          skip = True
    if not skip:
      for arg, arg_name in opcode['InOperandList']['args'] + opcode['OutOperandList']['args']:
        if arg['kind'] == 'def' and arg['def'] in UNSUPPORTED_REGISTER_CLASSES:
          skip = True
    if not skip:
      if opcode['OpEnc']['def'] == 'EncEVEX':
        skip = True
    if not skip:
      # Skip instructions that implicitly use ESI because they usually access memory
      # Skip instructions that use MXCSR & FPCW because they're floating-point
      # Skip instructions that use XMM0 because we don't have it designed yet$
      for use in opcode['Uses']:
        if use['def'] in ('ESI', 'MXCSR', 'FPCW', 'XMM0', 'ESP'):
          skip = True
    if not skip:
      # Skip instructions that define FPSW because they're floating-point
      for def_ in opcode['Defs']:
        if def_['def'] in ('FPSW', 'YMM0', 'RSI', 'ESP'):
          skip = True
    if not skip:
      # Skip instruction prefixes and nops
      # Skip Rets
      if opcode['SchedRW'] and opcode['SchedRW'][0]['def'] in ('WriteNop', 'WriteLoad', 'WriteJumpLd'):
        skip = True
    if skip:
      skip_opcodes.add(opcode_name)

  for opcode_name in skip_opcodes:
    if opcode_name in x:
      del x[opcode_name]

  for opcode in x.values():
    for field in IRRELEVANT_FIELDS: 
      if field in opcode:
        del opcode[field]

  # Dump formatted x86.json for manual inspection
  if False:
    json.dump(x, x86_json.open('w'), indent=2)

  import re

  def select_opcodes_by_regex(group, opcode_name_regexp):
    selected = {opcode_name: opcode for opcode_name, opcode in x.items() if re.match(opcode_name_regexp, opcode_name)}
    group.update(selected)
  
  groups = {}

  class Group:
    def __init__(self, name, regexps, ring0=False, shortcut=None):
      if not shortcut:
        shortcut = name
      self.name = name
      self.shortcut = shortcut
      if name in groups:
        raise ValueError(f'Group {name} already exists')
      groups[name] = self
      self.prefixes = regexps
      self.ring0 = ring0
      self.opcodes = {}
      for regexp in regexps:
        select_opcodes_by_regex(self.opcodes, regexp)

  Group('LLVM sanitizers', ['^UBSAN', '^ASAN'])
  Group('LLVM internals', ['^ADJCALLSTACK', '^CMOV_', '^CATCHRET', '^CLEANUPRET', '^DYN_ALLOCA_64', '^EH_', '^Int_', '^KCFI_CHECK', '^PROBED_ALLOCA', '^SEG_ALLOCA', '^PTDPBF16PS', '^REP_', '^CMOV_'])
  Group('Synchronization prefixes', ['^LOCK_PREFIX', '^XRELEASE_PREFIX', '^XACQUIRE_PREFIX'])
  Group('Rep prefixes', ['^REPNE_PREFIX', '^REP_PREFIX'])
  Group('String scan', ['^SCAS'])
  Group('String store', ['^STOS'])
  Group('System Paging', ['^WBINVD', '^INVLPG', '^TLBSYNC', '^WBNOINVD'], ring0=True)
  Group('Restricted Transactional Memory', ['^XABORT', '^XBEGIN', '^XEND', '^XTEST', '^XSUSLDTRK', '^XRESLDTRK'])
  Group('Push/Pop FS/GS', ['^POPFS', '^POPGS', '^PUSHFS', '^PUSHGS'])
  Group('Control Flow Tracking/Indirect Branch Tracking', ['^ENDBR'])
  Group('Control Flow Shadow Stack', ['^INCSSP', '^RDSSP', '^SAVEPREVSSP', '^RSTORSSP', '^WRSS', '^WRUSS', '^SETSSBSY', '^CLRSSBSY'])
  Group('Port Input', ['^IN8', '^IN16', '^IN32', '^IN64', '^INSB', '^INSW', '^INSL'])
  Group('Port Output', ['^OUT'])
  Group('Exchange/Add', ['^XADD'])
  Group('Exchange', ['^XCHG'], shortcut='Swap')
  Group('Not', ['^NOT'])
  Group('And', ['^AND'])
  Group('Or', ['^OR'])
  Group('Xor', ['^XOR'])
  Group('Increment', [r'^INC\d'], shortcut='+1')
  Group('Decrement', ['^DEC'], shortcut='-1')
  Group('Multi-line unsigned add', ['^ADOX', '^ADCX'])
  Group('Shift without affecting flags', ['^SHLX', '^SHRX', '^SARX', '^RORX'])
  Group('Add Carry', [r'^ADC\d'], shortcut='+ chain')
  Group('Add', ['^ADD'], shortcut='+')
  Group('Subtract Carry', ['^SBB'], shortcut='- chain')
  Group('Subtract', ['^SUB'], shortcut='-')
  Group('Signed Multiply', ['^IMUL'], shortcut='×')
  Group('Unsigned Multiply', [r'^MUL\d'], shortcut='×')
  Group('Multiply without affecting flags', [r'^MULX\d'])
  Group('Unsigned Divide', ['^DIV'], shortcut='÷')
  Group('Signed Divide', ['^IDIV', '^CBW', '^CWD', '^CDQ', '^CQO'], shortcut='÷')
  Group('Signed Shift', ['^SAL', r'^SAR\d'], shortcut='Shift')
  Group('Unsigned Double Shift', ['^SHLD', '^SHRD'])
  Group('Unsigned Shift', [r'^SHL\d', r'^SHR\d'], shortcut='Shift')
  Group('Signed Negate', ['^NEG'], shortcut='±')
  Group('Sign Extend', ['^MOVSX'], shortcut='Extend')
  Group('Zero Extend', ['^MOVZX'], shortcut='Extend')
  Group('Move', [r'^MOV\d+(rr|ri)'])
  Group('Move Debug', [r'^MOV\d+(rd|dr)'])
  Group('Move If', [r'^CMOV\d', '^SETCC'])
  Group('Jump', ['^JMP', '^JCC', '^JECXZ', '^JRCXZ', '^LOOP'])
  Group('CRC32', ['^CRC32'])
  Group('Carry Flag', ['^CMC', '^CLC', '^STC'], shortcut='Carry')
  Group('Auxiliary Carry', ['^CLAC', '^STAC'])
  Group('Direction Flag', ['^STD', '^CLD'])
  Group('Flag Register', ['^LAHF', '^SAHF'], shortcut='Move')
  Group('Breakpoint', ['^INT3'])
  Group('Software Interrupt', ['^INT'])
  Group('User Interrupt', ['^UIRED', '^CLUI', '^STUI', '^TESTUI', '^SENDUIPI', '^UIRET', '^TESTUI', '^CLI', '^STI'])
  Group('Bit Rotations Carry', ['^RCL', '^RCR'], shortcut='Rotate Carry')
  Group('Bit Rotations', ['^ROL', r'^ROR\d'], shortcut='Rotate')
  Group('Bit Complement', ['^BTC'], shortcut='Flip')
  Group('Bit Tests', [r'^BT\d'], shortcut='Test')
  Group('Bit Set', [r'^BTS\d'], shortcut='Raise')
  Group('Bit Reset', [r'^BTR\d'], shortcut='Lower')
  Group('Bit Counting', ['^LZCNT', '^POPCNT', '^TZCNT'], shortcut='Count')
  Group('Invalid Byte Swap', ['^BSWAP16'])
  Group('Bit Twiddling', ['^BEXTR', '^BLC', '^BLS', '^BSF', '^BSR', '^BSWAP(32|64)', '^BZHI', '^PDEP', '^PEXT', '^T1MSKC', '^TZMSK'], shortcut='Twiddle')
  Group('AMD-V', ['^VMRUN', '^VMLOAD', '^VMSAVE', '^CLGI', '^VMMCALL', '^INVLPGA', '^SKINIT', '^STGI'])
  Group('Intel VMX', ['^VMPTRLD', '^VMPTRST', '^VMCLEAR', '^VMREAD', '^VMWRITE', '^VMCALL', '^VMLAUNCH', '^VMRESUME', '^VMXOFF', '^VMXON', '^INVEPT', '^INVVPID', '^VMFUNC', '^SEAMCALL', '^SEAMOPS', '^SEAMRET', '^TDCALL'])
  Group('Intel FRED', ['^ERET', '^LKGS'], ring0=True)
  Group('SGX', ['^ENCLS', '^ENCLU', '^ENCLV'])
  Group('CPUID', ['^CPUID'])
  Group('System Task Switching', ['^CLTS', '^LTR'], ring0=True)
  Group('System Segment Descriptors', ['^LLDT', '^LMSW'], ring0=True)
  Group('Read/Write FS/GS', ['^RDFSBASE', '^RDGSBASE', '^WRFSBASE', '^WRGSBASE'])
  Group('Swap GS', ['^SWAPGS'], ring0=True)
  Group('System Call', ['^SYSCALL', '^SYSRET', '^SYSENTER', '^SYSEXIT'])
  Group('Cache Control', ['^CLZERO'])
  Group('Processor History', ['^HRESET', '^INVD', '^INVLPGB64'], ring0=True)
  Group('Compare and Exchange', ['^CMPXCHG'])
  Group('Compare', [r'^CMP\d'])
  Group('Test', [r'^TEST\d'])
  Group('Fence', ['^LFENCE', '^SFENCE', '^MFENCE', '^SERIALIZE'])
  Group('Stack Frames', ['^ENTER', '^LEAVE'])
  Group('x87', ['^FEMMS'])
  Group('Intel Trusted Execution', ['^GETSEC'])
  Group('Power', ['^HLT'], ring0=True)
  Group('Segment Descriptors', ['^LAR', '^LSL', '^VERR', '^VERW', '^SLDT', '^STR', '^SMSW'])
  Group('Performance Counters', ['^RDPMC'])
  Group('Get Extended Control Registers', ['^XGETBV'])
  Group('Set Extended Control Registers', ['^XSETBV'], ring0=True)
  Group('User Page Keys', ['^RDPKRU', '^WRPKRU'])
  Group('Core ID', ['^RDPID'])
  Group('System Model Specific Registers', ['^RDMSR', '^WRMSR'], ring0=True)
  Group('Model Specific Registers', ['^RDPRU'])
  Group('Random Numbers', ['^RDRAND', '^RDSEED'], shortcut='Random')
  Group('Time', ['^RDTSC$'])
  Group('Time + Core ID', ['^RDTSCP$'])
  Group('Memory Monitoring', ['^MONITORX', '^MWAITX', '^UMONITOR', '^UMWAIT', '^TPAUSE'])
  Group('System Memory Monitoring', ['^MONITOR', '^MWAIT'], ring0=True)
  Group('VIA PadLock', ['^MONTMUL', '^XSTORE'])
  Group('System Total Storage Encryption', ['^PBNDKB', '^PCONFIG'], ring0=True)
  Group('Software Tracing', ['^PTWRITE'])
  Group('System Management Mode', ['^RSM'], ring0=True)
  Group('AMD Secure Nested Paging', ['^PSMASH', '^PVALIDATE', '^RMPADJUST', '^RMPQUERY', '^RMPUPDATE'], ring0=True)

  # At this point, all instructions should have been grouped.
  grouped_opcodes = set()
  for group_obj in groups.values():
    grouped_opcodes.update(group_obj.opcodes.keys())
  
  ungrouped_opcodes = set(x.keys()).difference(grouped_opcodes)
  for opcode_name in ungrouped_opcodes:
    # Useful reference: https://en.wikipedia.org/wiki/X86_instruction_listings
    print(f'Warning: {opcode_name} is not covered by any group. Check build/<BUILD_TYPE>/x86.json for details. Update src/llvm_asm.py to fix it.')

  categories = {}
  class Category:
    def __init__(self, name, groups_list, supported=True):
      self.name = name
      categories[name] = self
      self.groups = groups_list
      self.supported = supported

  Category('System', [name for name in groups.keys() if groups[name].ring0], supported=False)
  Category('Virtualization', ['AMD-V', 'Intel VMX'], supported=False)
  Category('Floating Point', ['x87'], supported=False)
  Category('LLVM Stuff', ['LLVM sanitizers', 'LLVM internals'], supported=False)
  Category('Specialist Stuff', [
    'Push/Pop FS/GS', 'Get Extended Control Registers', 'Intel Trusted Execution', 'Unsigned Double Shift', 'CPUID',
    'Software Tracing', 'Model Specific Registers', 'Cache Control', 'Control Flow Tracking/Indirect Branch Tracking',
    'Performance Counters', 'Software Interrupt', 'User Interrupt', 'Segment Descriptors', 'SGX', 'System Call',
    'Breakpoint', 'Memory Monitoring', 'User Page Keys', 'Read/Write FS/GS', 'Control Flow Shadow Stack',
    'Multi-line unsigned add', 'Move Debug', 'Flag Register', 'CRC32', 'Core ID', 'Time + Core ID',
    'Shift without affecting flags', 'Multiply without affecting flags'], supported=False)
  Category('Obsolete', ['VIA PadLock', 'Auxiliary Carry', 'Invalid Byte Swap'], supported=False)
  Category('Accesses Memory', ['String scan', 'String store', 'Stack Frames', 'Direction Flag'], supported=False)
  Category('Prefixes', ['Rep prefixes', 'Synchronization prefixes'], supported=False)
  Category('Multithreading', ['Fence', 'Restricted Transactional Memory'], supported=False)
  Category('Ports', ['Port Input', 'Port Output'], supported=False)
  Category('Synchronization for experts', ['Exchange/Add', 'Compare and Exchange'], supported=False)

  Category('Logic', ['Not', 'And', 'Or', 'Xor'])
  Category('Universal Math', ['Increment', 'Decrement', 'Add', 'Subtract'])
  Category('Bits', ['Bit Rotations Carry', 'Bit Rotations', 'Bit Tests', 'Bit Counting', 'Bit Twiddling', 'Bit Complement', 'Bit Set', 'Bit Reset'])
  Category('Move', ['Move', 'Move If', 'Exchange'])
  Category('Signed Math', ['Signed Negate', 'Signed Shift', 'Signed Divide', 'Signed Multiply', 'Sign Extend'])
  Category('Unsigned Math', ['Unsigned Shift', 'Unsigned Divide', 'Unsigned Multiply', 'Zero Extend', 'Add Carry', 'Subtract Carry'])
  Category('Misc', ['Random Numbers', 'Time'])
  Category('Control', ['Compare', 'Test', 'Jump', 'Move If', 'Carry Flag'])

  # At this point all categories should have been grouped. Print the stragglers.
  covered_groups = set()
  for category in categories.values():
    covered_groups.update(category.groups)

  non_covered_groups = set(groups.keys()).difference(covered_groups)
  for group in non_covered_groups:
    print(f'Warning: {group} is not covered by any category. Check src/llvm_asm.py to fix it.')

  # Print supported categories with instruction counts
  if False:
    for category in categories.values():
      if not category.supported:
        continue
      print(f'# {category.name}')
      for group_name in category.groups:
        group = groups[group_name]
        print(f'- {group_name:25s} ({len(group.opcodes)} instructions)')
      print()

  # --- Generate x86.hh below ---
  f = x86_hh.open('w')
  # First, emit the header and struct definitions.
  print(f'''#pragma once

// This file was automatically generated by {Path(__file__).name}

#include <span>
#include <string_view>

#include <llvm/lib/Target/X86/X86Subtarget.h>

namespace automat::x86 {{

using namespace llvm::X86;

struct Group {{
  std::string_view name;
  std::string_view shortcut;
  std::span<const unsigned> opcodes;
}};

struct Category {{
  std::string_view name;
  std::span<const Group> groups;
}};
''', file=f)

  # A helper to sanitize names for identifiers (remove whitespace and punctuation)
  import re
  def sanitize(name):
    return re.sub(r'[^a-zA-Z0-9]', '', name)

  output_lines = []

  # Only include groups referenced by supported categories.
  supported_group_names = []
  for cat in categories.values():
    if not cat.supported:
      continue
    for g in cat.groups:
      if g not in supported_group_names:
        supported_group_names.append(g)

  # For each group, generate an opcodes array.
  for group_name in supported_group_names:
    group_obj = groups[group_name]
    sanitized_group = sanitize(group_obj.name)
    opcodes = sorted(group_obj.opcodes.keys())
    opcode_list = ', '.join(f'{opcode}' for opcode in opcodes)
    output_lines.append(f'constexpr unsigned k{sanitized_group}Opcodes[] = {{{opcode_list}}};')
  output_lines.append('')

  # For each supported category, generate a groups array.
  supported_categories = [cat for cat in categories.values() if cat.supported]
  for cat in supported_categories:
    sanitized_cat = sanitize(cat.name)
    output_lines.append(f'constexpr Group k{sanitized_cat}Groups[] = {{')
    for group_name in cat.groups:
      if group_name not in groups:
        continue
      group_obj = groups[group_name]
      sanitized_group = sanitize(group_obj.name)
      output_lines.append(f'    {{"{group_obj.name}", "{group_obj.shortcut}", k{sanitized_group}Opcodes}},')
    output_lines.append('};')
    output_lines.append('')

  # Finally, generate the master categories array.
  output_lines.append('constexpr Category kCategories[] = {')
  for cat in supported_categories:
    sanitized_cat = sanitize(cat.name)
    output_lines.append(f'    {{.name = "{cat.name}", .groups = k{sanitized_cat}Groups}},')
  output_lines.append('};')
  output_lines.append('')
  output_lines.append('}  // namespace automat::x86')
  output_lines.append('')

  print("\n".join(output_lines), file=f)

def hook_recipe(r: make.Recipe):
  for build_type in build.types:
    x86_td = llvm.hook.src_dir / 'lib' / 'Target' / 'X86' / 'X86.td'
    x86_json = build_type.BASE() / 'x86.json'
    tblgen = llvm_tblgen_path(build_type)
    args = [tblgen, '--dump-json', x86_td,
            '-I', build_type.PREFIX() / 'include',
            '-I', x86_td.parent,
            '-o', x86_json]
    r.add_step(partial(make.Popen, args),
               outputs=[x86_json],
               inputs=[llvm.llvm_config_path(build_type)],
               desc='Generating x86.json',
               shortcut='x86.json ' +  build_type.name)

    # x86.hh is generated to PREFIX rather than `build/generated` because it depends on llvm-tblgen,
    # which exists in three different PREFIX-es.
    x86_hh = build_type.PREFIX() / 'include' / 'automat' / 'x86.hh'

    r.add_step(partial(gen_x86_hh, x86_json, x86_hh),
               outputs=[x86_hh],
               inputs=[x86_json, __file__],
               desc='Generating x86.hh',
               shortcut='x86.hh ' +  build_type.name)
    
# Hook x86.hh into the build graph

files_using_x86_hh = set()

def hook_srcs(srcs: dict[str, src.File], r: make.Recipe):
  for file in srcs.values():
    if 'automat/x86.hh' in file.system_includes:
      files_using_x86_hh.add(file)

def hook_plan(srcs: dict[str, src.File], objs: list[build.ObjectFile], bins: list[build.Binary], r: make.Recipe):
  for obj in objs:
    if obj.source in files_using_x86_hh:
      x86_hh = obj.build_type.PREFIX() / 'include' / 'automat' / 'x86.hh'
      obj.deps.add(x86_hh)
