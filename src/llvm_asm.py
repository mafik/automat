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
# To facilitate tracking, the script groups unsupported instructions into categories. Each instruction
# must be assigned to a category, otherwise a warning will be shown. Similarly, each category must be
# marked as supported or not (by inclusion in a "Super Category"). This structure means that once a new
# aspect of x86 is added to the design, all of the newly possible instructions can be enabled.

import build
import make
import src
import json
from functools import partial

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
    'JMP_2', # 16-bit jumps don't work in 64-bit mode
    'JMP64r_REX', 'JMP64r_NT', 'JMP64r', # memory is not supported yet
    ])

  # Step 1: Filter out opcodes that we don't support
  for opcode_name, opcode in x.items():
    skip = opcode_name.startswith('!') or 'Instruction' not in opcode['!superclasses']
    skip = skip or opcode['isPseudo'] or opcode['mayStore'] or opcode['mayLoad'] or opcode['isCall']
    if not skip:
      for predicate in opcode['Predicates']:
        if predicate['kind'] == 'def' and predicate['def'] in ('In32BitMode', 'Not64BitMode', 'HasX87', 'HasMMX', 'HasLWP', 'HasAMXCOMPLEX', 'HasAMXTILE', 'HasAMXINT8', 'HasAMXFP16'):
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
    del x[opcode_name]

  for opcode in x.values():
    for field in IRRELEVANT_FIELDS: 
      if field in opcode:
        del opcode[field]

  def select_opcodes_by_prefix(category, opcode_name_prefix):
    selected = {opcode_name: opcode for opcode_name, opcode in x.items() if opcode_name.startswith(opcode_name_prefix)}
    category.update(selected)
  
  categories = {}

  class Category():
    def __init__(self, name, prefixes, ring0=False):
      self.name = name
      if name in categories:
        raise ValueError(f'Category {name} already exists')
      categories[name] = self
      self.prefixes = prefixes
      self.ring0 = ring0
      self.opcodes = {}
      for prefix in prefixes:
        select_opcodes_by_prefix(self.opcodes, prefix)

  Category('LLVM sanitizers', ['UBSAN', 'ASAN'])
  Category('LLVM internals', ['ADJCALLSTACK', 'CMOV_', 'CATCHRET', 'CLEANUPRET', 'DYN_ALLOCA_64', 'EH_', 'Int_', 'KCFI_CHECK', 'PROBED_ALLOCA', 'SEG_ALLOCA', 'PTDPBF16PS', 'REP_'])
  Category('Synchronization prefixes', ['LOCK_PREFIX', 'XRELEASE_PREFIX', 'XACQUIRE_PREFIX'])
  Category('Rep prefixes', ['REPNE_PREFIX', 'REP_PREFIX'])
  Category('String scan', ['SCAS'])
  Category('String store', ['STOS'])
  Category('System Paging', ['WBINVD', 'INVLPG', 'TLBSYNC', 'WBNOINVD'], ring0=True)
  Category('Restricted Transactional Memory', ['XABORT', 'XBEGIN', 'XEND', 'XTEST', 'XSUSLDTRK', 'XRESLDTRK'])
  Category('Push/Pop FS/GS', ['POPFS', 'POPGS', 'PUSHFS', 'PUSHGS'])
  Category('Control Flow Tracking/Indirect Branch Tracking', ['ENDBR'])
  Category('Control Flow Shadow Stack', ['INCSSP', 'RDSSP', 'SAVEPREVSSP', 'RSTORSSP', 'WRSS', 'WRUSS', 'SETSSBSY', 'CLRSSBSY'])
  Category('Port Input', ['IN8', 'IN16', 'IN32', 'IN64', 'INSB', 'INSW', 'INSL'])
  Category('Port Output', ['OUT'])
  Category('Exchange/Add', ['XADD'])
  Category('Exchange', ['XCHG'])
  Category('Not', ['NOT'])
  Category('And', ['AND'])
  Category('Or', ['OR'])
  Category('Xor', ['XOR'])
  Category('Increment', ['INC'])
  Category('Decrement', ['DEC'])
  Category('Add', ['ADD', 'ADC', 'ADOX'])
  Category('Subtract', ['SUB', 'SBB'])
  Category('Signed Multiply', ['IMUL'])
  Category('Unsigned Multiply', ['MUL'])
  Category('Unsigned Divide', ['DIV'])
  Category('Signed Divide', ['IDIV'])
  Category('Signed Shift', ['SAL', 'SAR'])
  Category('Unsigned Double Shift', ['SHLD', 'SHRD'])
  Category('Unsigned Shift', ['SHL', 'SHR'])
  Category('Signed Negate', ['NEG'])
  Category('Sign Extend', ['CBW', 'CDQ', 'CQO', 'CWD', 'MOVSX'])
  Category('Zero Extend', ['MOVZX'])
  Category('Move', ['MOV'])
  Category('Move If', ['CMOV'])
  Category('Jump', ['JMP'])
  Category('Jump If', ['JCC', 'JECXZ', 'JRCXZ', 'LOOP'])
  Category('CRC32', ['CRC32'])
  Category('Flag Complement', ['CMC'])
  Category('Auxiliary Carry', ['CLAC', 'STAC'])
  Category('Flag Clear', ['CLC', 'CLD', 'CLI'])
  Category('Flag Set', ['STC', 'STD', 'STI'])
  Category('Flag Register', ['LAHF', 'SAHF'])
  Category('Breakpoint', ['INT3'])
  Category('Software Interrupt', ['INT'])
  Category('User Interrupt', ['UIRED', 'CLUI', 'STUI', 'TESTUI', 'SENDUIPI', 'UIRET'])
  Category('Bit Rotations Carry', ['RCL', 'RCR'])
  Category('Bit Rotations', ['ROL', 'ROR'])
  Category('Bit Tests', ['BT'])
  Category('Bit Counting', ['LZCNT', 'POPCNT', 'TZCNT'])
  Category('Bit Twiddling', ['BEXTR', 'BLC', 'BLS', 'BSF', 'BSR', 'BSWAP', 'BZHI', 'PDEP', 'PEXT', 'T1MSKC', 'TZMSK'])
  Category('AMD-V', ['VMRUN', 'VMLOAD', 'VMSAVE', 'CLGI', 'VMMCALL', 'INVLPGA', 'SKINIT', 'STGI'])
  Category('Intel VMX', ['VMPTRLD', 'VMPTRST', 'VMCLEAR', 'VMREAD', 'VMWRITE', 'VMCALL', 'VMLAUNCH', 'VMRESUME', 'VMXOFF', 'VMXON', 'INVEPT', 'INVVPID', 'VMFUNC', 'SEAMCALL', 'SEAMOPS', 'SEAMRET', 'TDCALL'])
  Category('Intel FRED', ['ERET', 'LKGS'], ring0=True)
  Category('SGX', ['ENCLS', 'ENCLU', 'ENCLV'])
  Category('CPUID', ['CPUID'])
  Category('System Task Switching', ['CLTS', 'LTR'], ring0=True)
  Category('System Segment Descriptors', ['LLDT', 'LMSW'], ring0=True)
  Category('Read/Write FS/GS', ['RDFSBASE', 'RDGSBASE', 'WRFSBASE', 'WRGSBASE'])
  Category('Swap GS', ['SWAPGS'], ring0=True)
  Category('System Call', ['SYSCALL', 'SYSRET', 'SYSENTER', 'SYSEXIT'])
  Category('Cache Control', ['CLZERO'])
  Category('Processor History', ['HRESET', 'INVD', 'INVLPGB64'], ring0=True)
  Category('Compare and Exchange', ['CMPXCHG'])
  Category('Compare', ['CMP'])
  Category('Test', ['TEST'])
  Category('Set If', ['SETCC'])
  Category('Fence', ['LFENCE', 'SFENCE', 'MFENCE', 'SERIALIZE'])
  Category('Stack Frames', ['ENTER', 'LEAVE'])
  Category('x87', ['FEMMS'])
  Category('Intel Trusted Execution', ['GETSEC'])
  Category('Power', ['HLT'], ring0=True)
  Category('Segment Descriptors', ['LAR', 'LSL', 'VERR', 'VERW', 'SLDT', 'STR', 'SMSW'])
  Category('Performance Counters', ['RDPMC'])
  Category('Get Extended Control Registers', ['XGETBV'])
  Category('Set Extended Control Registers', ['XSETBV'], ring0=True)
  Category('User Page Keys', ['RDPKRU', 'WRPKRU'])
  Category('Core ID', ['RDPID'])
  Category('System Model Specific Registers', ['RDMSR', 'WRMSR'], ring0=True)
  Category('Model Specific Registers', ['RDPRU'])
  Category('Random Numbers', ['RDRAND', 'RDSEED'])
  Category('Time', ['RDTSC'])
  Category('Memory Monitoring', ['MONITORX', 'MWAITX', 'UMONITOR', 'UMWAIT', 'TPAUSE'])
  Category('System Memory Monitoring', ['MONITOR', 'MWAIT'], ring0=True)
  Category('VIA PadLock', ['MONTMUL', 'XSTORE'])
  Category('System Total Storage Encryption', ['PBNDKB', 'PCONFIG'], ring0=True)
  Category('Software Tracing', ['PTWRITE'])
  Category('System Management Mode', ['RSM'], ring0=True)
  Category('AMD Secure Nested Paging', ['PSMASH', 'PVALIDATE', 'RMPADJUST', 'RMPQUERY', 'RMPUPDATE'], ring0=True)

  # At this point, all instructions should have been categorized.
  categorized_opcodes = set()
  for category in categories.values():
    categorized_opcodes.update(category.opcodes.keys())
  
  uncategorized_opcodes = set(x.keys()).difference(categorized_opcodes)
  for opcode_name in uncategorized_opcodes:
    # Useful reference: https://en.wikipedia.org/wiki/X86_instruction_listings
    print(f'Warning: {opcode_name} is not covered by any category. Check build/<BUILD_TYPE>/x86.json for details. Update src/llvm_asm.py to fix it.')

  super_categories = {}
  class SuperCategory():
    def __init__(self, name, categories, supported=True):
      self.name = name
      super_categories[name] = self
      self.categories = categories
      self.supported = supported
  SuperCategory('System', [name for name in categories.keys() if categories[name].ring0], supported=False)
  SuperCategory('Virtualization', ['AMD-V', 'Intel VMX'], supported=False)
  SuperCategory('Floating Point', ['x87'], supported=False)
  SuperCategory('LLVM Stuff', ['LLVM sanitizers', 'LLVM internals'], supported=False)
  SuperCategory('Specialist Stuff', [
    'Push/Pop FS/GS', 'Get Extended Control Registers', 'Intel Trusted Execution', 'Unsigned Double Shift', 'CPUID',
    'Software Tracing', 'Model Specific Registers', 'Cache Control', 'Control Flow Tracking/Indirect Branch Tracking',
    'Performance Counters', 'Software Interrupt', 'User Interrupt', 'Segment Descriptors', 'SGX', 'System Call',
    'Breakpoint', 'Memory Monitoring', 'User Page Keys', 'Read/Write FS/GS', 'Control Flow Shadow Stack'], supported=False)
  SuperCategory('Obsolete', ['VIA PadLock', 'Auxiliary Carry'], supported=False)
  SuperCategory('Accesses Memory', ['String scan', 'String store', 'Stack Frames'], supported=False)
  SuperCategory('Prefixes', ['Rep prefixes', 'Synchronization prefixes'], supported=False)
  SuperCategory('Multithreading', ['Fence', 'Restricted Transactional Memory'], supported=False)
  SuperCategory('Ports', ['Port Input', 'Port Output'], supported=False)
  SuperCategory('Synchronization for experts', ['Exchange/Add', 'Compare and Exchange'], supported=False)

  SuperCategory('Logic', ['Not', 'And', 'Or', 'Xor'])
  SuperCategory('Universal Math', ['Increment', 'Decrement', 'Add', 'Subtract'])
  SuperCategory('Bits', ['Bit Rotations Carry', 'Bit Rotations', 'Bit Tests', 'Bit Counting', 'Bit Twiddling'])
  SuperCategory('Move', ['Move', 'Move If', 'Exchange', 'Sign Extend', 'Zero Extend'])
  SuperCategory('Signed Math', ['Signed Negate', 'Signed Shift', 'Signed Divide', 'Signed Multiply', 'Sign Extend'])
  SuperCategory('Unsigned Math', ['Unsigned Shift', 'Unsigned Divide', 'Unsigned Multiply', 'Zero Extend'])
  SuperCategory('Misc', ['Random Numbers', 'Time', 'CRC32', 'Core ID'])
  SuperCategory('Flags', ['Flag Complement', 'Flag Clear', 'Flag Set', 'Flag Register'])
  SuperCategory('Control', ['Compare', 'Test', 'Jump', 'Jump If', 'Set If', 'Move If'])

  # At this point all categories should have been grouped. Print the stragglers.
  covered_categories = set()
  for super_category in super_categories.values():
    covered_categories.update(super_category.categories)

  non_covered_categories = set(categories.keys()).difference(covered_categories)
  for category in non_covered_categories:
    print(f'Warning: {category} is not covered by any super category. Check src/llvm_asm.py to fix it.')

  # Print supported super-categories with instruction counts
  for super_category in super_categories.values():
    if not super_category.supported:
      continue
    print(f'# {super_category.name}')
    for category_name in super_category.categories:
      category = categories[category_name]
      print(f'- {category_name:25s} ({len(category.opcodes)} instructions)')
    print()

  f = x86_hh.open('w')
  # json.dump(x, f, indent=2)
  f.write('// This file was automatically generated by llvm_asm.py\n')


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
