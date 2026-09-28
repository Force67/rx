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
        # Matches `old` bare or spelled from the root (`rx::old`), never as the
        # tail of another qualified name.
        pattern = r"(?<![\w:])(rx::)?" + re.escape(old)
        if old[-1].isalnum() or old[-1] == "_":
          pattern += r"\b"
        symbols.append((re.compile(pattern), r"\1" + new))
      else:
        sys.exit("renames.txt: unknown kind %r" % kind)
  return includes, prefixes, symbols


def renamed(spelled, includes, prefixes, depth=0):
  """The latest path for an include, following the table through every move.

  An intermediate path may no longer exist (a header moved twice), so only the
  end of a chain has to exist in this rx checkout. Returns `spelled` unchanged
  when no chain ends on an rx header."""
  if depth > 8:
    return spelled
  if spelled in includes:
    return renamed(includes[spelled], includes, prefixes, depth + 1)
  for old, new in prefixes:
    if spelled.startswith(old):
      candidate = renamed(new + spelled[len(old):], includes, prefixes, depth + 1)
      if os.path.isfile(os.path.join(RX_ROOT, candidate)):
        return candidate
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
