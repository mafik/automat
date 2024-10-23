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

MAIN_HASH_FILE = fs_utils.project_root / '.git' / 'refs' / 'heads' / 'main'
RELEASE_DIR = fs_utils.build_dir / 'release'

CHANGELOG_FILE = RELEASE_DIR / 'CHANGELOG.md'
SUMMARY_FILE = RELEASE_DIR / 'SUMMARY.md'

WINDOWS_X86_64_RELEASE_FILE = RELEASE_DIR / 'automat_windows_64bit.exe'
LINUX_X86_64_RELEASE_FILE = RELEASE_DIR / 'automat_linux_64bit'

today = datetime.date.today()
year = today.year
week = today.isocalendar()[1]
TAG = f'v{year}.{week}'

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
  if 'GITHUB_TOKEN' in os.environ:
    headers['Authorization'] = f'token {os.environ["GITHUB_TOKEN"]}'
  req =  request.Request(url, data=data, method=method, headers=headers)
  try:
    resp = request.urlopen(req)
  except HTTPError as e:
    raise Exception(f"Couldn't call GitHub API {path}: {e}")
  resp = resp.read()
  return json.loads(resp) if resp else None

def artifact_download(url, path):
  print('Downloading', url, 'to', path)
  zip_path = fs_utils.build_dir / 'download.zip'
  subprocess.run(['curl', '-s', '--header', f'Authorization: token {os.environ['GITHUB_TOKEN']}', '-L', '-o', zip_path, url], check=True)
  with zipfile.ZipFile(zip_path, 'r') as zip_ref:
    files = zip_ref.namelist()
    if len(files) != 1:
      raise Exception(f'Expected a single file in the zip, got {len(files)}')
    main_file = files[0]
    zip_ref.extractall(path=path.parent, members=[main_file])
    shutil.move(path.parent / main_file, path)

  
  # print('Downloading', url, 'to', path)
  # req =  request.Request(url)
  # if 'GITHUB_TOKEN' in os.environ:
  #   req.add_header('Authorization', f'token {os.environ["GITHUB_TOKEN"]}')
  # req.add_header('X-GitHub-Api-Version', '2022-11-28')
  # with request.urlopen(req) as resp:
  #   path.write_bytes(resp.read())
  
def build_release():
  # report an error if there are uncommited changes
  uncommited_changes = bool(subprocess.run(['git', 'diff', '--quiet']))
  if uncommited_changes:
    raise Exception('Uncommited changes, please commit before building the release')
  # report an error if the current branch is not main
  git_head_branch = Popen(['git', 'rev-parse', '--abbrev-ref', 'HEAD'], stdout=subprocess.PIPE)
  head_branch = git_head_branch.stdout.read().decode().strip() # type: ignore
  if head_branch != 'main':
    raise Exception('Not on the main branch, please switch to main before building the release')
  # report an error if the local has unpushed changes
  local_sha = MAIN_HASH_FILE.read_text().strip()
  origin_sha = (fs_utils.project_root / '.git' / 'refs' / 'remotes' / 'origin' / 'main').read_text().strip()
  if local_sha != origin_sha:
    raise Exception('Local and origin main hashes are different, please push the changes before building the release')
  
  # At this point we can either call GitHub "Build" action or SSH into build servers to build the per-platform releases
  # For new we'll just use GitHub
  
  # call workflow_dispatch on the "Build" GitHub action using the GitHub API
  main_hash = MAIN_HASH_FILE.read_text().strip()

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
      # TODO: make sure this works
      post_dispatch = github_call('actions/workflows/Build/dispatches', 'POST', {
        'ref': '#' + main_hash,
        'inputs': {
          'ref': main_hash,
          'release_name': TAG,
        }
      })
      time.sleep(10)
      continue
    elif in_progress:
      print('Run is already queued or in progress, waiting for it to finish')
      time.sleep(10)
      continue
    else:
      print("There are some runs but they're not in-progress")
      print(json.dumps(list_runs, indent=4))
      break


def upload_release():
  try:
    release = github_call('releases/tags/' + TAG)
    print('Release already exists...')
  except Exception as e:
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
            '-H', f'Authorization: token {os.environ["GITHUB_TOKEN"]}',
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
