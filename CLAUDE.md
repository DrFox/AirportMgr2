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
