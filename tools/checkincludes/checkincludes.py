#!/usr/bin/env python3
"""Checks rx's includes against the target graph CMake writes (modules.json).

A quoted include must resolve to a file of the including target itself or of a
target it can see through its links: its direct links, plus what those export
through INTERFACE_LINK_LIBRARIES. On top of that come the layering rules from
docs/STRUCTURE.md. A plugin is any module under plugins/.

Existing violations live in baseline.txt next to this script. The check fails on
a violation that is not in the baseline, and on a baseline entry that no longer
occurs, so the baseline only ever shrinks. --update-baseline rewrites it.
"""

import json
import os
import re
import sys

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "baseline.txt")

# Modules that work on their own handles and must not know the entity world.
ENTITY_FREE = {"foundation", "events", "window", "gpu", "imgui_renderer", "ui", "asset", "audio", "net", "http", "rpc", "physics",
               "anim", "render", "render2d"}
ENTITY_WORLD = {"ecs", "scene", "script", "world", "edit", "devtools", "app"}
# Headers only the editor and tools may use; a shipping game never links them.
EDITOR_ONLY = {"rxe/edit/selection.h", "rxe/edit/undo.h", "rxe/world/world_bake.h"}
HEADER_EXTS = (".h", ".hpp", ".inl", ".def")
SOURCE_EXTS = (".cc", ".cpp", ".c") + HEADER_EXTS
CODE_DIRS = ("foundation", "rxe", "plugins", "runtime", "apps", "tools", "test", "examples")


def unwrap(item):
  """Returns (target name, link_only) for one raw CMake link item."""
  link_only = False
  while item.startswith("$<"):
    if item.startswith("$<LINK_ONLY:"):
      link_only = True
    elif not re.match(r"^\$<(BUILD_INTERFACE|LINK_ONLY):", item):
      # A conditional link ($<$<PLATFORM_ID:Linux>:pthread>): keep the payload.
      item = item.rsplit(":", 1)[-1]
      break
    item = item[item.index(":") + 1:]
    if item.endswith(">"):
      item = item[:-1]
  item = item.rstrip(">")
  if item.startswith("rx::"):
    item = "rx_" + item[4:]
  return item, link_only


def module_of(target):
  return target[3:] if target.startswith("rx_") else None


class Graph:
  def __init__(self, path):
    with open(path) as f:
      data = json.load(f)
    self.root = data["source_root"]
    self.targets = data["targets"]
    self.libraries = {t for t, v in self.targets.items() if v["type"] != "EXECUTABLE"}
    self.direct = {}
    self.exported = {}
    for name, t in self.targets.items():
      self.direct[name] = {n for n, _ in map(unwrap, t["link"]) if n in self.targets}
      self.exported[name] = {n for n, only in map(unwrap, t["interface_link"])
                             if n in self.targets and not only}
    self.owners = self._assign_owners()

  def visible(self, target):
    seen = {target}
    stack = list(self.direct[target])
    while stack:
      t = stack.pop()
      if t in seen:
        continue
      seen.add(t)
      stack.extend(self.exported[t])
    return seen

  def _assign_owners(self):
    """Maps every rx source and header to the targets it belongs to.

    A source belongs to the targets that compile it, libraries first (a test
    that compiles an engine .cc does not own it). A header belongs to whatever
    compiles its .cc twin, by the same rule; otherwise to the module named
    after the closest directory holding targets, or to all of that directory's
    targets when none is.
    """
    compiled = {}
    for name, t in self.targets.items():
      for src in t["sources"]:
        compiled.setdefault(src, set()).add(name)
    owners = {}
    for src, names in compiled.items():
      libs = names & self.libraries
      owners[src] = libs or names
    # Targets defined in the top-level CMakeLists (tests, tools) have the root
    # as their directory; they own only what they compile.
    by_dir = {}
    for name, t in self.targets.items():
      if t["source_dir"] != self.root:
        by_dir.setdefault(t["source_dir"], set()).add(name)
    for dirpath, _, filenames in (w for top in CODE_DIRS
                                  for w in os.walk(os.path.join(self.root, top))):
      for fn in filenames:
        path = os.path.join(dirpath, fn)
        if not fn.endswith(HEADER_EXTS) or path in owners:
          continue
        stem = os.path.splitext(path)[0]
        twins = set()
        for ext in (".cc", ".cpp", ".c"):
          twins |= compiled.get(stem + ext, set())
        if twins:
          owners[path] = (twins & self.libraries) or twins
          continue
        d = dirpath
        while d != self.root:
          names = by_dir.get(d)
          if names:
            named = {n for n in names if n == "rx_" + os.path.basename(d)}
            owners[path] = named or names
            break
          d = os.path.dirname(d)
    return owners

  def resolve(self, target, including, spelled):
    dirs = [os.path.dirname(including)]
    for inc in self.targets[target]["include_dirs"]:
      inc, _ = unwrap(inc)
      if not inc.startswith("$<"):
        dirs.append(inc)
    dirs.append(self.root)
    for d in dirs:
      path = os.path.normpath(os.path.join(d, spelled))
      if os.path.isfile(path):
        return path
    return None


