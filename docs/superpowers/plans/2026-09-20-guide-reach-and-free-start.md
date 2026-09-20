# Guide Reach, Free Starts and the Road Split

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make the matching-gap guide usable, and stop the Road column meaning two different things.

**Spec:** `docs/superpowers/specs/2026-09-20-guide-grid-design.md`. These four items came from PIE on 2026-09-20 and are user-ruled; the reasoning is below, not in the spec yet.

**State when written:** the grid, the apron column, the widths and the two tool anchors are all built and committed on `feature/guide-grid`. `FRunwayTool` and `FOutlineDrawTool` consume their guides as of the last commit. 479 tests green.

## Global Constraints

- **Editor CLOSED for every build.** Worktree build, `-NoHotReloadFromIDE`. The editor has interrupted three builds today by holding `UnrealEditor-Airside.dll`; check for the process before starting.
- A new test `.cpp` needs TWO builds. Read the `N test(s) run, N failed, N crashed` line, never the exit code.
- Run tests from a **PowerShell** tool call.
- Prove each new test can fail by breaking the rule it protects.
- No `Co-Authored-By` trailer.

---

## 1. Positional sources reach from the CURSOR, not the origin

**The bug, reported with `samples/matching1.png`.** "If you build a road that had a matching gap
suggestion, the next road you try to build has to have a SHORTER segment than the previous one
otherwise it wont get the suggestion."

**Length is the symptom.** `FOffsetGuideSource` picks its reference as the nearest segment to the
ANCHOR'S ORIGIN - the player's first click - and applies `SearchRadiusUu` from there. A longer
road starts further from the pair being matched, the search finds nothing, and the source returns
before proposing anything. The cursor, right beside the roads in question, is never consulted:
`Propose` is not given it.

**The fix.** `IGuideSource::Propose` gains the cursor. Each source then chooses which point its
reach is measured from:

| Source | Reach from | Why |
|---|---|---|
| `FParallelGuideSource`, `FExtendingGuideSource` | **origin** | they answer "which way from here", and its own comment records why a cursor-keyed search is wrong: *"a search keyed to the cursor would hand the guide to a different road halfway through the drag"* |
| `FOffsetGuideSource`, `FCollinearGuideSource`, `FApron*`, `FAngled*` | **cursor** | they answer "where did the far end land", and the far end is under the cursor |

The failing test is already written: `Airside.Tool.OffsetGuideReachesWhatTheCursorIsNear` in
`OffsetGuideTest.cpp`. It asserts a road starting 300 m away still gets the gap when its far end
reaches the pair. **It has never been run** - the editor held the DLL. Run it first; it is
expected to fail on its second leg.

## 2. A free start: guides before the first click

**Ruled 2026-09-20.** "I think a lot of the tools would benefit from snapping to guides before the
first place of the road. It doesnt make sense for all of them, but some it does."

**It costs almost nothing, because the arbiter already handles it.** With the anchor's origin set
to the cursor, every ANGULAR guide sits out automatically - `SnapGuide::Arbitrate` cannot measure
a direction from a point to itself and says so deliberately: *"A CURSOR ON TOP OF THE ORIGIN HAS
NO DIRECTION, so no angular candidate can be measured at all... A Perpendicular one still can."*
So a free start offers exactly the positional guides and nothing meaningless. No special-casing.

**One virtual, not per-tool boilerplate:**

```cpp
	/**
	 * Whether a guide may position this gesture's FIRST click.
	 *
	 * POSITIONAL GUIDES ONLY, and that falls out rather than being enforced: the base fills the
	 * anchor's Origin with the cursor, and Arbitrate then measures no direction from it, so every
	 * angular candidate sits out on its own.
	 */
	virtual bool WantsFreeStartGuides() const { return false; }
```

The base `DescribeGuideAnchor` returns a free-start anchor when this is true and the tool is idle.
`FGuideAnchor` needs a `bFreeStart` flag because `DescribeGuideAnchor` is not handed the cursor -
`FBuildSession::MakeContext` fills `Origin` from `PlaneHit` when it is set.

**Who opts in, as ruled:**

| Tool | | Why |
|---|---|---|
| Taxiway (1), Road (9) | yes | start a road in line with, or a matching gap from, existing ones |
| Runway (6) | yes | place a threshold in line with another runway, or at a standard separation |
| Apron (2) | yes | start an outline flush with a road edge or level with a corner |
| Stand (3) | yes | positioning a stand level with a row of stands is what the Stand column is for |
| Fuel depot (0) | no | its first click MUST land on a service road - already snap-constrained, and its own comment warns that an extra rule here would be a second opinion about where the anchor may go |
| Guidelines (5), Holding point (8) | no | both click existing nodes; the snap chain already decides |
| Select (4) | no | not a placement |

**A TOOL MUST ALSO CONSUME IT.** Describing an anchor and stopping is what shipped on 2026-09-20
and showed the player nothing - see the `RunwayDrawGuide` comment. Each opted-in tool's first
click takes `Context.GuidedCursor()` and its preview emits the dashed line.

## 3. `Collinear` on by default

It was off when it meant "one candidate per road in reach". It now also gates the apron flush
guide, the runway's extended centreline and `Collinear x Stand` - and a player hit exactly that,
switching Collinear on being the undocumented step between "no edge alignment" and "works".

## 4. Split `EReference::Road` into `Taxiway` and `ServiceRoad`

**Why.** `RoadNaming::Describe` already classifies three ways - `"runway 18/36"`, `"the service
road"`, `"the taxiway"` - so a label reading *"parallel to the service road"* appears under a
button marked **Road**. The grid collapsed two things the rest of the codebase keeps apart:
different tools (1 and 9), different profiles, different traversal classes.

**ONE classification, not a third copy.** `RoadNaming.h`'s own header exists because this was
duplicated once already: *"A guide label saying 'parallel to the taxiway' over a service road is
the kind of wrong that survives review, because each reader assumes the other's definition."* So
`RoadNaming` gains a function returning the KIND, `Describe` is rebuilt on it, and the guide
sources ask the same one.

**Nodes are tagged by their incident segments** - ruled 2026-09-20. A node where a taxiway meets a
service road belongs to both columns, so `FRoadDrawTool` emits a `FGuidePoint` per distinct kind
rather than picking a winner.

**`MatchingGap` splits too**: taxiway separation and service-road separation are different
standards.

Seven references, and every Road cell doubles: **24 legal cells**, up from 19. Eleven bar buttons.

## Order

1 first - it is the reported bug and needs no other change. Then 2, which shares the anchor
plumbing. Then 3, a one-line default with test updates. Then 4, which touches every source and is
the largest; doing it last keeps the others bisectable.

## Still not done, and deliberately

- **No PIE pass on `SearchRadiusUu`, `MaxPullUu` or the candidate count.** `ProposeAll` still
  reserves 16; `AngledFrom` alone proposes three spokes per end per segment in reach.
- **`MatchingGap x Runway`** - runway-to-taxiway separation is a real ICAO standard and a
  legitimate cell, but it needs its own search rather than a free ride on the road one.
