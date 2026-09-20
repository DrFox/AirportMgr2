# Handover: the guide grid

Written 2026-09-20, twice. The first version handed items 2 to 4 over; this one records that
all four are **built and green, and that NONE of 2, 3 or 4 has been seen in PIE**.

## Where the work is

- **Worktree `C:\repos\airportmgr2_snapping`, branch `feature/guide-grid`.** Seventeen commits
  ahead of `origin/main`, nothing pushed, no PR open. The user asked for all four items on one
  branch and one PR.
- **THERE ARE THREE WORKTREES.** `airportmgr2_snapping` is this work; `airportmgr2_plotwork` is
  something else (the user's screenshots land there); the main checkout `C:\repos\AirportMgr2`
  has a binary with NONE of this in it. Build and test the right one, and if the user reports
  "I see nothing", check which editor they ran before believing a code fault.
- **488 tests, 0 failed, 0 crashed.** 480 at the previous handover; the eight are named below.

## The four items, and what landed

1. **Positional sources reach from the CURSOR.** The reported bug - a longer road lost its
   matching-gap guide because `FOffsetGuideSource` searched from the drag's ORIGIN. `Propose`
   gained the cursor and each source chose which end its reach is measured from.
   **Confirmed in PIE by the user.**
2. **A free start: guides before the first click.** One virtual, `WantsFreeStartGuides`, with
   the base answering it through a `bFreeStart` anchor whose Origin the driver fills from the
   plane hit. Taxiway, Road, Runway, Apron and Stand opt in; each also CONSUMES it.
   Not seen in PIE.
3. **`Collinear` on by default.** One line, and without it item 2 is dead on a new airport:
   every angular row sits out on a free start, so Collinear is the only row that can offer
   anything at all. Not seen in PIE.
4. **`EReference::Road` split into `Taxiway` and `ServiceRoad`.** `RoadNaming` gained
   `ReferenceOf` and `Describe` was rebuilt on it; four sources tag per segment; a node is
   tagged by every kind that meets it; `MatchingGap` requires one kind. Twenty-five cells,
   twelve bar buttons. Not seen in PIE.

## What to ask the user to look at FIRST

Nothing in 2, 3 or 4 has been in front of a player. In rough order of what is most likely to
be wrong on screen:

- **A free start on the taxiway tool (key 1), cursor near the extension of an existing road.**
  A dashed teal line and a label should appear BEFORE the first click, and the click should
  land on the line. If nothing appears, check whether `Collinear` is lit on the bar - and if it
  is not, item 3 did not reach the level's `GuideSources`, which is a UPROPERTY on
  `ARoadNetworkActor` and may carry an authored value that predates the new default. See
  CLAUDE.md on an editor-set UPROPERTY overriding a constructor: this is the FIRST thing to
  suspect, not the last.
- **The bar's SNAP TO row.** It should read Taxiway, Service road, Runway, Apron, Stand, World.
  Two new ids, `snapto.taxiway` and `snapto.serviceroad`, replace `snapto.road`, so
  `UUIStyle::IconFor` has no mapping written for either.
  `AirportMgr.UI.EveryActionResolvesAnIcon` passes, so something is resolving; whether it LOOKS
  right is a screen question and only a screenshot answers it.
- **The stand tool (key 3) beside an existing stand.** The one opt-in whose guides had to be
  built rather than wired: the stand names its heading as the anchor's reference so
  `Level with x Stand` can propose at all. Needs the Stand column switched on - it is off by
  default.
- **Candidate volume.** `ProposeAll` still reserves 16. Collinear is now on by default and
  spans six columns, AngledFrom throws three spokes per end per segment, and the stand tool
  offers every live entity unbounded. Nothing breaks - it reallocates - but this is where the
  first tuning complaint will come from, along with `SearchRadiusUu` and `MaxPullUu`, neither
  of which has ever had a PIE pass.

## Traps that cost time, across both sessions

Each of these was paid for. Do not pay again.

**THE EDITOR HOLDS THE DLL.** Three builds died on
`cannot open file ... UnrealEditor-Airside.dll`. Check before building:
`Get-Process -Name UnrealEditor*`. Ask the user to close it; do not kill it.

**A TEST THAT MEASURES THE PRODUCER AND NEVER THE CONSUMER.** This bit twice on 2026-09-20 and
the second time reached the user: `FRunwayTool` and `FOutlineDrawTool` were given
`DescribeGuideAnchor` and tests asserting the anchor was correct, and neither tool DREW or
OBEYED the guide. 479 tests were green while the feature did nothing on screen. Item 2 was
written against this - `FreeStartGuideTest.cpp` measures the PREVIEW and the CLICK through
`FBuildSession::MakeContext`, and never the anchor.

**A DEFENSIVE FILTER MAKES ITS OWN TEST VACUOUS.**
`Airside.Tool.GuideGridHasNoCellOutsideTheList` once passed with a source pointed at a declared
hole, because the chain gated candidates on `IsEnabled`, which consults `IsLegalCell`. It gates
on the two FLAGS only now. Related, and found during item 4: that same test's "every row and
every column on" list had `bAngledFrom` missing, so the spoke sources proposed nothing and it
had never seen the cells they tag. **A list that says "every" is worth counting.**

**ONE SOURCE DECLARES ONE RELATION.** `FSnapGuideChain::Resolve` skips a source by its declared
`Relation()` BEFORE it walks anything, so a source proposing two relations has both silenced by
whichever it named. That is why there are fifteen sources for twenty-five cells.

**PROVE EVERY NEW TEST CAN FAIL.** Break the rule it protects, watch the NAMED assertion go
red, restore by hand. Ten breakages were run across items 2 to 4, in four batches; batching is
fine as long as no two of them would mask the other's named assertion - check that before
batching, not after reading the output.

**A NEW TEST `.cpp` NEEDS TWO BUILDS** - the first can report `Result: Succeeded` without
compiling it. Check the DLL's mtime rather than trusting the word.

**`Check-Architecture` fails on an orphaned doc comment.** Deleting a declaration and leaving
its comment behind fails the lint before any test runs.

**Large Python through a bash heredoc breaks** - the shell eats the backslashes. Write the
script to the scratchpad and run it by path, with raw strings for Windows paths.

**Read the `N test(s) run, N failed, N crashed` line, never the exit code.**

## Decisions made, and not to be re-litigated

From the first session, all still standing:

- Two axes ANDed, not one flat list. The grid is a declared list consulted by `IsEnabled`, the
  registry test and the no-illegal-cell test.
- `ThisGesture` is a column with no button; the Alt hold covers "not for this drag".
- `AngledFrom x ThisGesture` is a deliberate hole: `LevelWith` already gives those lines.
- Collinear is the 0 degree member of the AngledFrom family.
- `MatchingGap` stays centreline-to-centreline: ICAO separations are specified that way.
- Only `Collinear x Apron` displaces by half-width.
- `DescribeGuideAnchor` takes the target, not the context.

Made in this session, each a judgement a reviewer may want to see and reverse:

- **`LevelWith x Runway` is a legal cell** - twenty-five, where the plan's arithmetic said
  twenty-four. Its hole rested on runway thresholds being "ordinary nodes already served by
  Road", a sentence the split made false: a threshold's node is incident to a runway segment
  and nothing else, so closing it would have taken away a line that works today. It is
  nonetheless off by default now, because the Runway column is.
- **`MatchingGap` requires the measured pair to be of ONE kind.** A narrowing, not a
  relabelling: before this, any two parallel non-runway roads could suggest a gap between them.
  ICAO separates taxiways by the wingspan admitted; a service road's spacing is a question of
  what has to drive between.
- **`bTaxiway` and `bServiceRoad` both default ON**, because the single `bRoad` they replace
  did. Splitting a switch is not a reason to change what it was set to.
- **The stand tool names its `LastHeading` as the anchor's reference.** Without a reference
  `FPointAlignGuideSource` declines outright, so the "level with a row of stands" the opt-in
  was argued for would never have been proposed at all. This is more than item 2 literally
  asked for, and is the smallest thing that makes the Stand opt-in mean anything.

## The eight new tests

- `Airside.Tool.FreeStartOffersOnlyPositionalGuides` - the mechanism: origin on cursor, so
  every angular candidate sits out inside `Arbitrate`. The angular rows are deliberately left
  ON, so their absence is the measurement.
- `Airside.Tool.FreeStartToolsDrawTheirGuide` - all five registry entries, at the PREVIEW.
- `Airside.Tool.FreeStartClickLandsOnTheGuide` - all four tool classes, at the CLICK.
- `Airside.Tool.ToolsThatDidNotOptInGetNoFreeStart` - the other half of the ruling.
- `Airside.Tool.FreeStartWorksOnTheShippedDefaults` - item 3, on an untouched settings struct.
- `Airside.Tool.RoadColumnAndLabelAgree` - one classification answers the label and the column.
- `Airside.Tool.EachRoadColumnSwitchesAlone` - what the split actually buys.
- `Airside.Tool.MatchingGapIsWithinOneKind` - with a control leg on a same-kind pair.

`Airside.Tool.BuildSession` also grew the five-tool free-start list, written out BY ID rather
than asked of the tools: a test that compared each tool's virtual against itself would have
passed however it was answered.

## Working with this user

- They test in PIE and report precisely, with screenshots in a `samples/` folder. Believe the
  report; find the cause. Three times across these sessions the cause was ours.
- They rule on design questions quickly when given a real choice and a recommendation. Ask one
  question at a time, with the geometry drawn rather than described.
- CLAUDE.md governs: no `Co-Authored-By`, feature branch then PR, never merge main locally,
  extremely concise prose.
