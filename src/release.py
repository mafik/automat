# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from functools import partial
import json
import shutil
import subprocess
import time
import urllib
from urllib import request
from urllib.error import HTTPError

import urllib.parse
from make import Popen
import fs_utils
import os
import datetime
import zipfile
from pathlib import Path

MAIN_HASH_FILE = fs_utils.project_root / '.git' / 'refs' / 'heads' / 'main'
RELEASE_DIR = fs_utils.build_dir / 'release'

CHANGELOG_FILE = RELEASE_DIR / 'CHANGELOG.md'
SUMMARY_FILE = RELEASE_DIR / 'SUMMARY.md'

WINDOWS_X86_64_RELEASE_FILE = RELEASE_DIR / 'automat_windows_64bit.exe'
LINUX_X86_64_RELEASE_FILE = RELEASE_DIR / 'automat_linux_64bit'

GITHUB_TOKEN_PATH = Path('/etc/credstore/GITHUB_TOKEN')
if 'GITHUB_TOKEN_PATH' in os.environ:
  GITHUB_TOKEN_PATH = Path(os.environ['GITHUB_TOKEN_PATH'])

today = datetime.date.today()
year, week, weekday = today.isocalendar()
TAG = f'{year}-W{week:02d}' # ISO 8601

def github_call(path, method='GET', data=None):
  if path.startswith('https://'):
    url = path
  else:
    url = f'https://api.github.com/repos/mafik/automat/{path}'
  if method == 'POST':
    data = json.dumps(data)
    data = data.encode('utf-8')
  elif method == 'GET' and data:
    url += '?' + urllib.parse.urlencode(data)
    data = None
  headers = {
    'X-GitHub-Api-Version': '2022-11-28',
  }
  if GITHUB_TOKEN_PATH.exists():
    github_token = GITHUB_TOKEN_PATH.read_text().strip()
    headers['Authorization'] = f'token {github_token}'
  req =  request.Request(url, data=data, method=method, headers=headers)
  try:
    resp = request.urlopen(req)
  except HTTPError as e:
    raise Exception(f"Couldn't call GitHub API {path}: {e}")
  resp = resp.read()
  return json.loads(resp) if resp else None

def artifact_download(url, path):
  print('Downloading', url, 'to', path)
  github_token = GITHUB_TOKEN_PATH.read_text().strip()
  zip_path = fs_utils.build_dir / 'download.zip'
  subprocess.run(['curl', '-s', '--header', f'Authorization: token {github_token}', '-L', '-o', zip_path, url], check=True)
  with zipfile.ZipFile(zip_path, 'r') as zip_ref:
    files = zip_ref.namelist()
    if len(files) != 1:
      raise Exception(f'Expected a single file in the zip, got {len(files)}')
    main_file = files[0]
    zip_ref.extractall(path=path.parent, members=[main_file])
    shutil.move(path.parent / main_file, path)
  
