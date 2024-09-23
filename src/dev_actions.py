import make
from pathlib import Path
import subprocess
import fs_utils
import shutil

def optimize_sfx():
  # go through every .wav file in assets and run
  # ffmpeg -i in.mov -map_metadata -1 -c:v copy -c:a copy -fflags +bitexact -flags:v +bitexact -flags:a +bitexact out.mov
  assets = fs_utils.project_root / 'assets'
  for wav_path in assets.glob('**/*.wav'):
    if wav_path.stem.endswith('_temp'):
      continue
    print(wav_path)
    temp_copy = wav_path.with_stem(wav_path.stem + '_temp')
    args = ['ffmpeg', '-i', str(wav_path), '-map_metadata', '-1', '-c:v', 'copy', '-c:a', 'copy', '-fflags', '+bitexact', '-flags:v', '+bitexact', '-flags:a', '+bitexact', str(temp_copy)]
    print('Running:', *args)
    subprocess.run(args, check=True)
    shutil.move(temp_copy, wav_path)


def hook_recipe(recipe: make.Recipe):
  recipe.add_step(optimize_sfx, [], [])
