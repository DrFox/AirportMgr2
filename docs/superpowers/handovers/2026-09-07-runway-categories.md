# Handover: runway categories and markings

Paste everything below the line into a fresh Claude Code session in `C:\repos\AirportMgr2`.

---

You are picking up AirportMgr (UE 5.8.2, C++) to build **runway categories and markings**:
a runway states its surface and approach class, an aircraft states what it needs, admission
compares them, and the runway is painted with the ICAO marking set. Read `CLAUDE.md` first;
it is the project's rules and is not optional. Then read, in this order:

1. `docs/superpowers/specs/2026-09-07-runway-categories-and-markings-design.md` — the spec,
   approved in conversation on 2026-09-07 with two assumptions the user accepted by saying
   "write the plan with your assumptions": reinforced looks like concrete and differs only in
   the panel and in what it admits; a precision runway too short for its touchdown-zone
   stripes paints what fits. Displaced thresholds and declared distances are OUT by decision.
2. `docs/superpowers/plans/2026-09-07-runway-categories-and-markings.md` — the plan, seven
   tasks, red-then-green each. Execute it with the subagent-driven-development skill or
   inline; the user has accepted both before.
3. `docs/superpowers/specs/2026-09-06-runway-exit-arcs-design.md` §3–§12 — how a runway
   junction is built now: symmetric exit arcs, the flare fillet, the holding position on the
   taxiway end. The marking builder must not touch any of it; it paints a separate mesh.
4. `Plugins/Airside/Source/Airside/Private/Build/HoldingPositionMarkingBuilder.cpp` — the
   pattern the runway marking builder copies: quads into `FRoadMeshBuffers`, UV1 = 0 so the
   road material paints them as marking, winding MEASURED by signed area (the first cut faced
   down on 112 of 112 normals), a census log line.

## State of the tree (all merged to `main`, 2026-09-07)

| PR | What |
|---|---|
| #54 | M2 ground traffic: occupancy, holding at bars, deadlock resolver, arrivals and departures on the strip |
| #55 | Runway exit arcs: `URoadProfile::ExitLength`, the taxiway end set back, the landing hands its speed to the taxi |
| #56 | Holding positions: `EHoldingPositionKind`, runway ones derived at every taxiway end, intermediate ones placed with key 8, painted |
| #57 | Exit-arc fixes: stub set-back never on the slab; arcs symmetric; `DeparturePlanner` (intersection departures, backtrack fallback); `RunwayExtentAt` admits any on-strip point |
| #58 | The flare: `ExitGeometry` (Build/) decides exit geometry once for solver and builder; `FJunctionArm::FilletRadiusToNext`; the holding-position floor is slab clearance |

`feature/runway-categories` exists with the spec and plan committed; nothing else. 133 tests,
0 failed, 0 crashed on main. `UE_LOG` count in the Airside module: 100.
`Content/Maps/M_Starter.umap` is modified in the working tree - the user's level, never
commit it. `Airside.Probe.StarterMapRoutes` loads it headlessly and logs what it finds; use it.

## Facts that will save you a round trip

- **Builds need the editor closed.** Check `tasklist | grep -i unrealeditor`. If the latest
  `Saved/Logs/AirportMgr*.log` has `Road building ready` and no `Road Build mode entered`,
  the session was PIE-only and the level was saved before it opened (compare the umap mtime
  with `Log file open`), and you may close it with `CloseMainWindow()` - say so. Otherwise ask.
  There can be TWO editor logs (`AirportMgr.log`, `AirportMgr_2.log`); read the newest.
- **Do NOT call `CompileLiveCoding` over MCP**: it crashed the editor (`UObjectHash.cpp(650)`).
- **Compare the DLL mtime with your commit before reading any PIE report.** A worktree build
  does not update the main checkout's binaries; the user tests whatever is in
  `Plugins/Airside/Binaries/Win64/UnrealEditor-Airside.dll`.
