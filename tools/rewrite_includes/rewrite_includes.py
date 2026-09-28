#!/usr/bin/env python3
"""Applies renames.txt (next to this script) to C++ sources: rewrites quoted
#include paths and renamed qualified symbols. Usage:

  rewrite_includes.py <dir or file>...

Prints each file it changed. Safe to run more than once.
"""

import os
import re
import sys

EXTS = (".h", ".hpp", ".inl", ".cc", ".cpp", ".c", ".def", ".mm")
SKIP_DIRS = {".git", "build", "third_party"}
# The rx checkout this script belongs to: a prefix rule only fires when the
# rewritten path exists here, so a project's own ui/ or net/ headers are left
# alone.
RX_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_renames():
  includes, prefixes, symbols = {}, [], []
  path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "renames.txt")
  with open(path) as f:
    for line in f:
      parts = line.split("#", 1)[0].split()
      if not parts:
        continue
      kind, old, new = parts
      if kind == "include":
        includes[old] = new
      elif kind == "prefix":
        prefixes.append((old, new))
      elif kind == "symbol":
        symbols.append((re.compile(r"(?<![\w:])" + re.escape(old) + r"\b"), new))
      else:
        sys.exit("renames.txt: unknown kind %r" % kind)
  return includes, prefixes, symbols


def renamed(spelled, includes, prefixes):
  # Chained: an include renamed in phase 2 may have moved again since.
  for _ in range(8):
    if spelled in includes:
      spelled = includes[spelled]
      continue
    for old, new in prefixes:
      if spelled.startswith(old):
        candidate = new + spelled[len(old):]
        if os.path.isfile(os.path.join(RX_ROOT, candidate)):
          spelled = candidate
          break
    else:
      return spelled
  return spelled


def rewrite(path, includes, prefixes, symbols):
  with open(path, errors="surrogateescape") as f:
    text = f.read()
  out = re.sub(r'(#\s*include\s+")([^"]+)(")',
               lambda m: m.group(1) + renamed(m.group(2), includes, prefixes) + m.group(3),
               text)
  for pattern, new in symbols:
    out = pattern.sub(new, out)
  if out != text:
    with open(path, "w", errors="surrogateescape") as f:
      f.write(out)
    print(path)


def main(argv):
  if len(argv) < 2:
    print(__doc__)
    return 2
  includes, prefixes, symbols = load_renames()
  for root in argv[1:]:
    if os.path.isfile(root):
      rewrite(root, includes, prefixes, symbols)
      continue
    for dirpath, dirnames, filenames in os.walk(root):
      dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
      for fn in filenames:
        if fn.endswith(EXTS):
          rewrite(os.path.join(dirpath, fn), includes, prefixes, symbols)
  return 0


if __name__ == "__main__":
  sys.exit(main(sys.argv))