def build_release():
  # report an error if there are uncommited changes
  uncommited_changes = bool(subprocess.run(['git', 'diff', '--quiet']).returncode)
  if uncommited_changes:
    raise Exception('Uncommited changes, please commit before building the release')
  # fetch the most recent changes from GitHub
  subprocess.run(['git', 'fetch', 'origin', 'main'], check=True)
  # report an error if the current branch is not main
  git_head_branch = Popen(['git', 'rev-parse', '--abbrev-ref', 'HEAD'], stdout=subprocess.PIPE)
  head_branch = git_head_branch.stdout.read().decode().strip() # type: ignore
  if head_branch != 'main':
    raise Exception('Not on the main branch, please switch to main before building the release')
  # report an error if the local has unpushed changes
  main_hash = MAIN_HASH_FILE.read_text().strip()
  origin_sha = (fs_utils.project_root / '.git' / 'refs' / 'remotes' / 'origin' / 'main').read_text().strip()
  if main_hash != origin_sha:
    raise Exception('Local and origin main hashes are different, please push the changes before building the release')
  
  # At this point we can either call GitHub "Build" action or SSH into build servers to build the per-platform releases
  # For new we'll just use GitHub

  # wait for the run to finish & download artifacts
  while True:
    list_runs = github_call('actions/runs', 'GET', {
      'event': 'workflow_dispatch',
      'head_sha': main_hash,
    })
    if not list_runs:
      raise Exception("Didn't get any GitHub Action runs? This shouldn't happen")
    in_progress = []
    completed = []
    other = []
    for run in list_runs['workflow_runs']:
      if run['name'] != 'Build':
        continue
      if run['status'] == 'queued' or run['status'] == 'in_progress':
        in_progress.append(run)
      elif run['status'] == 'completed':
        completed.append(run)
      else:
        other.append(run)
    total = len(in_progress) + len(completed) + len(other)
    if completed:
      # print('Found', len(completed), 'completed runs:', json.dumps(completed, indent=4))
      windows_url = None
      linux_url = None
      artifacts = []
      for run in completed:
        artifacts = github_call(run['artifacts_url'])
        if artifacts is None:
          raise Exception(f"Couldn't get artifacts for run {run['id']}")
        # print('Artifacts:', json.dumps(artifacts, indent=4))
        for artifact in artifacts['artifacts']:
          if artifact['name'] == 'Windows Executable':
            windows_url = artifact['archive_download_url']
          elif artifact['name'] == 'Linux Executable':
            linux_url = artifact['archive_download_url']
      if windows_url and linux_url:
        # download the artifacts and save them to the release directory
        artifact_download(windows_url, WINDOWS_X86_64_RELEASE_FILE)
        artifact_download(linux_url, LINUX_X86_64_RELEASE_FILE)
      else:
        print('Couldn\'t find the artifacts in the completed runs. Debugging will be necessary')
        print('Runs:', json.dumps(completed, indent=4))
        print('Artifacts:', json.dumps(artifacts, indent=4))
      break
    elif total == 0:
      print('No runs found, dispatching a new one')
      github_call('actions/workflows/build.yaml/dispatches', 'POST', {
        'ref': 'main',
        'inputs': {},
      })
      time.sleep(30)
      continue
    elif in_progress:
      print('Run is already queued or in progress, waiting for it to finish')
      time.sleep(30)
      continue
    else:
      print("There are some runs but they're not in-progress")
      print(json.dumps(list_runs, indent=4))
      break


def upload_release():
  github_token = GITHUB_TOKEN_PATH.read_text().strip()
  try:
    release = github_call('releases/tags/' + TAG)
    print('Release already exists...')

    main_hash = MAIN_HASH_FILE.read_text().strip()
    tag_hash = fs_utils.project_root / '.git' / 'refs' / 'tags' / TAG
    if not tag_hash.exists():
      raise Exception(f"GitHub contains release {TAG} but local git tag couldn't be found at {tag_hash}.")
    tag_hash = tag_hash.read_text().strip()

    if main_hash != tag_hash:
      print('Local and remote tags are different, updating the tag...')
      subprocess.run(['git', 'tag', '-f', TAG, main_hash], check=True)
      subprocess.run(['git', 'push', 'origin', 'tag', TAG], check=True)

  except Exception as e:
    release = None

  if release is None:
    print('Creating a new release...')
    release = github_call('releases', 'POST', {
      'tag_name': TAG,
      'generate_release_notes': True,
    })

  if release is None:
    raise Exception("Couldn't get/create a new release")
  for asset in release['assets']:
    print('Deleting', asset['name'])
    github_call(f'releases/assets/{asset["id"]}', 'DELETE')
  # print('Release:', json.dumps(release, indent=4))

  release_id = release['id']
  for file in [WINDOWS_X86_64_RELEASE_FILE, LINUX_X86_64_RELEASE_FILE]:
    print('Uploading', file)
    args = ['curl',
            '-s', # silent
            '-L', # follow redirects
            '-X', 'POST',
            '-H', 'Accept: application/vnd.github+json',
            '-H', f'Authorization: token {github_token}',
            '-H', 'X-GitHub-Api-Version: 2022-11-28',
            '-H', 'Content-Type: application/octet-stream',
            f'https://uploads.github.com/repos/mafik/automat/releases/{release_id}/assets?name={file.name}',
            '--data-binary', f'@{file}']
    subprocess.run(args, stdout=subprocess.DEVNULL, check=True)
    print('Done!')

    


def hook_recipe(recipe):
  recipe.generated.add(str(WINDOWS_X86_64_RELEASE_FILE))
  recipe.generated.add(str(LINUX_X86_64_RELEASE_FILE))
  recipe.add_step(
    build_release,
    outputs=[WINDOWS_X86_64_RELEASE_FILE, LINUX_X86_64_RELEASE_FILE],
    inputs=[MAIN_HASH_FILE],
    desc = 'Building the release',
    shortcut='build release')
  
  recipe.add_step(
    upload_release,
    inputs=[WINDOWS_X86_64_RELEASE_FILE, LINUX_X86_64_RELEASE_FILE],
    outputs=[],
    desc = 'Uploading the release',
    shortcut='upload release')