- **Long Python heredocs in Bash fail** in this environment with "unexpected EOF". Write the
  script to the scratchpad with the Write tool and run it. Anchors must tolerate blank lines
  the sources carry and `grep -v "^\s*$"` hid.
- **The runway is recognised by profile** (`bContinuousThroughJunctions`), never by a flag on
  the segment. `RunwayChain(Seed)` expands one segment to the strip; `RunwayExtentAt(Near,...)`
  gives threshold (nearest), direction, length. Runway profile assets are `DA_Runway_18m..60m`
  in `Content/`; a runway with a non-asset profile reloads as a TAXIWAY (the null-profile
  fallback is the taxiway profile), which is why the facts go on the segment, not on new assets.
- **Materials**: `M_RoadSurface` paints its centreline from UV1.X (`CentrelineWidth`,
  `MarkingSharpness`, `MarkingColor` parameters); a quad with UV1 = 0 is painted solid
  `MarkingColor` - the marking trick. `URoadMaterialSet` maps band slot names to materials;
  `FRoadProfileBands::FromProfile(Profile, Materials)` resolves each band's slot id and logs
  unresolved ones. Authoring a material asset needs `Tools/Python/*.py` with the editor closed;
  a runtime `UMaterialInstanceDynamic` (see `GhostMID` in `RoadSurfacePresenter.cpp`) needs no
  asset - use it for the white marking colour.
- **`FRunwayTool::NextWidth` has no caller.** Width cycling was never wired to a key. Task 6
  adds `IBuildTool::OnReselect` for it.
- **The designator**: `RunwayDesignator::Designate(Direction)`, `Reciprocal`, `ToText`,
  `ToPairText` in `Solve/RunwayDesignator.h`.
- **Field lengths vs rolls**: `FTakeoffRun::RequiredRoll` and
  `FLandingRun::RequiredLandingDistance * LandingMargin (1.25)` are the physics; the Piper's
  are ~23000 and ~37000 uu. The published field lengths are admission figures and must never be
  shorter (Task 3's test).
- **Winding**: assert on engine-computed normals (`FMeshNormals::QuickComputeVertexNormals`,
  Z > 0), never on 2D area by hand. Memory: `unreal-triangle-winding-convention`.
- **The test tree drops a bare-named test once a dotted child exists** - name leaves distinctly
  (`Airside.Build.RunwayMarkings.Precision45`, never `Airside.Build.RunwayMarkings` alongside).
- `Run-AirsideTests.ps1` runs `Check-Architecture.ps1` first; a doc comment followed by another
  doc comment (no declaration between) fails it.

## Open items you may meet (not this scope, do not start them uninvited)

- Displaced thresholds, stopways, declared distances per end (spec §1; a later spec).
- Lighting, weather, minima (M5). The approach class refuses only aircraft that declare a need.
- The road/taxiway profile split and a vehicle road tool (M4). Stands' vehicle anchors log
  "joins nothing" warnings until then - expected.
- M3, the first fifteen minutes: offers, flight board, sequencer (design it against holding
  positions and `DeparturePlanner`), allocator, jobs, ledger, HUD. After this piece.

## User preferences (from memory; honour them)

Extremely concise replies; sacrifice grammar for concision. OO veteran new to Unreal: skip OO
basics, explain UE machinery. Name the pattern, justify every deviation UE forces. Comments say
WHY and which alternative was rejected. Tests measure, with a named reason. Build what they can
click first and say what foundation you skipped. A fix reply ends with the verification step:
what to run, where to look, what confirms it. Never assert what the user is doing - measure it
from the log. When they say something does not work, that is an observation: read the log,
instrument the boundary, then reason. Feature branches `feature/*`, PRs to `main` via `gh`,
they say "merge" when they are happy; no `Co-Authored-By` trailer. Design decisions are
revisable for reasons, recorded where the old decision was.
