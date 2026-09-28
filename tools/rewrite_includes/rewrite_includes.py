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


def load_renames():
  includes, symbols = {}, []
  path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "renames.txt")
  with open(path) as f:
    for line in f:
      parts = line.split("#", 1)[0].split()
      if not parts:
        continue
      kind, old, new = parts
      if kind == "include":
        includes[old] = new
      elif kind == "symbol":
        symbols.append((re.compile(r"(?<![\w:])" + re.escape(old) + r"\b"), new))
      else:
        sys.exit("renames.txt: unknown kind %r" % kind)
  return includes, symbols


def rewrite(path, includes, symbols):
  with open(path, errors="surrogateescape") as f:
    text = f.read()
  out = re.sub(r'(#\s*include\s+")([^"]+)(")',
               lambda m: m.group(1) + includes.get(m.group(2), m.group(2)) + m.group(3),
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
  includes, symbols = load_renames()
  for root in argv[1:]:
    if os.path.isfile(root):
      rewrite(root, includes, symbols)
      continue
    for dirpath, dirnames, filenames in os.walk(root):
      dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
      for fn in filenames:
        if fn.endswith(EXTS):
          rewrite(os.path.join(dirpath, fn), includes, symbols)
  return 0


if __name__ == "__main__":
  sys.exit(main(sys.argv))
