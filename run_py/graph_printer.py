import fs_utils
import json
import make
from pathlib import Path
from subprocess import Popen

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
  return json.dumps(obj, indent=2)

def print_graph(recipe, html_path):
  fs_utils.build_dir.mkdir(parents=True, exist_ok=True)
  file_to_step = {}
  for step in recipe.steps:
    for output in step.outputs:
      file_to_step[output] = step
  with open(html_path, 'w') as f:
    f.write('<html><meta charset="utf-8"><link rel="stylesheet" href="../run_py/graph.css"><body>\n')
    for step in recipe.steps:
      f.write(f'<h2 id="{step.id}">{step.shortcut} <a href="#{step.id}">#</a></h2>\n')

      if hasattr(step.build, 'func'):
        func = step.build.func
      elif callable(step.build):
        func = step.build

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
          f.write('<details><summary>env</summary><pre class="env">')
          for k, v in kwargs['env'].items():
            f.write(f'{k}={v}\n')
          f.write('</pre></details>\n')
        if 'cwd' in kwargs:
          f.write(f'<p>Working directory: <code class="cwd">{kwargs["cwd"]}</code></p>\n')
        if func is make.Popen:
          f.write(f'<pre class="shell">')
          for arg in args[0]:
            f.write(f'{str(arg)} ')
          f.write('</pre>\n')
        else:
          # Python function
          f.write(f'<pre class="python">{func.__name__}(')
          f.write(', '.join(repr(x) for x in args))
          for k, v in kwargs.items():
            f.write(f', {k}={repr(v)}')
          f.write(')</pre>\n')
      f.write(f'<p>Message: <em>{step.desc}</em></p>\n')
      if step.inputs:
        f.write('<p>Inputs: ')
        for inp in sorted(str(x) for x in step.inputs):
          inp_short = short_path(inp)
          if inp in file_to_step:
            input_step = file_to_step[inp]
            f.write(f'<a href="#{input_step.id}">{inp_short}</a>')
          else:
            f.write(inp_short)
          f.write(', ')
        f.write('</p>\n')
      f.write('<p>Outputs: ')
      for out in sorted(str(x) for x in step.outputs):
        f.write(short_path(out))
        f.write(', ')
      f.write('</p>\n')
    f.write('<script>\n')
    f.write('const graph = ')
    f.write(to_json(recipe))
    f.write(';\n')
    f.write('</script>\n')
    f.write('<script src="../run_py/graph.js"></script>')
    f.write('</body></html>')
