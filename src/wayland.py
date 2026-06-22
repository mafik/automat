# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import xml.etree.ElementTree as ET

import fs_utils
import make
import src

# Requests whose handler the server implements by hand; the rest get an empty
# generated default so the generated dispatcher always has something to call.
HANDLED = {
  ('wl_display', 'sync'),
  ('wl_display', 'get_registry'),
  ('wl_registry', 'bind'),
  ('wl_compositor', 'create_surface'),
  ('wl_shm', 'create_pool'),
  ('wl_shm_pool', 'create_buffer'),
  ('wl_shm_pool', 'resize'),
  ('wl_surface', 'attach'),
  ('wl_surface', 'frame'),
  ('wl_surface', 'commit'),
  ('wl_surface', 'set_buffer_scale'),
  ('wl_surface', 'set_buffer_transform'),
  ('wl_surface', 'set_input_region'),
  ('wl_region', 'add'),
  ('wl_region', 'subtract'),
  ('wl_seat', 'get_pointer'),
  ('wl_seat', 'get_keyboard'),
  ('wl_subcompositor', 'get_subsurface'),
  ('wl_subsurface', 'set_position'),
  ('wl_subsurface', 'set_sync'),
  ('wl_subsurface', 'set_desync'),
  ('wl_subsurface', 'place_above'),
  ('wl_subsurface', 'place_below'),
  ('wp_viewporter', 'get_viewport'),
  ('wp_viewport', 'set_source'),
  ('wp_viewport', 'set_destination'),
  ('zxdg_decoration_manager_v1', 'get_toplevel_decoration'),
  ('zxdg_toplevel_decoration_v1', 'set_mode'),
  ('zxdg_toplevel_decoration_v1', 'unset_mode'),
  ('xdg_surface', 'get_popup'),
  ('xdg_positioner', 'set_size'),
  ('xdg_positioner', 'set_anchor_rect'),
  ('xdg_positioner', 'set_anchor'),
  ('xdg_positioner', 'set_gravity'),
  ('xdg_positioner', 'set_offset'),
  ('xdg_positioner', 'set_constraint_adjustment'),
  ('xdg_popup', 'grab'),
  ('xdg_popup', 'reposition'),
  ('xdg_popup', 'destroy'),
  ('wp_cursor_shape_device_v1', 'set_shape'),
  ('wl_data_device_manager', 'get_data_device'),
  ('wl_data_device', 'set_selection'),
  ('wl_data_source', 'offer'),
  ('wl_data_offer', 'receive'),
  ('zwp_linux_dmabuf_v1', 'get_default_feedback'),
  ('zwp_linux_dmabuf_v1', 'get_surface_feedback'),
  ('zwp_linux_buffer_params_v1', 'add'),
  ('zwp_linux_buffer_params_v1', 'create'),
  ('zwp_linux_buffer_params_v1', 'create_immed'),
  ('xdg_wm_base', 'get_xdg_surface'),
  ('xdg_surface', 'get_toplevel'),
  ('xdg_surface', 'set_window_geometry'),
  ('xdg_surface', 'ack_configure'),
  ('xdg_surface', 'destroy'),
  ('xdg_toplevel', 'set_title'),
  ('xdg_toplevel', 'set_app_id'),
}

