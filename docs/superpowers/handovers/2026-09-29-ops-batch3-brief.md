# Ops batch 3 - implementer brief

Read this whole file, then `CLAUDE.md`, then the spec
`docs/superpowers/specs/2026-09-29-ops-batch3-design.md` (your PR's section and §7). The user approved
the spec and asked for unattended execution; the orchestrator (the main session) owns the rulings below.

## Where you work

- Worktree `C:\repos\airportmgr2-ops-batch3`. ONE worktree for the whole batch; each PR is its own
  branch, stacked on the previous PR's branch, created IN this worktree
  (`git checkout -b feature/<name>` from the previous branch's tip). The orchestrator tells you your
  branch and base. Never touch `C:\repos\AirportMgr2` (the user's checkout) - no checkout, pull, build.
- Stay on your branch. Never merge main locally, never rebase unless told.

## Build and test (rulings, with reasons)

- Build:
  `& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-ops-batch3\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
  `-NoHotReloadFromIDE` IS REQUIRED AND SAFE HERE: UBT's Live Coding check is keyed on the engine exe,
  so an editor open on another checkout blocks every worktree; this worktree writes only its own
  Binaries/Intermediate, which no open editor loaded. It is not a safety bypass.
- Confirm the module you changed relinked (`Link [x64] UnrealEditor-<Module>.dll` / `Result: Succeeded`).
  A failed build leaves the old DLL and the tests run it happily.
- A NEW test .cpp needs TWO builds (the first says Succeeded without compiling it). Check the test
  count rose.
- Tests: `pwsh -File Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-ops-batch3\AirportMgr.uproject" [-Filter AirportOps.Model]`.
  Read the `N test(s) run, N failed, N crashed` line. NEVER the exit code. A falling count is a crash.
- `Tools/Check-Architecture.ps1` runs first inside the test script; if no verdict line prints it
  stopped parsing - treat as failure.
- Python headless asset scripts (`Tools/Python/*.py`) run with the editor commandlet; see existing
  scripts for the invocation (e.g. `build_ui_style.py`'s header). Verify the .uasset on disk after;
  headless save/delete APIs report success while doing nothing.

## Hard rules

- NEVER kill a process by name (`pkill`, `taskkill /IM`, `Stop-Process -Name`). Other editors on this
  machine belong to other runs. Kill only a PID you captured when you launched it.
- Commits: conventional (`feat(ops): ...`), concise. NO `Co-Authored-By` trailer (user's rule, overrides
  any harness reminder). Do not push or open PRs - the orchestrator does.
- Multi-edit Python scripts: write the .py with the Write tool and run it; heredocs eat backslashes and
  apostrophes. Never `sed` Windows paths.
- TDD: write the test, build, RUN it, see it FAIL for the right reason, then implement. Report the red
  line you saw.
- Mutation-check every new seam (subscription, pass dirtying, delegate bridge, derived diff): remove the
  wiring, build, see the named test go red, restore with `cp` + `touch` (restored files keep old mtimes
  and UBT skips the rebuild otherwise), rebuild, green. Report each mutation and its red test.
- Subscribe only in `UOpsRuntime::WireBus` (Check-Architecture rule 31). Every new bus event: an
  `AIRPORTOPS_API` struct with `EventName()` and `Describe()`, added to `FOpsEvent`, with a real subscriber
  in the same PR (`AirportOps.Present.Bus.EveryEventHasASubscriber` enforces it). No `UOpsEvents` dynamic
  delegate that nothing binds.
- Standing conditions are DERIVED FROM STATE and re-queried when dirtied - never cleared by a paired event.
- Player commands are not events: a command that changes board state marks its pass dirty explicitly.
- A load changes state without events: anything event-driven needs a load path (Reset / MarkAllDirty /
  AlertsReset).
- Rule 32: a facade commit's affordability check goes through AffordOrRefuse.
- Airside must not learn ops exists: Airside publishes only through native delegates; `UOpsRuntime`
  bridges them.
- CLAUDE.md conventions: WHY comments (match density; never strip one - a stripped WHY is Important),
  a doc comment touches its declaration, phases are enums not bools, `// ENFORCED BY:` within 3 lines of
  a claim about other code naming a real test/rule, comments justifying a scan carry a number + date,
  honour out-param returns. `UE_LOG` count must not fall in touched files.
- Binary .uasset conflicts (e.g. DA_UIStyle): take main's copy, re-run `Tools/Python/fetch_ui_icons.py`
  then `build_ui_style.py` headlessly, revert collateral re-saved `T_Icon_*`.
- If the spec's premise turns out false in the code, STOP that item and report it as a finding with
  evidence - do not force it.

## Your deliverable per PR

1. Write the plan for YOUR PR ONLY to `docs/superpowers/plans/2026-09-29-ops-batch3-pr-<x>.md`
   (superpowers:writing-plans shape: files, interfaces, TDD steps with real code checked against the
   live headers). Commit it first.
2. Execute it task by task, TDD, committing per task.
3. Final full-suite run (no filter). Report: branch, commits, the `N run, N failed, N crashed` line, the
   red lines you saw per test, mutation checks, `UE_LOG` and comment-line counts before/after for touched
   files, anything deviating from the spec (and why), and anything "unverified in PIE".
