# AirportMgr

UE 5.8.2, C++. A Cities-Skylines-style procedural airport: roads, taxiways, aprons,
stands, and routing over them. The `Airside` plugin holds the model; the `AirportMgr`
game module holds the runtime driver; `AirsideEditor` holds the design-time mode.

Engine source: `D:\Epic\UE_5.8`. Project: `C:\repos\AirportMgr2`.

## Diagnosing "it doesn't work"

**Read `Saved/Logs/AirportMgr.log` BEFORE forming any hypothesis.** Not after two wrong
guesses. It answers, in one grep, the questions most often guessed at:

- **Which driver is live.** There are two, and they have different keys and different
  cancel gestures. `Game class is 'BP_RoadBuildGameMode_C'` plus `LogRoadBuild: Road
  building ready on ...` means PIE and `ARoadBuildController` (right-click cancels).
  `Road Build mode entered` means the editor mode `URoadBuildEdMode` (Escape cancels).
  Confusing the two has cost multiple rounds of fixes aimed at the wrong module.
- **Which binary is running.** `Log file open, <time>` against the DLL's mtime says
  whether the user is running the build you just made.
- **Whether the thing even started.** An absent log line beats any amount of reasoning
  about why a feature misbehaves.

**Never assert what the user is doing.** "You're in the editor mode" is a guess; say what
would distinguish the cases and measure it. When the user says something does not work,
that is an OBSERVATION, not a hypothesis - believe it and find the cause. Telling them
they have not done something, when a log on disk could have settled it, is the single
most expensive failure mode in this project's history.

**A log line is not evidence the thing it describes exists.** The startup banner once
advertised "4 routes" while `EKeys::Four` was unbound, because the banner and the binding
were written in different breaths. Verify the mechanism, not the message about it.

**Check where a list is CONSUMED, not where it is declared.** This codebase has shipped
this bug three times: `UEdMode::ToolCommandList` (created, never read),
`GetModeCommands()` (a palette nothing rendered, because `FModeToolkit::GetToolPaletteNames`
is empty by default), and `ARoadBuildController::Tools` (a tool with no key binding).
Adding to one list means finding every list that must agree with it.

### Instrument before the repro, not after

Each repro costs the user a PIE session, often an editor restart. Make one round trip
answer the question: put `UE_LOG` lines at the boundary under suspicion (categories
`LogRoadBuild` in the game module, `LogAirside` in the plugin) BEFORE asking them to
reproduce, and say exactly which line to look for. A log line is a function-body edit, so
it is Live Coding, never a close. If the logic lives in `Model/` or `Solve/`, skip the
repro entirely: write the failing automation test that reproduces the report first, then
fix until it passes. A test that pins the bug is worth more than the fix.

### When the code is right and the runtime is wrong

If the source implies X and the user sees Y, there is a mechanism you have not found.
Say so, then check these before re-reading the code - each has fooled a session:

- **A stale Blueprint.** `BP_RoadBuildGameMode`, `ABP_PiperMeridian` and friends cache
  their C++ parent's layout; a changed signature or reparent needs the BP recompiled and
  resaved, or the old graph runs against the new class.
- **A stale CDO.** After Live Coding, objects already in memory keep old defaults; a
  changed constructor default shows only after a restart. `Log file open` dates the binary.
- **An editor-set UPROPERTY overriding the constructor.** Once a value is set in a level
  or BP, the constructor default is dead code for that instance. This project deliberately
  puts knobs in properties, so it is the FIRST thing to ask about a "default" that does not
  take. Read the instance from the .umap or the log, not from the constructor.
- **Lifetime.** A pointer that is null at `BeginPlay` and set later, or an object collected
  because nothing `UPROPERTY`-held it.
- **Editor and runtime paths diverging.** `WITH_EDITOR`, `GIsEditor`, construction script
  against `BeginPlay`, and the two drivers above. The same function can run in both and
  behave differently in each.

Replication is not on this list: the game is single-player and has no networking.

### Claiming a fix

"Fixed", "works" and "verified" mean one of these exists and is named in the reply:

- a line from `Saved/Logs/AirportMgr.log` after the user's repro, or one you read yourself
- output from a console command or an MCP query against the running editor
- a screenshot, or the user's own description of the behaviour
- a headless test run whose pass line you can quote

Anything else is an inference, and the reply says which kind:

| You have | It proves | Say |
|---|---|---|
| `Result: Succeeded` | the code is plausible | "builds, unverified at runtime" |
| a call site for X | X can be reached | "called from X; not shown to run in this case" |
| a null check added | nothing | "should handle it; a prediction" |
| a walkthrough of the logic | static analysis | "traced; not measured" |

A fix reply ends with the verification step: what to run, where to look (log line,
screen, console command), and what result confirms it. The user's answer to that IS the
result of the fix; do not treat it as a formality. If the `unreal` MCP tools are absent
this session, the editor was not up when the session started - use `Tools/Mcp.py` (see
the MCP section) rather than reporting the capability missing. A `shot` of the viewport
after the repro is admissible evidence; a screenshot the user describes is too.

## Engine source

Grep it freely - it is the authoritative answer and beats recalled UE lore, which is
frequently wrong. But keep it cheap:

- **Scope the path.** `grep -rn "Foo" D:/Epic/UE_5.8/Engine/Source/Runtime/Engine` - never
  the whole tree.
- **Use `-A`/`-B` for context; never `Read` a whole engine file.** Tens of lines, not
  thousands.
- **Write durable findings to memory** (`unreal-*` notes). A fact looked up twice is a
  memory file that was not written.