if sys.platform == 'linux':
  cpp = fs_utils.generated_dir / 'wayland_protocols.cpp'
  hpp = fs_utils.generated_dir / 'wayland_protocols.hpp'
  hpp_forward = fs_utils.generated_dir / 'wayland_protocols_forward.hpp'

  TYPE_MAP = {
    'int': 'I32',
    'uint': 'U32',
    'fixed': 'float',
    'string': 'StrView',
    'array': 'Span<>',
    'fd': 'FD&&',
    'new_id': 'INTERFACE',
    'object': 'INTERFACE',
  }

  # Interface names that have a generated struct; an object/new_id of any other
  # (forward-declared only, e.g. zwp_tablet_tool_v2) is carried as a bare id.
  generated_interfaces = set()

  def camel_case(snake):
    return ''.join(word.capitalize() for word in snake.split('_'))

  def no_prefix(name):
    for prefix in ('zwp_', 'wp_', 'wl_'):
      if name.startswith(prefix):
        return name[len(prefix):]
    return name

  def cpp_name(name):
    return camel_case(no_prefix(name))

  def block_comment(text, prefix='// '):
    deindent = 10
    lines = text.splitlines()
    while lines[0] == '' or lines[0].isspace():
      lines = lines[1:]
    while lines[-1] == '' or lines[-1].isspace():
      lines = lines[:-1]
    for line in lines:
      if line.isspace() or len(line) == 0:
        continue
      deindent = min(deindent, max(line.count(' ', 0, deindent), line.count('\t', 0, deindent)))
    return '\n'.join(prefix + line[deindent:] for line in lines)

  def messages(iface):
    return (*iface.iter('request'), *iface.iter('event'))

  def interface_methods(iface):
    names = {'On' + camel_case(r.attrib['name']) for r in iface.iter('request')}
    return names | {camel_case(e.attrib['name']) for e in iface.iter('event')}

  def arg_type(arg, methods):
    t = TYPE_MAP[arg.attrib['type']] if 'type' in arg.attrib else '???'
    enum = arg.get('enum')
    if enum:
      if '.' in enum:
        outer, _, inner = enum.partition('.')
        t = 'enum ' + cpp_name(outer) + '::' + camel_case(inner)
      else:
        t = 'enum ' + camel_case(enum)
    if t == 'INTERFACE':
      if arg.attrib.get('interface') in generated_interfaces:
        t = cpp_name(arg.attrib['interface'])
        if t in methods:
          t = 'struct ' + t
        t += '*' if arg.get('allow-null', 'false') == 'true' else '&'
      else:
        t = 'U32'
    return t

  # A bare new_id (no fixed interface, as in wl_registry.bind) is three wire
  # fields: the interface name, the version, and the id.
  def is_dynamic_new_id(arg):
    return arg.attrib.get('type') == 'new_id' and 'interface' not in arg.attrib

  def param_list(message, methods):
    params = []
    for arg in message.iter('arg'):
      name = arg.attrib['name']
      if is_dynamic_new_id(arg):
        params += [f'StrView {name}_interface', f'U32 {name}_version', f'U32 {name}']
      else:
        params.append(f'{arg_type(arg, methods)} {name}')
    return params

  def signature(message, methods):
    return ', '.join(param_list(message, methods))

  def size_terms(event):
    terms = ['8']
    for arg in event.iter('arg'):
      t = arg.attrib.get('type')
      if t == 'string':
        terms.append(f'SizeString({arg.attrib["name"]})')
      elif t == 'array':
        terms.append(f'SizeArray({arg.attrib["name"]})')
      elif t != 'fd':
        terms.append('4')
    return ' + '.join(terms)

  def write_arg(arg):
    name = arg.attrib['name']
    t = arg.attrib.get('type')
    if t == 'string':
      return f'WriteString(p, {name})'
    if t == 'array':
      return f'WriteArray(p, {name})'
    if t == 'fixed':
      return f'WriteFixed(p, {name})'
    if t == 'fd':
      return f'client.out_fds.push_back(std::move({name}))'
    if t in ('object', 'new_id') and arg.attrib.get('interface') in generated_interfaces:
      if arg.get('allow-null', 'false') == 'true':
        return f'Write(p, {name} ? {name}->id : 0)'
      return f'Write(p, {name}.id)'
    return f'Write(p, {name})'

  def decode_arg(arg, index, methods, iface):
    name = f'arg{index}'
    t = arg.attrib.get('type')
    bad = lambda msg: f'client.ProtocolError(id, Display::ErrorInvalidMethod, "{msg}"sv); return;'
    enum = arg.get('enum')
    if enum:
      if '.' in enum:
        outer, _, inner = enum.partition('.')
        qualified = cpp_name(outer) + '::' + camel_case(inner)
      else:
        qualified = iface + '::' + camel_case(enum)
      return ([f'enum {qualified} {name} = static_cast<enum {qualified}>(Read(p, end));'], [name])
    if t == 'int':
      return ([f'I32 {name} = ReadInt(p, end);'], [name])
    if t == 'fixed':
      return ([f'float {name} = ReadFixed(p, end);'], [name])
    if t == 'string':
      return ([f'StrView {name} = ReadString(p, end);'], [name])
    if t == 'array':
      return ([f'Span<> {name} = ReadArray(p, end);'], [name])
    if t == 'fd':
      return ([f'FD {name} = ReadFd(client);'], [f'std::move({name})'])
    if t == 'new_id' and arg.attrib.get('interface') in generated_interfaces:
      kind = cpp_name(arg.attrib['interface'])
      return ([f'U32 {name}_id = Read(p, end);',
               f'if (!client.CheckId({name}_id)) {{ {bad("Wayland requires that new_id arguments are allocated starting from 1 and without gaps.")} }}',
               f'{kind}& {name} = {kind}::ColonyMake({name}_id, client);'], [name])
    if is_dynamic_new_id(arg):
      return ([f'StrView {name}_interface = ReadString(p, end);',
               f'U32 {name}_version = Read(p, end);',
               f'U32 {name} = Read(p, end);',
               f'if (!client.CheckId({name})) {{ {bad("Wayland requires that new_id arguments are allocated starting from 1 and without gaps.")} }}'],
              [f'{name}_interface', f'{name}_version', name])
    if t == 'object' and arg.attrib.get('interface') in generated_interfaces:
      kind = cpp_name(arg.attrib['interface'])
      nullable = arg.get('allow-null', 'false') == 'true'
      guard = f'{name}_c && ' if nullable else f'!{name}_c || '
      cast = (f'{kind}* {name} = static_cast<{kind}*>({name}_c);' if nullable
              else f'{kind}& {name} = static_cast<{kind}&>(*{name}_c);')
      return ([f'Common* {name}_c = client.GetId(Read(p, end));',
               f'if ({guard}{name}_c->kind != {kind}::Kind) {{ {bad("Invalid argument type. Expected " + kind + ".")} }}',
               cast], [name])
    return ([f'U32 {name} = Read(p, end);'], [name])

  def enum_dependencies(iface):
    return {arg.get('enum').partition('.')[0]
            for message in messages(iface) for arg in message.iter('arg')
            if '.' in arg.get('enum', '')}

  def referenced_interfaces(iface):
    return {arg.attrib['interface']
            for message in messages(iface) for arg in message.iter('arg')
            if arg.get('interface')}

  def topo_sort(interfaces):
    ordered, resolved, pending = [], set(), list(interfaces)
    while pending:
      iface = pending.pop(0)
      if enum_dependencies(iface) <= resolved:
        resolved.add(iface.attrib['name'])
        ordered.append(iface)
      else:
        pending.append(iface)
    return ordered

  def gen_wayland():
    interfaces = []
    for xml in (fs_utils.src_dir / 'wayland').glob('*.xml'):
      interfaces += list(ET.parse(xml).getroot().iter('interface'))
    ordered = topo_sort(interfaces)
    our_names = {iface.attrib['name'] for iface in ordered}
    generated_interfaces.clear()
    generated_interfaces.update(our_names)
    external = sorted({ref for iface in ordered for ref in referenced_interfaces(iface)} - our_names)

    with hpp.open('w') as out:
      def w(line=''):
        print(line, file=out)

      def emit_doc(desc, indent, blank_if_none):
        if desc is None:
          if blank_if_none:
            w()
          return
        w(f'\n{indent}// {desc.attrib["summary"].capitalize()}')
        if desc.text:
          w(f'{indent}//')
          w(block_comment(desc.text, prefix=f'{indent}// '))

      def emit_enum(enum):
        name = camel_case(enum.attrib['name'])
        emit_doc(enum.find('description'), '  ', blank_if_none=True)
        w(f'  enum {name} : U32 {{')
        for entry in enum.iter('entry'):
          label = name + camel_case(entry.attrib['name'])
          if 'summary' in entry.attrib:
            w(f'    {label} = {entry.attrib["value"]}, // {entry.attrib["summary"].capitalize()}')
          else:
            w(f'    {label} = {entry.attrib["value"]},')
        w('  };')
        w('')
        w(f'  static StrView {name}ToStr(U32 value) {{')
        w('    switch (value) {')
        for entry in enum.iter('entry'):
          w(f'    case {entry.attrib["value"]}: return "{camel_case(entry.attrib["name"])}"sv;')
        w(f'    default: return "Unknown{name}"sv;')
        w('    }')
        w('  }')

      def emit_interface(iface):
        name = cpp_name(iface.attrib['name'])
        emit_doc(iface.find('description'), '', blank_if_none=True)
        w(f'struct {name} : Base<{name}> {{')
        w(f'  static constexpr int Version = {iface.attrib["version"]};')
        w(f'  static constexpr Kind Kind = Kind{name};')
        w('\n  static bool classof(const Common* c) { return c->kind == Kind; }  // LLVM-style RTTI')
        w(f'\n  static Colony<{name}> colony;')
        w('\n  // Do not use directly. Instead use `ColonyMake`')
        w(f'  {name}(U32 id, Client& client) : Base<{name}>(Kind, id, client) {{}}')
        w(f'\n  static {name}& ColonyMake(U32 id, Client& client) {{ return *colony.emplace(id, client); }}')
        w('  void ColonyDestroy() { colony.erase(colony.get_iterator(this)); }')
        methods = interface_methods(iface)
        for enum in iface.iter('enum'):
          emit_enum(enum)
        for request in iface.iter('request'):
          emit_doc(request.find('description'), '  ', blank_if_none=False)
          if request.get('type') == 'destructor':
            w('  //\n  // [destructor] After this method returns, this object will be released')
          w(f'  void On{camel_case(request.attrib["name"])}({signature(request, methods)});')
        for event in iface.iter('event'):
          emit_doc(event.find('description'), '  ', blank_if_none=False)
          w(f'  void {camel_case(event.attrib["name"])}({signature(event, methods)});')
        w('};')

      w('#pragma once\n')
      w('#include "../../src/colony.hpp"')
      w('#include "../../src/fd.hpp"')
      w('#include "../../src/int.hpp"')
      w('#include "../../src/span.hpp"')
      w('#include "../../src/wayland_ext.hpp"\n')
      w('namespace automat::wayland {')
      for iface in ordered:
        emit_interface(iface)
      w('\n}  // namespace automat::wayland')

    with cpp.open('w') as out:
      def w(line=''):
        print(line, file=out)

      w('// SPDX-FileCopyrightText: Copyright 2026 Automat Authors')
      w('// SPDX-License-Identifier: MIT')
      w('#include "wayland_protocols.hpp"\n')
      w('#include <cstring>')
      w('#include <utility>\n')
      w('namespace automat::wayland {')
      w('namespace {')
      w('void Write(char*& p, U32 value) { std::memcpy(p, &value, 4); p += 4; }')
      w('void WriteString(char*& p, StrView value) {')
      w('  U32 size = static_cast<U32>(value.size()) + 1;')
      w('  Write(p, size);')
      w('  std::memcpy(p, value.data(), value.size());')
      w('  std::memset(p + value.size(), 0, ((size + 3) & ~3u) - value.size());')
      w('  p += (size + 3) & ~3u;')
      w('}')
      w('void WriteArray(char*& p, Span<> value) {')
      w('  U32 size = static_cast<U32>(value.size_bytes());')
      w('  Write(p, size);')
      w('  std::memcpy(p, value.data(), size);')
      w('  std::memset(p + size, 0, ((size + 3) & ~3u) - size);')
      w('  p += (size + 3) & ~3u;')
      w('}')
      w('void WriteFixed(char*& p, float value) { Write(p, static_cast<U32>(static_cast<I32>(value * 256))); }')
      w('size_t SizeString(StrView value) { return 4 + ((value.size() + 4) & ~size_t(3)); }')
      w('size_t SizeArray(Span<> value) { return 4 + ((value.size_bytes() + 3) & ~size_t(3)); }')
      w('U32 Read(const char*& p, const char* end) {')
      w('  if (end - p < 4) { p = end; return 0; }')
      w('  U32 v; std::memcpy(&v, p, 4); p += 4; return v;')
      w('}')
      w('I32 ReadInt(const char*& p, const char* end) { return static_cast<I32>(Read(p, end)); }')
      w('float ReadFixed(const char*& p, const char* end) { return static_cast<I32>(Read(p, end)) / 256.0f; }')
      w('StrView ReadString(const char*& p, const char* end) {')
      w('  U32 size = Read(p, end);')
      w('  if (size == 0) return {};')
      w('  size_t padded = (size_t(size) + 3) & ~size_t(3);')
      w('  if (padded > size_t(end - p)) { p = end; return {}; }')
      w('  StrView v(p, size - 1);')
      w('  p += padded;')
      w('  return v;')
      w('}')
      w('Span<> ReadArray(const char*& p, const char* end) {')
      w('  U32 size = Read(p, end);')
      w('  size_t padded = (size_t(size) + 3) & ~size_t(3);')
      w('  if (padded > size_t(end - p)) { p = end; return {}; }')
      w('  p += padded;')
      w('  return {};')
      w('}')
      w('FD ReadFd(Client& client) {')
      w('  if (client.recv_fds.empty()) return FD();')
      w('  FD fd = std::move(client.recv_fds.front());')
      w('  client.recv_fds.pop_front();')
      w('  return fd;')
      w('}')
      w('}  // namespace\n')
      for iface in ordered:
        name = cpp_name(iface.attrib['name'])
        w(f'Colony<{name}> {name}::colony;')
      w('')
      for iface in ordered:
        name = cpp_name(iface.attrib['name'])
        methods = interface_methods(iface)
        for opcode, event in enumerate(iface.iter('event')):
          w(f'void {name}::{camel_case(event.attrib["name"])}({signature(event, methods)}) {{')
          w(f'  size_t wire_size = {size_terms(event)};')
          w('  size_t wire_header = client.out.size();')
          w('  client.out.resize(wire_header + wire_size);')
          w('  char* p = client.out.data() + wire_header;')
          w('  Write(p, this->id);')
          w(f'  Write(p, (static_cast<U32>(wire_size) << 16) | {opcode});')
          for arg in event.iter('arg'):
            w(f'  {write_arg(arg)};')
          w('}')
      w('')
      for iface in ordered:
        name = cpp_name(iface.attrib['name'])
        methods = interface_methods(iface)
        for request in iface.iter('request'):
          if (iface.attrib['name'], request.attrib['name']) in HANDLED:
            continue
          w(f'void {name}::On{camel_case(request.attrib["name"])}({signature(request, methods)}) {{}}')
      w('')
      w('void Common::GenericDispatch(U32 opcode, const char* p, const char* end) {')
      w('  switch (kind) {')
      for iface in ordered:
        name = cpp_name(iface.attrib['name'])
        requests = list(iface.iter('request'))
        if not requests:
          continue
        w(f'    case Kind{name}: {{')
        w(f'      auto& obj = static_cast<{name}&>(*this);')
        w('      switch (opcode) {')
        for opcode, request in enumerate(requests):
          w(f'        case {opcode}: {{')
          call = []
          for index, arg in enumerate(request.iter('arg')):
            decls, exprs = decode_arg(arg, index, methods, name)
            for decl in decls:
              w(f'          {decl}')
            call += exprs
          w(f'          obj.On{camel_case(request.attrib["name"])}({", ".join(call)});')
          if request.get('type') == 'destructor':
            w('          obj.ColonyDestroy();')
          w('        } break;')
        w('      }')
        w('    } break;')
      w('    default:')
      w('      break;')
      w('  }')
      w('}')
      w('')
      w('void Common::GenericColonyDestroy() {')
      w('  switch (kind) {')
      for iface in ordered:
        name = cpp_name(iface.attrib['name'])
        w(f'    case Kind{name}: static_cast<{name}&>(*this).ColonyDestroy(); break;')
      w('    default:')
      w('      break;')
      w('  }')
      w('}')
      w('\n}  // namespace automat::wayland')

    with hpp_forward.open('w') as out:
      def w(line=''):
        print(line, file=out)

      w('#pragma once\n')
      w('namespace automat::wayland {\n')
      for iface in ordered:
        w(f'struct {cpp_name(iface.attrib["name"])};')
      for name in external:
        w(f'struct {cpp_name(name)};')
      w('\nenum Kind {')
      w('  KindNone = 0,')
      for iface in ordered:
        w(f'  Kind{cpp_name(iface.attrib["name"])},')
      w('  KindEnd,  // sentinel')
      w('};')
      w('')
      for iface in ordered:
        w(f'using {iface.attrib["name"]} = {cpp_name(iface.attrib["name"])};')
      w('\n}  // namespace automat::wayland')

  def hook_srcs(srcs: dict[str, 'src.File'], recipe: 'make.Recipe'):
      recipe.add_step(
          gen_wayland,
          [cpp, hpp, hpp_forward],
          [__file__, 'src/wayland_ext.hpp'],
          desc='Generating Wayland stubs',
          shortcut='wayland-protocols')
      for path in (cpp, hpp, hpp_forward):
        recipe.generated.add(str(path))
        srcs[str(path)] = src.File(path)
