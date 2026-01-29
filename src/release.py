#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import argparse
import json
import subprocess
import urllib
import urllib.parse
import sys, os

from dataclasses import dataclass
from pathlib import Path
from urllib import request
from urllib.error import HTTPError

if __name__ == '__main__':

  args = argparse.ArgumentParser(description='''Builds Automat's binaries for all platforms & uploads them to GitHub as a new release.

    This script hardcodes specific SSH hosts so is unlikely to run on any random machine.''')
  args.add_argument('--pull_and_restart', action='store_true',
    help='''Causes the current git repository to be first synchronized with the
    most recent GitHub change. Then `release.py` will restart itself. This makes
    it possible to pull in ''')
  args = args.parse_args()

  repo = Path(subprocess.check_output(['git', 'rev-parse', '--show-toplevel']).decode().strip())
  git_refs = repo / '.git' / 'refs'

  if args.pull_and_restart:
    # Report an error if there are uncommited changes
    uncommited_changes = bool(subprocess.run(['git', 'diff', '--quiet']).returncode)
    if uncommited_changes:
      raise Exception('Uncommited changes, please commit before building the release')

    # Fetch the most recent changes from GitHub
    subprocess.run(['git', 'fetch', 'origin', 'main'], check=True)

    # Report an error if the current branch is not main
    git_head_branch = subprocess.Popen(['git', 'rev-parse', '--abbrev-ref', 'HEAD'], stdout=subprocess.PIPE)
    head_branch = git_head_branch.stdout.read().decode().strip() # type: ignore
    if head_branch != 'main':
      raise Exception('Not on the main branch, please switch to main before building the release')

    # Attempt fast-forward to origin/HEAD
    subprocess.run(['git', 'merge', '--ff-only', 'origin/main'], check=True)

    returncode = subprocess.run(['python', sys.argv[0]], check=True).returncode

    sys.exit(returncode)

  # Report an error if the local has unpushed changes
  main_hash = (git_refs / 'heads' / 'main').read_text().strip()
  origin_sha = (git_refs / 'remotes' / 'origin' / 'main').read_text().strip()
  if main_hash != origin_sha:
    raise Exception('Local and origin main hashes are different, please push the changes before building the release')

  @dataclass
  class Slave:
    ssh_host: str
    repo_dir: str
    out_binary: str
    out_name: str

    def is_windows(self) -> bool:
      '''Check if this slave is a Windows machine based on repo_dir path.'''
      # Windows paths typically start with a drive letter like C:
      return len(self.repo_dir) >= 2 and self.repo_dir[1] == ':'

    def ssh(self, cmd: list[str], cwd: str | None = None) -> None:
      '''Execute a command on the remote slave via SSH.

      Args:
        cmd: Command and arguments to execute
        cwd: Working directory on the remote machine (optional)
      '''

      def shquote(s):
        if ' ' in s:
          return f'"{s}"'
        return s

      if cwd:
        cmd_str = ' '.join(shquote(arg) for arg in cmd)
        if self.is_windows():
          # On Windows, use cmd /c with cd /d for drive changes and && to chain commands
          # Convert forward slashes to backslashes for Windows
          win_cwd = cwd.replace('/', '\\')
          full_cmd = f'cd /d {win_cwd} && {cmd_str}'
          ssh_cmd = ['ssh', self.ssh_host, full_cmd]
        else:
          # On Unix, use cd && command with proper shell quoting
          full_cmd = f'cd {shquote(cwd)} && {cmd_str}'
          ssh_cmd = ['ssh', self.ssh_host, full_cmd]
      else:
        # Without cwd, just pass the command directly
        # SSH will execute it via the remote shell
        ssh_cmd = ['ssh', self.ssh_host] + cmd

      print(f'[{self.ssh_host}] Running: {" ".join(cmd)}')
      result = subprocess.run(ssh_cmd, check=False)
      if result.returncode != 0:
        raise Exception(f'SSH command failed on {self.ssh_host} with return code {result.returncode}')

    def scp(self, remote_path: str, local_path: Path) -> None:
      '''Copy a file from the remote slave to the local machine.

      Args:
        remote_path: Path to the file on the remote machine
        local_path: Local destination path
      '''
      # Ensure the local directory exists
      local_path.parent.mkdir(parents=True, exist_ok=True)

      scp_src = f'{self.ssh_host}:{remote_path}'
      print(f'[{self.ssh_host}] Copying {remote_path} -> {local_path}')
      result = subprocess.run(['scp', scp_src, str(local_path)], check=False)
      if result.returncode != 0:
        raise Exception(f'SCP failed from {self.ssh_host}:{remote_path} with return code {result.returncode}')

  slaves = [
    # maf's linux pc
    Slave(
      ssh_host='vr',
      repo_dir='~/Pulpit/automat-release',
      out_binary='automat',
      out_name='automat_linux_64bit'),
    # maf's windows pc
    Slave(
      ssh_host='user@wall-e',
      repo_dir='C:/Users/User/Desktop/automat-release',
      out_binary='automat.exe',
      out_name='automat_windows_64bit.exe'),
    # maf's laptop - backup in case that wall-e fails
    # Slave(
    #   ssh_host='nano',
    #   repo_dir='C:/Users/maf/automat-release',
    #   out_binary='automat.exe',
    #   out_name='automat_windows_64bit.exe'),
  ]

  release_assets = []
  for slave in slaves:
    out = Path('/tmp') / slave.out_name
    slave.ssh(['git', 'fetch', 'origin'], cwd=slave.repo_dir)
    slave.ssh(['git', 'checkout', main_hash], cwd=slave.repo_dir)
    slave.ssh(['python', 'run.py', 'link automat', '--variant=release'], cwd=slave.repo_dir)
    slave.scp(f'{slave.repo_dir}/build/release/{slave.out_binary}', out)
    release_assets.append(out)

  # Upload to github
  import datetime
  today = datetime.date.today()
  year, week, weekday = today.isocalendar()
  TAG = f'{year}-W{week:02d}' # ISO 8601

  github_token_path = Path(os.environ.get('CREDENTIALS_DIRECTORY', default='/etc/credstore')) / 'GITHUB_TOKEN'
  github_token = github_token_path.read_text().strip()

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
    headers['Authorization'] = f'token {github_token}'
    req =  request.Request(url, data=data, method=method, headers=headers)
    try:
      resp = request.urlopen(req)
    except HTTPError as e:
      raise Exception(f"Couldn't call GitHub API {path}: {e}")
    resp = resp.read()
    return json.loads(resp) if resp else None

  try:
    release = github_call('releases/tags/' + TAG)
    print('Release already exists...')

    tag_hash = git_refs / 'tags' / TAG
    if not tag_hash.exists():
      raise Exception(f"GitHub contains release {TAG} but local git tag couldn't be found at {tag_hash}.")
    tag_hash = tag_hash.read_text().strip()

    if main_hash != tag_hash:
      print('Local and remote tags are different, updating the tag...')
      subprocess.run(['git', 'tag', '-f', TAG, main_hash], check=True)
      subprocess.run(['git', 'push', 'origin', 'tag', TAG, '--force'], check=True)

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
  for file in release_assets:
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
