"""Sweeps the WHOLE fleet for per-asset materials that build_fleet_materials.py made
redundant. Headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

THE MECHANISM MOVED TO airside_import.sweep_orphan_materials ON 2026-09-21, and so did the
reasoning: why Interchange makes these, why material generation is left on anyway, why a
referenced material is kept, and why the delete is checked on disk rather than believed.

WHAT THIS FILE IS FOR NOW is the BREADTH. import_models.py sweeps only the folders it just
imported into, so that an import can never again leave its orphans behind - which is what
happened to plane7 in PR #225, where all seven reached main. This sweeps everything, which is
the right tool for tidying up after an import that predates that change, or after a model has
been deleted by hand.

NOTHING IS KEPT BY FOLDER ANY MORE. This file used to hold
KEEP_PREFIXES = [..., "/Game/Aircraft/PiperMeridian"], protecting the hand-authored
M_PiperMeridian and M_PiperGlass, which were built against real texture maps that the
flat-colour master cannot replace. plane7 replaced that model and PR #225 deleted both
materials with it, so the entry guarded nothing - a list that outlived the thing it named,
which is the failure CLAUDE.md calls out under "check where a list is CONSUMED". The shared
set at /Game/Materials/Fleet is still excluded, and that one is a default of the mechanism
rather than a fact about this script.

IF A HAND-AUTHORED MATERIAL EVER LIVES UNDER /Game/Aircraft AGAIN, it does not need a folder
in a keep list: assign it to something. The sweep deletes only what nothing references, so a
material that is worn is safe by construction, and one that is worn by nothing is
indistinguishable from Interchange's leavings.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note. Put it on before importing the shared mechanism.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import say, sweep_orphan_materials  # noqa: E402

SCAN = ["/Game/Aircraft", "/Game/Vehicles"]


def run():
    sweep_orphan_materials(SCAN)
    say("DONE")


run()