def check(graph):
  rel = lambda p: os.path.relpath(p, graph.root)
  violations = {}  # key -> first path:line
  # The layering rules bind foundation, rxe and plugin modules. An app's own library (the viewer's
  # scene authoring) answers only to the link check, like the app.
  module_dirs = tuple(os.path.join(graph.root, d) + os.sep for d in ("foundation", "rxe", "plugins"))
  plugin_dir = os.path.join(graph.root, "plugins") + os.sep
  plugins = {module_of(t) for t, v in graph.targets.items()
             if t in graph.libraries and (v["source_dir"] + os.sep).startswith(plugin_dir)}
  def module_rules_of(target):
    if (target in graph.libraries and
        (graph.targets[target]["source_dir"] + os.sep).startswith(module_dirs)):
      return module_of(target)
    return None
  visible_of = {t: graph.visible(t) for t in graph.targets}
  for path, owners in sorted(graph.owners.items()):
    if not path.endswith(SOURCE_EXTS) or not os.path.isfile(path):
      continue
    # A header shared by several targets (no .cc twin) may include what any
    # of them can see.
    visible = set().union(*(visible_of[o] for o in owners))
    mods = {module_rules_of(o) for o in owners} - {None}
    with open(path, errors="replace") as f:
      lines = f.readlines()
    for lineno, line in enumerate(lines, 1):
      m = INCLUDE.match(line)
      if not m:
        continue
      dep = None
      for o in sorted(owners):
        dep = graph.resolve(o, path, m.group(1))
        if dep:
          break
      if dep is None or dep.startswith(os.path.join(graph.root, "third_party")):
        continue
      dep_owners = graph.owners.get(dep)
      if not dep_owners or owners & dep_owners:
        continue
      dep_mods = {module_of(o) for o in dep_owners if o in graph.libraries} - {None}
      found = []
      if not dep_owners & visible:
        found.append(("undeclared", "%s does not link %s" % (
            " or ".join(sorted(owners)), " or ".join(sorted(dep_owners)))))
      for mod in sorted(mods):
        if mod in ENTITY_FREE and dep_mods & ENTITY_WORLD:
          found.append(("entity-free", "%s may not know the entity world" % mod))
        if "app" in dep_mods:
          found.append(("host", "only apps may depend on the host"))
        if rel(dep) in EDITOR_ONLY:
          found.append(("editor-only", "a library may not use editor-only code"))
        if mod not in plugins and dep_mods & plugins:
          found.append(("plugin", "engine module %s may not depend on a plugin" % mod))
      for rule, why in found:
        key = "%s %s %s" % (rule, rel(path), rel(dep))
        violations.setdefault(key, "%s:%d: %s: includes %s (%s)" % (
            rel(path), lineno, rule, rel(dep), why))
  return violations


def main(argv):
  update = "--update-baseline" in argv
  args = [a for a in argv[1:] if not a.startswith("--")]
  if len(args) != 1:
    print("usage: checkincludes.py [--update-baseline] <build>/modules.json")
    return 2
  violations = check(Graph(args[0]))
  if update:
    with open(BASELINE, "w") as f:
      f.write("# Include violations that existed when the lint landed. Fix one,\n"
              "# then delete its line. Format: <rule> <including file> <included file>\n")
      for key in sorted(violations):
        f.write(key + "\n")
    print("baseline: %d entries" % len(violations))
    return 0
  baseline = set()
  if os.path.isfile(BASELINE):
    with open(BASELINE) as f:
      baseline = {l.strip() for l in f if l.strip() and not l.startswith("#")}
  new = sorted(k for k in violations if k not in baseline)
  stale = sorted(baseline - set(violations))
  for k in new:
    print(violations[k])
  for k in stale:
    print("baseline.txt: fixed, delete this line: %s" % k)
  print("checkincludes: %d violations (%d baselined), %d new, %d stale" % (
      len(violations), len(violations) - len(new), len(new), len(stale)))
  return 1 if new or stale else 0


if __name__ == "__main__":
  sys.exit(main(sys.argv))
