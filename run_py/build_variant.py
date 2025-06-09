from typing import Callable
import fs_utils
from pathlib import Path
from sys import platform

index = dict()

# Change this to set the current build variant
current = None
name = None
BASE = None
PREFIX = None

'''Base class for build variants.'''
class BuildVariant:
  name: str
  BASE: Path
  PREFIX: Path
  base_variant: 'BuildVariant'

  def __init__(self, name:str, base_variant: 'BuildVariant' = None):
    self.name = name
    self.BASE = fs_utils.build_dir / self.name
    self.PREFIX = self.BASE / 'PREFIX'
    self.base_variant = base_variant
    index[self.name] = self
  
  def visit_base_variants(self, cb:Callable[['BuildVariant'], None]):
    cb(self)
    if self.base_variant:
      self.base_variant.visit_base_variants(cb)

  def set_as_current(self):
    global current, name, BASE, PREFIX
    current = self
    name = self.name
    BASE = self.BASE
    PREFIX = self.PREFIX

  def __bool__(self):
    return self == current

release = BuildVariant('release')
fast = BuildVariant('fast')
debug = BuildVariant('debug')
if platform == 'linux':
  asan = BuildVariant('asan', debug)
  tsan = BuildVariant('tsan', debug)
  ubsan = BuildVariant('ubsan', debug)
