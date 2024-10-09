# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
'''Rules for downloading the Ninja build system'''

import shutil

BIN = shutil.which('ninja')

print('Ninja found at', BIN)

if not BIN:
  print('Ninja not found - adding recipe to download it.')

  import build, sys, fs_utils
  if sys.platform == 'win32':
    ZIP_NAME = 'ninja-win.zip'
  elif sys.platform == 'darwin':
    ZIP_NAME = 'ninja-mac.zip'
  elif sys.platform == 'linux':
    ZIP_NAME = 'ninja-linux.zip'
  else:
    raise NotImplementedError(f'TODO: platform {sys.platform} is missing in ninja.py')

  DOWNLOAD_URL = 'https://github.com/ninja-build/ninja/releases/latest/download/' + ZIP_NAME
  ZIP_PATH = fs_utils.build_dir / ZIP_NAME
  BIN = build.base.PREFIX() / 'bin' / 'ninja'

  def download(url, out_path):
    import urllib.request, shutil
    with urllib.request.urlopen(url) as response:
      with open(out_path, 'wb') as out_file:
        shutil.copyfileobj(response, out_file)

  def download_step(url, out_path):
    import functools
    return functools.partial(download, url, out_path)

  def unzip(zip_path, out_dir):
    import zipfile
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
      zip_ref.extractall(out_dir)

  def unzip_step(zip_path, out_dir):
    import functools
    return functools.partial(unzip, zip_path, out_dir)

  def hook_recipe(recipe):
    recipe.add_step(
        download_step(DOWNLOAD_URL, ZIP_PATH),
        outputs=[ZIP_PATH],
        inputs=[],
        desc='Downloading Ninja',
        shortcut='download ninja')

    recipe.add_step(
        unzip_step(ZIP_PATH, BIN.parent),
        outputs=[BIN],
        inputs=[ZIP_PATH],
        desc='Unzipping Ninja',
        shortcut='unzip ninja')
else:
  def hook_recipe(recipe):
    pass
