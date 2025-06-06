import fs_utils
from pathlib import Path

def short_path(path_str):
  path = Path(path_str)
  if path.is_relative_to(fs_utils.project_root):
    return str(path.relative_to(fs_utils.project_root))
  else:
    return path_str

def print_graph(recipe, html_path):
  fs_utils.build_dir.mkdir(parents=True, exist_ok=True)
  file_to_step = {}
  for step in recipe.steps:
      for output in step.outputs:
          file_to_step[output] = step
  with open(html_path, 'w') as f:
      f.write('<html><body>\n')
      for step in recipe.steps:
          f.write(f'<h2 id="{step.id}">{step.shortcut}</h2>\n')
          f.write(f'<p>{step.desc}</p>\n')
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
      f.write('</body></html>')
