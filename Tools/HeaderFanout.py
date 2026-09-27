"""
HeaderFanout.py - measures transitive #include fan-out for project headers.

Why: a header edit forces a rebuild of every .cpp that transitively includes it.
UBT's own dependency graph would give an exact answer but requires a build; this
walks quoted #include directives statically, which is enough to RANK headers by
fan-out and to prove a cut reduced it. It is not a substitute for the build itself
(see the -DisableUnity build in the PR) - a static walk can't see conditional
compilation (#if WITH_EDITOR) and will over-count on macro-guarded includes.

Usage:
    python Tools/HeaderFanout.py [--top N]

Resolves quoted includes against the Public/Private dirs of each module (the
same search path UBT gives a module: its own Public+Private, plus the Public
dir of every module, since Build.cs dependencies expose only Public headers to
others - approximated here as "all Public dirs" rather than parsing every
Build.cs's PublicDependencyModuleNames, which is a fair trade for a ranking tool).
"""
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MODULE_ROOTS = [
    # (module name, public dir or None, private dir or None)
    ("Airside", "Plugins/Airside/Source/Airside/Public", "Plugins/Airside/Source/Airside/Private"),
    ("AirsideEditor", "Plugins/Airside/Source/AirsideEditor/Public", "Plugins/Airside/Source/AirsideEditor/Private"),
    ("AirsideTests", None, "Plugins/Airside/Source/AirsideTests/Private"),
    ("AirportOps", "Plugins/AirportOps/Source/AirportOps/Public", "Plugins/AirportOps/Source/AirportOps/Private"),
    ("AirportOpsTests", None, "Plugins/AirportOps/Source/AirportOpsTests/Private"),
    ("AirportMgr", "Source/AirportMgr", "Source/AirportMgr"),  # flat module: one dir is both
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def abspath(rel):
    return os.path.normpath(os.path.join(ROOT, rel))


def build_search_roots():
    """All Public dirs (visible project-wide) plus each module's own Private dir
    (visible only to itself, but we don't gate that here - a ranking tool, not a linter)."""
    public_roots = []
    all_roots_by_module = {}
    for name, pub, priv in MODULE_ROOTS:
        mine = []
        if pub:
            p = abspath(pub)
            public_roots.append(p)
            mine.append(p)
        if priv and priv != pub:
            p = abspath(priv)
            mine.append(p)
        all_roots_by_module[name] = mine
    return public_roots, all_roots_by_module


def find_all_files():
    files = []
    for name, pub, priv in MODULE_ROOTS:
        dirs = set()
        if pub:
            dirs.add(abspath(pub))
        if priv:
            dirs.add(abspath(priv))
        for d in dirs:
            for dirpath, _, filenames in os.walk(d):
                for fn in filenames:
                    if fn.endswith(".h") or fn.endswith(".cpp"):
                        files.append((name, os.path.join(dirpath, fn)))
    return files


def module_of_file(path, all_roots_by_module):
    for name, roots in all_roots_by_module.items():
        for r in roots:
            if path.startswith(r + os.sep) or path == r:
                return name
    return None


def resolve_include(including_file, spelled, public_roots, own_module_roots):
    """Try, in order: sibling-relative, own module roots, then any public root."""
    candidates = []
    own_dir = os.path.dirname(including_file)
    candidates.append(os.path.normpath(os.path.join(own_dir, spelled)))
    for r in own_module_roots:
        candidates.append(os.path.normpath(os.path.join(r, spelled)))
    for r in public_roots:
        candidates.append(os.path.normpath(os.path.join(r, spelled)))
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def parse_includes(path):
    includes = []
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = INCLUDE_RE.match(line)
                if m:
                    includes.append(m.group(1))
    except OSError:
        pass
    return includes


def main():
    top_n = 15
    if "--top" in sys.argv:
        top_n = int(sys.argv[sys.argv.index("--top") + 1])

    public_roots, all_roots_by_module = build_search_roots()
    files = find_all_files()

    # Precompute each file's directly-resolved include set (as absolute header paths).
    direct_includes = {}
    cpp_files = []
    for mod, path in files:
        includes = parse_includes(path)
        own_roots = all_roots_by_module[mod]
        resolved = set()
        for spelled in includes:
            r = resolve_include(path, spelled, public_roots, own_roots)
            if r:
                resolved.add(r)
        direct_includes[path] = resolved
        if path.endswith(".cpp"):
            cpp_files.append(path)

    # Memoized transitive closure per header/cpp (headers only need computing once).
    memo = {}

    def transitive(path, stack):
        if path in memo:
            return memo[path]
        if path in stack:
            return set()  # include guard cycle guard (shouldn't happen with #pragma once, but be safe)
        stack.add(path)
        result = set()
        for inc in direct_includes.get(path, ()):
            result.add(inc)
            result |= transitive(inc, stack)
        stack.discard(path)
        memo[path] = result
        return result

    # fan_out[header] = set of .cpp files that transitively include it
    fan_out = defaultdict(set)
    for cpp in cpp_files:
        closure = transitive(cpp, set())
        for hdr in closure:
            fan_out[hdr].add(cpp)
        # A .cpp always "depends on itself" trivially but we only rank HEADERS.

    ranked = sorted(fan_out.items(), key=lambda kv: len(kv[1]), reverse=True)

    print(f"{len(cpp_files)} .cpp files scanned; {len(fan_out)} project headers reached transitively.\n")
    print(f"Top {top_n} headers by transitive .cpp fan-out:")
    print(f"{'count':>6}  header")
    for hdr, cpps in ranked[:top_n]:
        rel = os.path.relpath(hdr, ROOT).replace("\\", "/")
        print(f"{len(cpps):6d}  {rel}")

    return ranked


if __name__ == "__main__":
    main()
