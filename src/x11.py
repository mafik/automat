# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Generates C++ structs and wire code for the X11 protocol from xcb-proto XML files
# vendored in src/x11/. Every request, reply and event becomes a plain C++ struct in
# build/generated/x11_generated.hpp; the decoding, encoding and dispatch code goes to
# build/generated/x11_generated.cpp. Requests carry a `Handle(Client&)` method which the
# server (src/x11.cpp) implements; adding an extension = dropping its XML here and
# implementing the new Handle bodies (missing ones are link errors).
#
# The wire codec is little-endian only; connections declaring big-endian are refused at
# the handshake. Unlike Wayland (whose interfaces mix generated declarations with
# hand-written per-object state, hence the validated blocks in wayland_protocol.hpp),
# X11 messages are pure data - server state lives on resource structs in
# x11_protocol.hpp - so everything generated stays in build/generated/.

import sys
import xml.etree.ElementTree as ET

import fs_utils
import make
import src

if sys.platform == 'linux':
  generated_cpp = fs_utils.generated_dir / 'x11_generated.cpp'
  generated_hpp = fs_utils.generated_dir / 'x11_generated.hpp'
  protocol_hpp = fs_utils.src_dir / 'x11_protocol.hpp'
  xml_dir = fs_utils.src_dir / 'x11'

  PRIMITIVES = {
    'CARD8': ('U8', 1), 'BYTE': ('U8', 1), 'BOOL': ('U8', 1), 'char': ('char', 1),
    'void': ('U8', 1), 'CARD16': ('U16', 2), 'CARD32': ('U32', 4), 'CARD64': ('U64', 8),
    'INT8': ('I8', 1), 'INT16': ('I16', 2), 'INT32': ('I32', 4), 'INT64': ('I64', 8),
    'float': ('float', 4), 'double': ('double', 8),
  }

  # XID types referenced by present.xml from protocol files we do not vendor.
  EXTERNAL_XIDS = ['CRTC', 'REGION', 'FENCE', 'BARRIER']

  CPP_KEYWORDS = {'class', 'new', 'delete', 'default', 'operator', 'private', 'public'}

  def field_name(name):
    return name + '_' if name in CPP_KEYWORDS else name

  class Type:
    def __init__(self, cpp, size, is_struct=False):
      self.cpp = cpp
      self.size = size  # None = variable
      self.is_struct = is_struct

  class Field:
    def __init__(self, name, type):
      self.name = field_name(name)
      self.type = type

  class Pad:
    def __init__(self, bytes):
      self.bytes = bytes

  class ListItem:
    def __init__(self, name, type, expr):
      self.name = field_name(name)
      self.type = type
      self.expr = expr  # parsed expr tree or None (open-ended)

    def const_len(self):
      return self.expr[1] if self.expr and self.expr[0] == 'value' else None

  class StrListItem:  # LISTofSTR: length-prefixed strings
    def __init__(self, name, expr):
      self.name = field_name(name)
      self.expr = expr

  class SwitchCase:
    def __init__(self, bit, field):
      self.bit = bit
      self.field = field

  class SwitchItem:
    def __init__(self, mask_field, cases):
      self.mask_field = mask_field
      self.cases = cases

  class FdItem:
    def __init__(self, name):
      self.name = field_name(name)

  class FdListItem:
    def __init__(self, name, expr):
      self.name = field_name(name)
      self.expr = expr

  class Message:
    def __init__(self, name, items):
      self.name = name
      self.items = items

  class Request(Message):
    def __init__(self, name, opcode, items, reply):
      super().__init__(name, items)
      self.opcode = opcode
      self.reply = reply  # Message or None

  class Event(Message):
    def __init__(self, name, number, items, xge, no_sequence):
      super().__init__(name, items)
      self.number = number
      self.xge = xge
      self.no_sequence = no_sequence

  class Module:
    def __init__(self, path):
      self.root = ET.parse(path).getroot()
      self.xml_name = path.name
      self.header = self.root.attrib['header']
      self.xname = self.root.attrib.get('extension-xname')  # None for xproto
      self.namespace = None if self.header == 'xproto' else self.header.replace('-', '_')
      self.enums = []      # (name, [(item_name, value)])
      self.typedefs = []   # (cpp_name, underlying_cpp)
      self.structs = []    # (name, items, size)
      self.unions = []     # (name, [(member_cpp, member_name, count)], size)
      self.requests = []
      self.events = []
      self.errors = []     # (name, number)
      self.major_opcode = 0
      self.first_event = 0
      self.first_error = 0

  types = {}

  def resolve(name):
    if ':' in name:  # e.g. "RANDR:CRTC" - namespaced ref to an unvendored file
      name = name.split(':')[1]
    if name not in types:
      raise SystemExit(f'x11.py: unknown type {name}')
    return types[name]

  def parse_expr(elem):
    if elem.tag == 'fieldref':
      return ('fieldref', elem.text.strip())
    if elem.tag == 'value':
      return ('value', int(elem.text.strip(), 0))
    if elem.tag == 'op':
      sub = [e for e in elem if e.tag in ('fieldref', 'value', 'op', 'popcount')]
      return ('op', elem.attrib['op'], parse_expr(sub[0]), parse_expr(sub[1]))
    if elem.tag == 'popcount':
      return ('popcount', parse_expr(next(iter(elem))))
    raise SystemExit(f'x11.py: unsupported expression <{elem.tag}>')

  def render_expr(expr, prefix):
    kind = expr[0]
    if kind == 'fieldref':
      return f'(size_t){prefix}{field_name(expr[1])}'
    if kind == 'value':
      return str(expr[1])
    if kind == 'op':
      return f'({render_expr(expr[2], prefix)} {expr[1]} {render_expr(expr[3], prefix)})'
    if kind == 'popcount':
      return f'(size_t)std::popcount((U32)({render_expr(expr[1], prefix)}))'

  def enum_bit_values(module_of_enum, enum_name):
    # Bit values of an enum, for switch bitcases; searched across all modules.
    for module in modules:
      for name, entries in module.enums:
        if name == enum_name:
          return dict(entries)
    raise SystemExit(f'x11.py: enum {enum_name} not found')

  def parse_items(parent, module):
    items = []
    for elem in parent:
      tag = elem.tag
      if tag in ('doc', 'reply', 'required_start_align'):
        continue  # required_start_align is an assertion in xcb, not padding
      if tag in ('field', 'exprfield'):  # an exprfield is an ordinary field on the wire
        items.append(Field(elem.attrib['name'], resolve(elem.attrib['type'])))
      elif tag == 'pad':
        if 'bytes' in elem.attrib:
          items.append(Pad(int(elem.attrib['bytes'])))
        # align pads never trigger in the vendored set (offsets are aligned by design)
      elif tag == 'list':
        exprs = [e for e in elem if e.tag in ('fieldref', 'value', 'op', 'popcount')]
        expr = parse_expr(exprs[0]) if exprs else None
        if elem.attrib['type'] == 'fd':
          items.append(FdListItem(elem.attrib['name'], expr))
        elif elem.attrib['type'] == 'STR':
          items.append(StrListItem(elem.attrib['name'], expr))
        else:
          items.append(ListItem(elem.attrib['name'], resolve(elem.attrib['type']), expr))
      elif tag == 'fd':
        items.append(FdItem(elem.attrib['name']))
      elif tag == 'switch':
        sub = list(elem)
        mask_field = sub[0].text.strip()  # <fieldref>value_mask</fieldref>
        cases = []
        for bitcase in sub[1:]:
          enumref = bitcase.find('enumref')
          fields = [e for e in bitcase if e.tag == 'field']
          assert len(fields) == 1, 'multi-field bitcases are out of scope'
          bits = enum_bit_values(module, enumref.attrib['ref'])
          cases.append(SwitchCase(bits[enumref.text.strip()], Field(fields[0].attrib['name'], resolve(fields[0].attrib['type']))))
        items.append(SwitchItem(mask_field, cases))
      else:
        raise SystemExit(f'x11.py: unsupported element <{tag}>')
    return items

  def fixed_items_size(items):
    # Wire size of a fully fixed layout; None if any item is dynamic.
    size = 0
    for item in items:
      if isinstance(item, Field):
        if item.type.size is None:
          return None
        size += item.type.size
      elif isinstance(item, Pad):
        size += item.bytes
      elif isinstance(item, ListItem) and item.const_len() is not None:
        size += item.type.size * item.const_len()
      elif isinstance(item, FdItem) or isinstance(item, FdListItem):
        continue  # ancillary, no wire bytes
      else:
        return None
    return size

  def fixed_wire_bytes(items):
    # Wire bytes of the fixed items alone; dynamic lists are counted separately.
    size = 0
    for item in items:
      if isinstance(item, Field):
        size += item.type.size
      elif isinstance(item, Pad):
        size += item.bytes
      elif isinstance(item, ListItem) and item.const_len() is not None:
        size += item.type.size * item.const_len()
    return size

  def parse_module(module):
    for elem in module.root:
      tag = elem.tag
      if tag == 'import':
        continue
      if tag in ('xidtype', 'xidunion'):
        name = elem.attrib['name']
        types[name] = Type(name, 4)
        module.typedefs.append((name, 'U32'))
      elif tag == 'typedef':
        old = resolve(elem.attrib['oldname'])
        name = elem.attrib['newname']
        types[name] = Type(name, old.size)
        module.typedefs.append((name, old.cpp))
      elif tag == 'enum':
        entries = []
        for item in elem.iter('item'):
          bit = item.find('bit')
          value = item.find('value')
          entries.append((item.attrib['name'], (1 << int(bit.text)) if bit is not None else int(value.text, 0)))
        module.enums.append((elem.attrib['name'], entries))
      elif tag == 'struct':
        items = parse_items(elem, module)
        size = fixed_items_size(items)
        name = elem.attrib['name']
        if size is None:
          types[name] = Type(name, None, is_struct=True)  # variable: usable only by name
        else:
          types[name] = Type(name, size, is_struct=True)
          module.structs.append((name, items, size))
      elif tag == 'union':
        members = []
        size = 0
        for lst in elem.iter('list'):
          t = resolve(lst.attrib['type'])
          count = parse_expr(next(e for e in lst if e.tag == 'value'))[1]
          members.append((t.cpp, field_name(lst.attrib['name']), count))
          size = max(size, t.size * count)
        name = elem.attrib['name']
        types[name] = Type(name, size, is_struct=True)
        module.unions.append((name, members, size))
      elif tag == 'request':
        items = parse_items(elem, module)
        reply_elem = elem.find('reply')
        reply = Message(elem.attrib['name'] + 'Reply', parse_items(reply_elem, module)) if reply_elem is not None else None
        module.requests.append(Request(elem.attrib['name'], int(elem.attrib['opcode']), items, reply))
      elif tag == 'event':
        module.events.append(Event(elem.attrib['name'], int(elem.attrib['number']), parse_items(elem, module),
                                   elem.attrib.get('xge') == 'true',
                                   elem.attrib.get('no-sequence-number') == 'true'))
      elif tag == 'eventcopy':
        ref = next(e for e in module.events if e.name == elem.attrib['ref'])
        module.events.append(Event(elem.attrib['name'], int(elem.attrib['number']), ref.items, ref.xge, ref.no_sequence))
      elif tag == 'error':
        module.errors.append((elem.attrib['name'], int(elem.attrib['number'])))
      elif tag == 'errorcopy':
        module.errors.append((elem.attrib['name'], int(elem.attrib['number'])))
      else:
        raise SystemExit(f'x11.py: unsupported top-level element <{tag}>')

  def member_decl(item):
    if isinstance(item, Field):
      return f'{item.type.cpp} {item.name};'
    if isinstance(item, ListItem):
      n = item.const_len()
      if n is not None:
        return f'{item.type.cpp} {item.name}[{n}];'
      if item.type.cpp == 'char':
        return f'StrView {item.name};'
      # A list of a variable-size struct only appears in a reply the server encodes by
      # hand (ListHosts, QueryPictFormats); carry it as raw bytes.
      if item.type.is_struct and item.type.size is None:
        return f'Span<const U8> {item.name};'
      return f'Span<const {item.type.cpp}> {item.name};'
    if isinstance(item, StrListItem):
      return f'Span<const StrView> {item.name};'
    if isinstance(item, FdItem):
      return f'FD {item.name};'
    if isinstance(item, FdListItem):
      return f'Vec<FD> {item.name};'
    return None

  def message_members(msg):
    out = []
    for item in msg.items:
      if isinstance(item, Pad):
        continue
      if isinstance(item, SwitchItem):
        for case in item.cases:
          out.append(f'  Optional<{case.field.type.cpp}> {case.field.name};')
        continue
      out.append('  ' + member_decl(item))
    return out

  # --- decode -------------------------------------------------------------------------

  def first_item_in_byte1(items):
    # Core requests, events and replies carry their first element in header byte 1.
    if items and isinstance(items[0], Field) and items[0].type.size == 1:
      return True, items[1:]
    if items and isinstance(items[0], Pad) and items[0].bytes == 1:
      return False, items[1:]
    return False, items

  def decode_lines(msg, items, module):
    out = []
    for item in items:
      if isinstance(item, Field):
        out.append(f'  m.{item.name} = r.Fixed<{item.type.cpp}>();')
      elif isinstance(item, Pad):
        out.append(f'  r.Skip({item.bytes});')
      elif isinstance(item, SwitchItem):
        for case in item.cases:
          out.append(f'  if (m.{field_name(item.mask_field)} & {case.bit}u) m.{case.field.name} = ({case.field.type.cpp})r.Fixed<U32>();')
      elif isinstance(item, ListItem):
        n = item.const_len()
        if n is not None:
          out.append(f'  r.Bytes(m.{item.name}, {n * item.type.size});')
        elif item.expr is None:
          if item.type.cpp == 'char':
            out.append(f'  m.{item.name} = r.RestStr();')
          else:
            out.append(f'  m.{item.name} = r.Rest<{item.type.cpp}>();')
        else:
          count = render_expr(item.expr, 'm.')
          if item.type.cpp == 'char':
            out.append(f'  m.{item.name} = r.Str({count});')
          else:
            out.append(f'  m.{item.name} = r.List<{item.type.cpp}>({count});')
      elif isinstance(item, StrListItem):
        out.append(f'  // LISTofSTR {item.name}: not decoded (unused by any handler)')
        out.append('  r.Rest<U8>();')
      elif isinstance(item, FdItem):
        out.append(f'  m.{item.name} = r.TakeFd(client);')
      elif isinstance(item, FdListItem):
        count = render_expr(item.expr, 'm.')
        out.append(f'  for (size_t i = 0; i < {count}; ++i) m.{item.name}.push_back(r.TakeFd(client));')
    return out

  def gen_request_decode(w, req, module):
    name = req.name
    w(f'{name} {name}::Decode(Client& client, Reader& r) {{')
    w(f'  {name} m{{}};')
    w('  m.sequence = ClientSequence(client);')
    items = req.items
    if module.namespace is None:  # core: first element rides header byte 1
      in_byte1, items = first_item_in_byte1(req.items)
      if in_byte1:
        w(f'  m.{req.items[0].name} = ({req.items[0].type.cpp})r.data_byte;')
    for line in decode_lines(req, items, module):
      w(line)
    w('  return m;')
    w('}')

  # --- encode -------------------------------------------------------------------------

  def encodable(msg):
    for item in msg.items:
      if isinstance(item, SwitchItem):
        return False
      if isinstance(item, ListItem) and item.type.size is None:
        return False
      if isinstance(item, StrListItem) and item.expr and item.expr[0] != 'fieldref':
        return False
    return True

  def simple_len_fields(msg):
    # Fields that hold the length of exactly one list: auto-filled from the span.
    refs = {}
    for item in msg.items:
      if isinstance(item, (ListItem, StrListItem)) and item.expr and item.expr[0] == 'fieldref':
        refs.setdefault(item.expr[1], []).append(item.name)
    return {field: lists[0] for field, lists in refs.items() if len(lists) == 1}

  def encode_field_value(msg, item, auto_len):
    if item.name in {field_name(k) for k in auto_len}:
      src = next(v for k, v in auto_len.items() if field_name(k) == item.name)
      return f'({item.type.cpp})m.{src}.size()'
    return f'm.{item.name}'

  def gen_encode_body(w, msg, items, auto_len, indent='  '):
    # Emits Writer `wr` calls for `items` (the part after the message header).
    for item in items:
      if isinstance(item, Field):
        w(f'{indent}wr.Fixed<{item.type.cpp}>({encode_field_value(msg, item, auto_len)});')
      elif isinstance(item, Pad):
        w(f'{indent}wr.Skip({item.bytes});')
      elif isinstance(item, ListItem):
        n = item.const_len()
        if n is not None:
          w(f'{indent}wr.Bytes(m.{item.name}, {n * item.type.size});')
        else:
          w(f'{indent}wr.List(m.{item.name});')
          w(f'{indent}wr.Pad4();')
      elif isinstance(item, StrListItem):
        w(f'{indent}wr.StrList(m.{item.name});')
        w(f'{indent}wr.Pad4();')
      elif isinstance(item, FdItem):
        w(f'{indent}wr.TakeFd(std::move(m.{item.name}));')
      elif isinstance(item, FdListItem):
        w(f'{indent}for (auto& fd : m.{item.name}) wr.TakeFd(std::move(fd));')

  def var_size_terms(items):
    terms = []
    for item in items:
      if isinstance(item, ListItem) and item.const_len() is None:
        terms.append(f'Pad4(m.{item.name}.size() * {item.type.size})')
      elif isinstance(item, StrListItem):
        terms.append(f'Pad4(StrListBytes(m.{item.name}))')
    return terms

  def gen_reply_encode(w, req, module):
    reply = req.reply
    req_name = req.name
    reply_name = reply.name
    in_byte1, items = first_item_in_byte1(reply.items)
    auto_len = simple_len_fields(reply)
    w(f'void {req_name}::Reply(Client& client, {reply_name}&& m) const {{')
    terms = ' + '.join([f'{8 + fixed_wire_bytes(items)}'] + var_size_terms(items))
    w(f'  size_t total = std::max<size_t>(32, Pad4({terms}));')
    w('  Writer wr(client, total);')
    w('  wr.Fixed<U8>(1);')
    if in_byte1:
      w(f'  wr.Fixed<{reply.items[0].type.cpp}>({encode_field_value(reply, reply.items[0], auto_len)});')
    else:
      w('  wr.Skip(1);')
    w('  wr.Fixed<U16>(sequence);')
    w('  wr.Fixed<U32>((U32)((total - 32) / 4));')
    gen_encode_body(w, reply, items, auto_len)
    w('}')

  def gen_event_encode(w, event, module):
    name = event.name
    auto_len = {}
    w(f'void {name}::Send(Client& client) const {{')
    w('  auto& m = *this;')
    if event.xge:
      items = event.items
      w(f'  size_t total = std::max<size_t>(32, Pad4({10 + fixed_wire_bytes(items)}));')
      w('  Writer wr(client, total);')
      w('  wr.Fixed<U8>(35);  // GenericEvent')
      w(f'  wr.Fixed<U8>(kMajorOpcode);')
      w('  wr.Fixed<U16>(ClientSequence(client));')
      w('  wr.Fixed<U32>((U32)((total - 32) / 4));')
      w(f'  wr.Fixed<U16>({event.number});')
    else:
      number = f'kFirstEvent + {event.number}' if module.namespace else str(event.number)
      w('  Writer wr(client, 32);')
      w(f'  wr.Fixed<U8>({number});')
      if event.no_sequence:
        items = event.items
      else:
        in_byte1, items = first_item_in_byte1(event.items)
        if in_byte1:
          w(f'  wr.Fixed<{event.items[0].type.cpp}>({encode_field_value(event, event.items[0], auto_len)});')
        else:
          w('  wr.Skip(1);')
        w('  wr.Fixed<U16>(ClientSequence(client));')
    gen_encode_body(w, event, items, auto_len)
    w('}')

  # --- top-level emission ---------------------------------------------------------------

  def gen_hpp(modules):
    lines = []
    w = lines.append
    w('#pragma once')
    w('// SPDX-FileCopyrightText: Copyright 2026 Automat Authors')
    w('// SPDX-License-Identifier: MIT')
    w('// Generated by src/x11.py from src/x11/*.xml. Do not edit.')
    w('')
    w('#include "../../src/fd.hpp"')
    w('#include "../../src/int.hpp"')
    w('#include "../../src/optional.hpp"')
    w('#include "../../src/span.hpp"')
    w('#include "../../src/str.hpp"')
    w('#include "../../src/vec.hpp"')
    w('')
    w('namespace automat::x11 {')
    w('')
    w('struct Client;')
    w('struct Reader;')
    w('')
    # XID and typedef aliases live at the top scope so a later extension (e.g. present)
    # can name another's type (e.g. dri3::SYNCOBJ) through enclosing-namespace lookup.
    emitted = set()
    for module in modules:
      for name, underlying in module.typedefs:
        if name not in emitted:
          emitted.add(name)
          w(f'using {name} = {underlying};')
    for module in modules:
      w('')
      w(f'// ---- {module.xml_name} ----')
      w('')
      if module.namespace:
        w(f'namespace {module.namespace} {{')
        w(f'constexpr StrView kXName = "{module.xname}"sv;')
        w(f'constexpr U8 kMajorOpcode = {module.major_opcode};')
        w(f'constexpr U8 kFirstEvent = {module.first_event};')
        w(f'constexpr U8 kFirstError = {module.first_error};')
        w('')
      for name, entries in module.enums:
        w(f'enum : U32 {{  // {name}')
        for item_name, value in entries:
          w(f'  {name}{item_name} = {value},')
        w('};')
      for name, number in module.errors:
        w(f'constexpr U8 {name}Error = {module.first_error} + {number};')
      w('')
      for name, members, size in module.unions:
        w(f'union {name} {{')
        for cpp, member_name, count in members:
          w(f'  {cpp} {member_name}[{count}];')
        w('};')
        w(f'static_assert(sizeof({name}) == {size});')
        w('')
      for name, items, size in module.structs:
        w(f'struct {name} {{')
        pad_i = 0
        for item in items:
          if isinstance(item, Pad):
            w(f'  U8 pad{pad_i}[{item.bytes}];')
            pad_i += 1
          else:
            w('  ' + member_decl(item))
        w('};')
        w(f'static_assert(sizeof({name}) == {size});')
        w('')
      for event in module.events:
        if module.namespace is None and event.xge:
          continue  # the core GeGeneric event is only a decode container, never sent
        w(f'struct {event.name} {{')
        w(f'  static constexpr U8 Number = {event.number};')
        for line in message_members(event):
          w(line)
        w('  void Send(Client&) const;')
        w('};')
        w('')
      for req in module.requests:
        if req.reply:
          w(f'struct {req.reply.name} {{')
          for line in message_members(req.reply):
            w(line)
          w('};')
        w(f'struct {req.name} {{')
        w(f'  static constexpr U8 Opcode = {req.opcode};')
        w('  U16 sequence;')
        for line in message_members(req):
          w(line)
        w(f'  static {req.name} Decode(Client&, Reader&);')
        w('  void Handle(Client&);')
        if req.reply:
          if encodable(req.reply):
            w(f'  void Reply(Client&, {req.reply.name}&&) const;')
          else:
            w(f'  // {req.reply.name} contains variable-size structures; encode manually.')
        w('};')
        w('')
      w(f'bool {"Dispatch" if module.namespace else "DispatchCore"}(Client&, U8 opcode, Reader&);')
      w('StrView RequestName(U8 opcode);')
      if module.namespace:
        w(f'}}  // namespace {module.namespace}')
    w('')
    w('}  // namespace automat::x11')
    w('')
    generated_hpp.write_text('\n'.join(lines))

  def gen_cpp(modules):
    lines = []
    w = lines.append
    w('// SPDX-FileCopyrightText: Copyright 2026 Automat Authors')
    w('// SPDX-License-Identifier: MIT')
    w('// Generated by src/x11.py from src/x11/*.xml. Do not edit.')
    w('')
    w('#include <bit>')
    w('#include <utility>')
    w('')
    w('#include "../../src/x11_protocol.hpp"')
    w('')
    w('namespace automat::x11 {')
    for module in modules:
      w('')
      w(f'// ---- {module.xml_name} ----')
      w('')
      if module.namespace:
        w(f'namespace {module.namespace} {{')
        w('')
      for event in module.events:
        if module.namespace is None and event.xge:
          continue  # the core GeGeneric event is only a decode container, never sent
        gen_event_encode(w, event, module)
        w('')
      for req in module.requests:
        gen_request_decode(w, req, module)
        w('')
        if req.reply and encodable(req.reply):
          gen_reply_encode(w, req, module)
          w('')
      dispatch = 'Dispatch' if module.namespace else 'DispatchCore'
      w(f'bool {dispatch}(Client& client, U8 opcode, Reader& r) {{')
      w('  switch (opcode) {')
      for req in module.requests:
        w(f'    case {req.opcode}: {{')
        w(f'      auto m = {req.name}::Decode(client, r);')
        w(f'      if (!r.ok) return DecodeFailed(client, m.sequence);')
        w(f'      m.Handle(client);')
        w('    } break;')
      w('    default:')
      w('      return false;')
      w('  }')
      w('  return true;')
      w('}')
      w('')
      w('StrView RequestName(U8 opcode) {')
      w('  switch (opcode) {')
      for req in module.requests:
        w(f'    case {req.opcode}: return "{req.name}"sv;')
      w('  }')
      w('  return "?"sv;')
      w('}')
      if module.namespace:
        w(f'}}  // namespace {module.namespace}')
    w('')
    w('}  // namespace automat::x11')
    w('')
    generated_cpp.write_text('\n'.join(lines))

  modules = []

  def gen_x11():
    global modules
    types.clear()
    for name, (cpp, size) in PRIMITIVES.items():
      types[name] = Type(cpp, size)
    for name in EXTERNAL_XIDS:
      types[name] = Type('U32', 4)
    paths = sorted(xml_dir.glob('*.xml'))
    modules = [Module(p) for p in paths]
    modules.sort(key=lambda m: (m.header != 'xproto', m.header))
    for module in modules:
      parse_module(module)
    next_major, next_event, next_error = 129, 64, 128
    for module in modules[1:]:
      module.major_opcode = next_major
      next_major += 1
      # present's "Generic" describes the GenericEvent frame itself; xge events do not
      # consume event codes.
      real_events = [e.number for e in module.events if not e.xge and e.name != 'Generic']
      if real_events:
        module.first_event = next_event
        next_event += max(real_events) + 1
      if module.errors:
        module.first_error = next_error
        next_error += max(number for _, number in module.errors) + 1
    gen_hpp(modules)
    gen_cpp(modules)

  def hook_srcs(srcs: dict[str, 'src.File'], recipe: 'make.Recipe'):
    recipe.add_step(
        gen_x11,
        [generated_cpp, generated_hpp],
        [__file__, *sorted(str(p) for p in xml_dir.glob('*.xml'))],
        desc='Generating X11 bindings',
        shortcut='x11-generated')
    for path in (generated_cpp, generated_hpp):
      recipe.generated.add(str(path))

    cpp_file = src.File(generated_cpp)
    cpp_file.direct_includes.append(str(protocol_hpp))
    srcs[str(generated_cpp)] = cpp_file

    srcs[str(generated_hpp)] = src.File(generated_hpp)
