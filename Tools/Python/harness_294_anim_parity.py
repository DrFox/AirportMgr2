"""Issue #294's refactor proof - kept alongside the PR, not part of the pipeline. Safe to
delete once main has moved past PR #342 and nobody needs to re-run this comparison.

  python Tools/Python/harness_294_anim_parity.py [--ref origin/main]

Proves the DERIVATION build_aircraft_anim.py/Tools/wire_plane_anim.py introduced - driven-bone
membership, chain-order validity, RAKED, and the wiring multiplier - agrees with the 26
retired per-plane scripts, against all fourteen rigs' real .glb/rig_map.json files. It does
NOT need a running editor: `_install_fake_unreal()` below stubs `unreal` well enough to
import airside_anim.py, build_aircraft_type.py and every aircraft/<key>.py, because none of
their MODULE-level code (only their function bodies) touches a real UAsset. It does NOT prove
resolve_axis's own geometry (component/sign), which is unchanged code this refactor did not
touch and which genuinely needs a live skeletal mesh - see PR #342's Runtime section for what
still needs the editor.

REPRODUCIBLE FROM THE MERGED BRANCH: the 26 old scripts this diffs against are gone from the
working tree by the time this file is useful to a reviewer, so their content is read with
`git show <ref>:<path>` (default `origin/main`, before PR #342 merges - pass `--ref <sha>` to
pin an exact commit once it has) rather than off disk.

For each of the fourteen keys:
  1. AST-extracts the OLD build_plane<N>_anim.py's DRIVEN_BONES and RAKED.
  2. Calls the NEW airside_anim.driven_bone_plan(spec.source, excluded=spec.excluded) and
     compares its bone SET against the old DRIVEN_BONES (order is expected to differ - see
     driven_bone_plan's own docstring - and is checked separately, for VALIDITY, against the
     .glb's own parent/child hierarchy).
  3. Compares the new aircraft/<key>.py spec's `raked` field against the old RAKED tuple -
     identical on all fourteen, including plane14 (the fix was its say() line, not its data).
  4. For the twelve wired keys, AST-extracts the OLD Tools/wire_plane<N>_anim.py's PLAN
     multiplier column and compares it, bone by bone, against
     airside_anim.anim_multiplier_for(bone, variable, spec.anim_multiplier).
"""
import argparse
import ast
import json
import os
import struct
import subprocess
import sys
import tempfile
import types

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KEYS = ["plane%d" % n for n in range(1, 15)]
WIRED_KEYS = ["plane%d" % n for n in range(3, 15)]


def _install_fake_unreal():
    """A trivial stub for `unreal`, good enough to IMPORT airside_anim.py,
    build_aircraft_type.py and aircraft/<key>.py without a running editor - see the module
    docstring for why that is safe (module-level code never CALLS unreal, only defines
    functions/dataclasses against its names). Any accidental call raises loudly rather than
    silently returning None."""
    def _missing(*_a, **_k):
        raise RuntimeError("harness_294_anim_parity: no real editor here - this call should "
                            "never have been reached (only DERIVATION is being checked)")
    stub = types.ModuleType("unreal")
    stub.__getattr__ = lambda name: _missing
    sys.modules["unreal"] = stub


def _old_file(ref, relpath, tmpdir):
    """<relpath>'s content AT `ref`, written into tmpdir and returned as a local path -
    `git show` refuses to read a deleted blob off the working tree, and ast.parse wants a
    real file for its error messages to name."""
    text = subprocess.check_output(["git", "show", "%s:%s" % (ref, relpath)], cwd=REPO,
                                    text=True)
    local = os.path.join(tmpdir, os.path.basename(relpath))
    with open(local, "w") as handle:
        handle.write(text)
    return local


def _literal_assign(tree, name):
    """The literal value of a module-level `name = <literal>` assignment, or None."""
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name) and node.targets[0].id == name:
            return ast.literal_eval(node.value)
    return None


def old_build_facts(ref, key, tmpdir):
    path = _old_file(ref, "Tools/Python/build_%s_anim.py" % key, tmpdir)
    with open(path) as handle:
        source = handle.read()
    tree = ast.parse(source, filename=path)
    driven = _literal_assign(tree, "DRIVEN_BONES")
    raked = _literal_assign(tree, "RAKED")
    if "ONE BONE IS RAKED ON PURPOSE" in source:
        say_line = "ONE BONE IS RAKED ON PURPOSE"
    elif "NO BONE IS RAKED" in source:
        say_line = "NO BONE IS RAKED"
    else:
        say_line = None
    return driven, raked, say_line


def _plane8_ratios(ref, tmpdir):
    """A literal re-read of plane8/scripts/rig_map.json (a real, still-live file in the
    models repo - not under git in THIS repo), replicating the OLD
    Tools/wire_plane8_anim.py's ratios() by hand: PLAN's multiplier column there is
    `GEAR["gear_wing_L"]`/`TRUCK[...]` subscripts, not literals, so ast.literal_eval cannot
    resolve them; this gives old_wire_plan() the same two dicts to look them up in."""
    rig_map = r"C:\repos\AirportMgr2Models\plane8\scripts\rig_map.json"
    with open(rig_map) as handle:
        rig = json.load(handle)
    gear = {bone: -1.0 * deg / 90.0 for bone, deg in rig["retract_deg"].items()}
    truck = {bone: (-1.0 * deg / 54.72) or 0.0 for bone, deg in rig["truck_tilt_deg"].items()}
    return {"GEAR": gear, "TRUCK": truck}