## MCP: look at the editor instead of guessing

Epic's own MCP ships with UE 5.8 (`Engine/Plugins/Experimental/ModelContextProtocol`,
Experimental). This project enables it plus three toolsets, in `AirportMgr.uproject`:

| Plugin | Why it is on |
|---|---|
| `ModelContextProtocol` | The server. `http://localhost:8000/mcp`, wired up in `.mcp.json` as `unreal`. |
| `EditorToolset` | Editor and PIE state - answers "which driver is live" without guessing. |
| `AutomationTestToolset` | `RunTestsByFilter` / `GetTestResults` against the RUNNING editor. |
| `LiveCodingToolset` | `CompileLiveCoding` - compile without making the user close the editor. |

`AllToolsets` is deliberately NOT enabled: it drags in MetaHuman, Niagara, PCG, Sequencer
and the rest, loading on every editor start for tools nothing here calls. Swap it in if a
workflow needs them.

`Config/DefaultEditorPerProjectUserSettings.ini` sets `bAutoStartServer=True`, because
**Claude Code connects to MCP servers once, at session start** - a server that must be
started by hand is dead for any session that began first. It also pins
`bEnableToolSearch=True`, which keeps hundreds of tool schemas out of the prompt behind
`list_toolsets` / `describe_toolset` / `call_tool`.

**If the editor came up AFTER the session did, the `unreal` tools are absent and stay
absent.** Do not restart the session; use `Tools/Mcp.py`, which speaks the same JSON-RPC
to the same port from the shell (verified 2026-09-05 against the running editor):

```
python Tools/Mcp.py shot out.png            # level viewport - LOOK before reasoning
python Tools/Mcp.py shot out.png editor     # whole editor window, dialogs included
python Tools/Mcp.py log LogRoadBuild        # tail this session's log by category [regex] [n]
python Tools/Mcp.py call EditorToolset.EditorAppToolset IsPIERunning
python Tools/Mcp.py toolsets | describe <toolset>
```

Tool names are the SHORT form (`IsPIERunning`); the qualified form is "Unknown tool". There
is no run-console-command tool, only `SearchCVars`; a runtime value you need to read
goes through a `UE_LOG` and `log`, not a console command.

**Prefer these over the shell when the editor is up:**

- `CompileLiveCoding` instead of asking the user to close the editor. It covers function
  bodies only - new UPROPERTYs, UCLASSes and classes with vtables still need a full
  `Build.bat`, so batch those.
- `AutomationTestToolset` instead of `Run-AirsideTests.ps1` for a quick loop; the script
  spawns a whole cold editor. Use the script for the authoritative pre-commit run, since
  it is what catches a CRASHED test.
- `EditorToolset` before asking the user what they are looking at.

`Plugins/McpAutomationBridge` (third-party, port 3000/8090) is present but DISABLED in the
.uproject - the fallback if the experimental one misbehaves. Do not run both.

## Build and test

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex

./Tools/Run-AirsideTests.ps1            # all; -Filter Airside.Solve to narrow
```

- **The editor must be CLOSED to build.** Live Coding holds the DLLs and the build fails
  with "Unable to build while Live Coding is active". `Ctrl+Alt+F11` covers function
  bodies only - not new UPROPERTYs, UCLASSes, or classes with vtables. Batch such changes
  into one round rather than making the user close the editor repeatedly.
- **Never trust the automation runner's exit code.** `Run-AirsideTests.ps1` parses the log
  and diffs started-against-completed tests, because a CRASHING test used to vanish and
  report green. Read its `N test(s) run, N failed, N crashed` line.
- `Tools/Python/*.py` author materials headlessly and need the editor closed.

## Architecture

- `Model/` - the graph, entities, traffic, route search. Plain UObjects, testable with
  `NewObject` and no world.
- `Solve/` - dependency-free geometry (`CoreMinimal.h` only). No engine types beyond it.
- `Build/` - derives meshes and the guideline graph from the model.
- `Tool/` - `IBuildTool` strategies. They describe intent to `IToolPreviewSink` in ROAD
  PLANE coordinates naming a MEANING, never a colour. That is what keeps the plugin free
  of the game module; do not leak presentation into it.
- `Present/` - `ARoadNetworkActor`, a Facade and the single level-resident object.

Named deviations from textbook patterns are documented at their site (undo is a Memento,
not a Command; `RouteSearch` takes `URoadNetwork` rather than a graph adapter; the editor
mode must use ITF because `UEdMode` has no raw input hooks). Keep that habit: state the
pattern, then justify the deviation.

## Two invariants that must not be weakened

- **The surface model welds BITWISE.** One solver owns each node's boundary and hands the
  same vertex values to both the junction polygon and the trimmed segment ends. Tests
  assert shared vertices are exactly equal. If a tolerance is ever needed there, the
  contract has broken and the seams are back. Assertions that merely NAME this contract
  can pass on a fully cracked mesh - prefer ones that measure it.
- **The guideline graph samples ONCE.** `GuidelineGeom::Sample` produces the array that
  the search costs, the overlay draws, and the follower walks. A second evaluator lets an
  agent leave the line the player was shown, visibly and only on bends.

Note the two are opposite by design: the surface model shares by POSITION and must be
bitwise; the guideline graph shares by HANDLE and needs no such contract.

## Conventions

- Comments explain WHY, and especially why an obvious alternative was rejected. The
  codebase is dense with this deliberately - match it rather than stripping it.
- Tests assert behaviour with a named reason, not just values.
- Branches are `feature/*`; PRs to `main`.
- A doc comment touches its declaration. Inserting between them means moving the comment.
