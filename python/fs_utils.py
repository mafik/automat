'''Utilities for operating on filesystem.'''

from pathlib import Path
import tempfile

project_root = Path(__file__).resolve().parents[1]
project_name = Path(project_root).name.lower()

# Alternative location (only works on systemd-compatible systems)
# f'/run/user/{os.geteuid()}/{fs_utils.project_name}/'
project_tmp_dir = Path(tempfile.gettempdir()) / project_name
