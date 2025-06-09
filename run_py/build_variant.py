from typing import Callable
import fs_utils
from pathlib import Path
from sys import platform

index = dict()

# Change this to set the current build variant
current = None
name = None
name_lower = None
BASE = None
PREFIX = None

'''Base class for build variants.'''
class BuildVariant:
  name: str
  name_lower: str
  BASE: Path
  PREFIX: Path
  base_variant: 'BuildVariant'

  def __init__(self, name:str, base_variant: 'BuildVariant' = None):
    self.name = name
    self.name_lower = name.lower()
    self.BASE = fs_utils.build_dir / self.name
    self.PREFIX = self.BASE / 'PREFIX'
    self.base_variant = base_variant
    index[self.name_lower] = self
  
  def visit_base_variants(self, cb:Callable[['BuildVariant'], None]):
    cb(self)
    if self.base_variant:
      self.base_variant.visit_base_variants(cb)

  def set_as_current(self):
    global current, name, name_lower, BASE, PREFIX
    current = self
    name = self.name
    name_lower = self.name_lower
    BASE = self.BASE
    PREFIX = self.PREFIX

  def __bool__(self):
    return self == current

Release = BuildVariant('Release')
Fast = BuildVariant('Fast')
Debug = BuildVariant('Debug')
if platform == 'linux':
  ASan = BuildVariant('ASan', Debug)
  TSan = BuildVariant('TSan', Debug)
  UBSan = BuildVariant('UBSan', Debug)