def old_wire_plan(ref, key, tmpdir):
    """[(bone, variable, multiplier)] from the OLD Tools/wire_<key>_anim.py at `ref`, or None
    if this key never had one (plane1, plane2)."""
    try:
        path = _old_file(ref, "Tools/wire_%s_anim.py" % key, tmpdir)
    except subprocess.CalledProcessError:
        return None
    with open(path) as handle:
        tree = ast.parse(handle.read(), filename=path)
    namespaces = _plane8_ratios(ref, tmpdir) if key == "plane8" else {}

    def eval_mul(node):
        if isinstance(node, ast.Constant) and node.value is None:
            return None
        if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Name):
            table = namespaces[node.value.id]
            return table[ast.literal_eval(node.slice)]
        return ast.literal_eval(node)

    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name) and node.targets[0].id == "PLAN":
            rows = []
            for elt in node.value.elts:
                bone, variable, mul = elt.elts
                rows.append((ast.literal_eval(bone), ast.literal_eval(variable), eval_mul(mul)))
            return rows
    return None


def _parent_map(source):
    with open(source, "rb") as handle:
        handle.read(12)
        length = struct.unpack("<I4s", handle.read(8))[0]
        doc = json.loads(handle.read(length).decode("utf-8"))
    nodes = doc.get("nodes", [])
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i
    name_to_idx = {n.get("name"): i for i, n in enumerate(nodes)}
    idx_to_name = {i: n.get("name") for i, n in enumerate(nodes)}
    return parent, name_to_idx, idx_to_name


def chain_order_problems(order, source):
    """Every (bone, ancestor) pair in `order` where the ancestor comes BEFORE the bone it is
    an ancestor of - a violation of "a bone goes after every bone it is the parent of"."""
    parent, name_to_idx, idx_to_name = _parent_map(source)
    pos = {name: i for i, name in enumerate(order)}

    def ancestors(name):
        idx = name_to_idx.get(name)
        out = []
        while idx in parent:
            idx = parent[idx]
            out.append(idx_to_name.get(idx))
        return out

    return [(name, anc) for name in order for anc in ancestors(name)
            if anc in pos and pos[anc] < pos[name]]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", default="origin/main",
                         help="git ref the 26 retired scripts are read from (default: "
                              "origin/main, before PR #342 merges)")
    args = parser.parse_args(argv[1:])

    _install_fake_unreal()
    sys.path.insert(0, os.path.join(REPO, "Tools", "Python"))
    from airside_anim import anim_multiplier_for, driven_bone_plan, raked_line  # noqa: E402
    from build_aircraft_type import spec_for  # noqa: E402

    all_ok = True
    with tempfile.TemporaryDirectory(prefix="harness_294_") as tmpdir:
        print("=" * 100)
        print("DRIVEN-BONE-SET / CHAIN-ORDER / RAKED comparison, all 14 keys (ref=%s)" % args.ref)
        print("=" * 100)
        for key in KEYS:
            spec = spec_for(key)
            old_driven, old_raked, old_say = old_build_facts(args.ref, key, tmpdir)

            if not spec.wire_script:
                print("%-8s hand-wired (wire_script=False) - no DRIVEN_BONES derivation to check"
                      % key)
                continue

            new_matched = driven_bone_plan(spec.source, excluded=spec.excluded)
            new_bones = [b for b, _ in new_matched]

            old_set = set(old_driven)
            new_set = set(new_bones)
            set_ok = old_set == new_set
            problems = chain_order_problems(new_bones, spec.source)
            old_raked = old_raked or ()   # plane5/plane7: resolve_axis() with no `raked=` at
                                           # all - equivalent to the default ().
            raked_ok = tuple(spec.raked) == tuple(old_raked)
            new_say = raked_line(spec.raked)

            all_ok = all_ok and set_ok and not problems and raked_ok
            print("%-8s members=%s chain_problems=%s raked_agrees=%s say(old=%r new=%r)"
                  % (key, "SAME" if set_ok else "DIFF", problems, raked_ok, old_say, new_say))
            if not set_ok:
                print("    !! %s: old-only=%s new-only=%s"
                      % (key, old_set - new_set, new_set - old_set))
            if problems:
                print("    !! %s: chain order problems: %s" % (key, problems))
            if not raked_ok:
                print("    !! %s: raked old=%s new=%s" % (key, old_raked, tuple(spec.raked)))

        print()
        print("=" * 100)
        print("WIRING MULTIPLIER comparison, twelve wired keys")
        print("=" * 100)
        for key in WIRED_KEYS:
            spec = spec_for(key)
            old_plan = old_wire_plan(args.ref, key, tmpdir)
            if old_plan is None:
                print("%-8s !! no old wire_%s_anim.py found at %s" % (key, key, args.ref))
                all_ok = False
                continue
            matched = dict(driven_bone_plan(spec.source, excluded=spec.excluded))
            mismatches = []
            for bone, old_variable, old_mul in old_plan:
                new_variable = matched.get(bone)
                new_mul = anim_multiplier_for(bone, new_variable, spec.anim_multiplier)
                mul_agrees = (old_mul is None and new_mul is None) or (
                    old_mul is not None and new_mul is not None
                    and abs(old_mul - new_mul) < 1e-6)
                if new_variable != old_variable or not mul_agrees:
                    mismatches.append((bone, (old_variable, old_mul), (new_variable, new_mul)))
            print("%-8s rows=%d mismatches=%d" % (key, len(old_plan), len(mismatches)))
            if mismatches:
                all_ok = False
                for bone, old, new in mismatches:
                    print("    !! %s: old=%s new=%s" % (bone, old, new))

    print()
    print("RESULT:", "ALL OK" if all_ok else "MISMATCHES FOUND (see !! lines above)")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
