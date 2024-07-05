'''Utilities for operating on filesystem.'''

from pathlib import Path

project_root = Path(__file__).resolve().parents[1]
project_name = Path(project_root).name.lower()


def relative_to_root(path: Path) -> Path:
    return path.resolve().relative_to(project_root)


build_dir = relative_to_root(project_root / 'build')
src_dir = project_root / 'src'
generated_dir = relative_to_root(build_dir / 'generated')
third_party_dir = project_root / 'third_party'
