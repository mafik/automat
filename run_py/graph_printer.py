import fs_utils
import json
import make
from pathlib import Path
from subprocess import Popen
from collections import defaultdict

def short_path(path_str):
  path = Path(path_str)
  if path.is_relative_to(fs_utils.project_root):
    return str(path.relative_to(fs_utils.project_root))
  else:
    return path_str

def to_json(recipe):
  file_to_step = {}
  obj = []
  for step in recipe.steps:
    for output in step.outputs:
      file_to_step[output] = step
    obj.append({
        'id': step.id,
        'name': step.shortcut,
        'steps_before': set(),
        'steps_after': set(),
        'group': [step.id],
    })
  for step in recipe.steps:
    for inp in step.inputs:
      if inp in file_to_step:
        step_before = file_to_step[inp]
        obj[step_before.id]['steps_after'].add(step.id)
        obj[step.id]['steps_before'].add(step_before.id)
  for step in obj:
    step['steps_before'] = list(sorted(step['steps_before']))
    step['steps_after'] = list(sorted(step['steps_after']))
  # combine steps with the same steps_before and steps_after
  groups = defaultdict(list) # maps key (steps_before, steps_after) to list of step ids
  for step in obj:
    if len(step['steps_before']) + len(step['steps_after']) == 0:
      continue
    key = ','.join(str(x) for x in step['steps_before']) + ' => ' + ','.join(str(x) for x in step['steps_after'])
    groups[key].append(step['id'])
  for key, steps in groups.items():
    leader_id = steps[0]
    for id in steps[1:]:
      obj[leader_id]['name'] += f'\n{obj[id]["name"]}'
      obj[leader_id]['group'].append(id)
      for other_step_id in obj[id]['steps_before']:
        other_step = obj[other_step_id]
        other_step['steps_after'].remove(id)
      obj[id]['steps_before'].clear()
      for other_step_id in obj[id]['steps_after']:
        other_step = obj[other_step_id]
        other_step['steps_before'].remove(id)
      obj[id]['steps_after'].clear()
      obj[id] = None
  obj = {x['id']: x for x in obj if x is not None}
  return json.dumps(obj)

def print_graph(recipe):
  fs_utils.build_dir.mkdir(parents=True, exist_ok=True)
  file_to_step = {}
  for step in recipe.steps:
    for output in step.outputs:
      file_to_step[output] = step
  html = ''
  html += '<html><meta charset="utf-8"><link rel="stylesheet" href="../run_py/graph.css"><body>\n'
  for step in recipe.steps:
    html += f'<h2 id="{step.id}">{step.shortcut} <a href="#{step.id}">#</a></h2>\n'

    if hasattr(step.build, 'func'):
      func = step.build.func
    elif callable(step.build):
      func = step.build
    else:
      func = None

    if hasattr(step.build, 'args'):
      args = step.build.args
    else:
      args = []

    if hasattr(step.build, 'keywords'):
      kwargs = step.build.keywords
    else:
      kwargs = {}

    if func:

      if 'env' in kwargs:
        html += '<details><summary>env</summary><pre class="env">'
        for k, v in kwargs['env'].items():
          html += f'{k}={v}\n'
        html += '</pre></details>\n'
      if 'cwd' in kwargs:
        html += f'<p>Working directory: <code class="cwd">{kwargs["cwd"]}</code></p>\n'
      if func is make.Popen:
        html += f'<pre class="shell">'
        for arg in args[0]:
          html += f'{str(arg)} '
        html += '</pre>\n'
      else:
        # Python function
        html += f'<pre class="python">'
        if hasattr(func, '__self__'):
          html += f'{repr(func.__self__)}.'
        html += f'{func.__name__}('
        first = True
        for x in args:
          if not first:
            html += ', '
          first = False
          html += repr(x)
        for k, v in kwargs.items():
          if not first:
            html += ', '
          first = False
          html += f'{k}={repr(v)}'
        html += ')</pre>\n'
    html += f'<p>Message: <em>{step.desc}</em></p>\n'
    if step.inputs:
      html += '<p>Inputs: '
      for inp in sorted(str(x) for x in step.inputs):
        inp_short = short_path(inp)
        if inp in file_to_step:
          input_step = file_to_step[inp]
          html += f'<a href="#{input_step.id}">{inp_short}</a>'
        else:
          html += inp_short
        html += ', '
      html += '</p>\n'
    html += '<p>Outputs: '
    for out in sorted(str(x) for x in step.outputs):
      html += short_path(out)
      html += ', '
    html += '</p>\n'
  html += '<script>\n'
  html += 'const graph = '
  html += to_json(recipe)
  html += ';\n'
  html += '</script>\n'
  html += '<script src="../run_py/graph.js"></script>'
  html += '</body></html>'
  return html
