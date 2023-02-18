'''Utilities for operating on filesystem.'''

from pathlib import Path

def ensure_dir(dir):
  Path(dir).mkdir(parents=True, exist_ok=True)

project_root = Path(__file__).resolve().parents[1]
project_name = Path(project_root).name.lower()
