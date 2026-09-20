# Handover: the guide grid, items 2 to 4

Written 2026-09-20 at the end of a long session. Items 1 of 4 is done and **confirmed in PIE by
the user**; 2, 3 and 4 are specified, ruled on, and not started.

## Where the work is

- **Worktree `C:\repos\airportmgr2_snapping`, branch `feature/guide-grid`.** 14 commits ahead of
  `origin/main`, nothing pushed. **Do not merge** - the user wants 2, 3 and 4 on this same branch
  and PR first.
- **THERE ARE THREE WORKTREES.** `airportmgr2_snapping` is this work; `airportmgr2_plotwork` is
  something else (the user's screenshots land there); the main checkout `C:\repos\AirportMgr2`
  has a binary from the morning with NONE of this in it. Build and test the right one, and if the
  user reports "I see nothing", check which editor they ran before believing a code fault.
- 480 tests, 0 failed, 0 crashed at handover.

## Read these first, in this order

1. `docs/superpowers/specs/2026-09-20-guide-grid-design.md` - the design. Section 3 is the grid,
   6 is the width rule, 8 is which tools have anchors.
2. `docs/superpowers/plans/2026-09-20-guide-reach-and-free-start.md` - **items 1 to 4, with the
   user's rulings and the reasoning.** Item 1 is done. 2, 3 and 4 are the job.
3. `Solve/GuideArbiter.h` - the two enums and the grid. The code has overtaken the spec more than
   once today; where they disagree, the code is right and the spec needs updating.

## What was built today

`ESource` split into `ERelation` x `EReference`, gated with an AND against a declared grid of 19
legal cells. `AngledFrom` added on a user sketch. The Apron column filled with four sources. A
road lines up EDGE to edge with an apron, at its own asymmetric half-widths. Runway and apron
tools gained anchors, and then gained the code that actually uses them. Positional sources reach
from the cursor.

## What is left

Items 2, 3 and 4 of the plan, in that order - it says why. In one sentence each:

- **2. A free start.** Guides before the first click, via one `WantsFreeStartGuides()` virtual.
  The opt-in list is ruled: Taxiway, Road, Runway, Apron, Stand - not Fuel depot, Guidelines,
  Holding point or Select.
- **3. `Collinear` on by default.** One line, plus the test updates.
- **4. Split `EReference::Road` into `Taxiway` and `ServiceRoad`.** The largest: every source,
  `RoadNaming`, the bar, the grid test. 24 cells, 11 buttons. Nodes tagged by their incident
  segments, as ruled.

## Traps that cost time in this session

Each of these was paid for once. Do not pay again.

**THE EDITOR HOLDS THE DLL.** Three builds died on
`cannot open file ... UnrealEditor-Airside.dll`. Check for the process before building:
`Get-Process -Name UnrealEditor*`. Ask the user to close it; do not kill it.

**A TEST THAT MEASURES THE PRODUCER AND NEVER THE CONSUMER.** This bit TWICE today, and the
second time reached the user. `FRunwayTool` and `FOutlineDrawTool` were given
`DescribeGuideAnchor` and tests asserting the anchor was correct - and neither tool DREW or OBEYED
the guide. 479 tests were green while the feature did nothing on screen. The dashed line is
emitted by the TOOL, in `BuildPreview`; the session only resolves it onto the context. **A tool
that describes an anchor must also take `Context.GuidedCursor()` and emit the line.** When item 2
opts five tools in, check each one consumes it, and write the test at the preview level.

**A DEFENSIVE FILTER MAKES ITS OWN TEST VACUOUS.** `Airside.Tool.GuideGridHasNoCellOutsideTheList`
passed with a source deliberately pointed at a declared hole, because the chain gated candidates
on `IsEnabled`, which consults `IsLegalCell` - the filter was tidying away the exact fault the
test existed to find. The chain now gates on the two FLAGS only and legality is a contract the
test enforces. If you add a validity check, make sure the test sees the data before it.

**ONE SOURCE DECLARES ONE RELATION.** `FSnapGuideChain::Resolve` skips a source by its declared
`Relation()` BEFORE it walks anything, so a source proposing two relations has both silenced by
whichever it named. That is why there are 15 sources for 19 cells, and why the Apron column
arrived as four. Item 4 does not change this; adding a column means adding sources per relation.

**PROVE EVERY NEW TEST CAN FAIL.** Break the rule it protects, watch the NAMED assertion go red,
restore by hand. This caught three real gaps today, including one where the plot tool's
`EDragPoint::Boundary` was asserted nowhere at all.

**A NEW TEST `.cpp` NEEDS TWO BUILDS.** The first reports `Result: Succeeded` without compiling it.

**`Check-Architecture` fails on an orphaned doc comment.** Deleting a declaration and leaving its
comment behind fails the lint before any test runs. It caught this twice.

**Large Python via a bash heredoc breaks.** Write the script to the scratchpad and run it by path.
Use raw strings for Windows paths - a non-raw `"D:\Epic\UE_5.8\..."` is a unicode escape error.

**Read the `N test(s) run, N failed, N crashed` line, never the exit code.** A crashing test is
reported only there - one crash today was a real bug the exit code would have hidden.

## Decisions already made - do not re-litigate

- Two axes ANDed, not one flat list. The grid is a declared list consulted by `IsEnabled`, the
  registry test and the no-illegal-cell test.
- `ThisGesture` is a column with no button; the Alt hold covers "not for this drag".
- `AngledFrom x ThisGesture` is a deliberate hole: `LevelWith` already gives those lines.
- Collinear is the 0 degree member of the AngledFrom family.
- `MatchingGap` stays centreline-to-centreline: ICAO separations are specified that way.
- Only `Collinear x Apron` displaces by half-width - it is the one cell where a centreline drag
  meets an extended boundary.
- `DescribeGuideAnchor` takes the target, not the context: it is called from inside
  `MakeContext` while that context is half-built.

## Still not done, and deliberately

- **No PIE pass on `SearchRadiusUu`, `MaxPullUu` or the candidate count.** `ProposeAll` reserves
  16; `AngledFrom` alone proposes three spokes per end per segment in reach, and the apron sources
  multiply by every edge. Nothing breaks - it reallocates - but this is the first number that will
  want tuning once the whole grid is switched on.
- **Stand placement has no guide anchor** until item 2 gives it one.
- **`MatchingGap x Runway`** - runway-to-taxiway separation is a real standard and a legitimate
  future cell, needing its own search rather than a free ride on the road one.

## Working with this user

- They test in PIE and report precisely, with screenshots in a `samples/` folder. Believe the
  report; find the cause. Twice today the cause was mine and not theirs.
- They rule on design questions quickly when given a real choice and a recommendation. Ask one
  question at a time, with the geometry drawn rather than described.
- CLAUDE.md governs: no `Co-Authored-By`, feature branch then PR, never merge main locally,
  extremely concise prose.
