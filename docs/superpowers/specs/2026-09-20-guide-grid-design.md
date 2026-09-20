# Snap guides: a grid, not a list

The eight guide toggles are two axes wearing one coat. Split them into what a guide MEANS and
what it is measured AGAINST, gate a candidate on both, and the bug that prompted this stops
being representable.

## 1. Why

Reported 2026-09-20, verbatim:

> *i have parallel on and runway off [and] the guide will still show me that my taxiway is
> parallel to a runway*

and, on being shown why:

> *the runway button implied to me that i turn runway on or off as a source for any of the
> previous guide types of extending, parallel etc. I also see the same thing with world and
> would expect runway, and road in there as options as well*

The second quote is the design. The first is a symptom of its absence.

**Root cause.** A runway is not a type in the model - `URoadNetwork::IsRunwaySegment`
(RoadNetwork.cpp:227) answers it by asking whether the segment's profile sets
`bContinuousThroughJunctions`. Four guide sources walk `Network.GetSegments()`; exactly one of
them asks that question:

| Source | Asks `IsRunwaySegment` |
|---|---|
| `FRunwayGuideSource` | yes - `if (!Network.IsRunwaySegment(Id)) continue;` |
| `FParallelGuideSource` | **no** |
| `FCollinearGuideSource` | **no** |
| `FOffsetGuideSource` | **no**, as reference or as neighbour |

And `RoadNaming::Describe` asks the runway question FIRST (RoadNaming.cpp:19), returning
`"runway 18/36"` ahead of the taxiway and service-road tests. So with the Runway toggle off,
`FParallelGuideSource` picks the runway as the nearest road and the player reads *"parallel to
runway 18/36"*. `Collinear` and `Offset` have the same bug waiting behind their own toggles.

**Why this is not a three-line fix.** Adding the missing `IsRunwaySegment` skip to the other
three would make the Runway toggle mean "no runway guidance at all" - and would DELETE "in line
with runway 09/27", the extended centreline, because `FRunwayGuideSource` proposes only angular
candidates. More to the point it would leave the real defect standing: `ESource` mixes
relationships (Extending, PointAlign, Collinear, Parallel, Offset) with references (Runway,
World, and `Aligned`, which means "a stand"), and `Parallel` and `Collinear` carry a reference -
"a road" - that has no name and no switch. There is no axis for `Runway` to switch off along,
which is precisely why the button could not mean what it was read to mean.

**The earlier hypothesis, ruled out.** The first report was that the bar's lit buttons
disagreed with the settings, "as if the mvvm model is out of sync". It does not:
`UBuildBarWidget::RefreshState` re-reads `Actor->GuideSources` every tick through
`IsGuideSourceOn`, `Entry->ActionIndex` is the global registry index, and `M_Starter` carries no
`GuideSources` override at all - it places `/Script/Airside.RoadNetworkActor` directly, with no
Blueprint subclass, so the instance runs on the struct defaults (Extending, PointAlign, Parallel
and World on). Recorded because the absence of a cause is a finding: if the lit set is ever seen
to be wrong AFTER this change, it is a second bug and wants its own `UE_LOG` on the flags before
a repro, not a re-derivation of this paragraph.

## 2. Decisions taken

- **Two enums, ANDed.** `ESource` splits into `ERelation` (what the guide means) and
  `EReference` (what it is measured against). A candidate carries both; it is offered only when
  BOTH toggles are on. A column switched off removes every row in that column - which is what
  the report asked for, stated as a rule rather than as a patch.
- **The grid is a declared list, in `Solve/`.** Not every pair is legal. The legal set is
  written down once and consumed three times: by `IsEnabled`, by the registry test, and by a new
  test that asserts no source can produce a pair outside it. CLAUDE.md's "check where a list is
  CONSUMED": the old registry test walked `ESource` against the button list and could never have
  caught this, because `Collinear x Runway` was firing while nothing declared that cell existed.
- **A column carries a SEARCH POLICY, not only a filter.** `FRunwayGuideSource` differs from
  `FParallelGuideSource` in two ways beyond the kind test: no reach limit, and every runway
  proposes rather than only the nearest. So Runway cannot be Parallel with a flag flipped. Two
  sources may share a row; rows are not owned.
- **A positional guide aligns LIKE WITH LIKE.** Centreline to centreline, boundary to boundary,
  and a centreline against a boundary is displaced by the drag's half-width. See section 6.
- **`ThisGesture` is a column with no button.** See section 7.
- **No new fit kind, no new arbitration.** `EFit` still decides which race a candidate runs in
  and there is still one winner per kind. This change is entirely about which candidates EXIST.

## 3. The grid

Six relations by SEVEN references. Twenty-five legal cells; the rest are holes, and a hole is
a statement, not an omission.

```
                  This drawing  Taxiway  Service rd  Runway   Apron   Stand   World
Extending              #           -          -        -        -       -       -
Level with             #           #          #        +        +       +       -
Parallel / square      -           #          #        #        +       #       #
Collinear              +           #          #        +        +       +       -
Angled from            -           +          +        +        +       -       -
Matching gap           -           #          #        -        -       -       -
```

`#` exists today, `+` is new, `-` is a hole.

**THE ROAD COLUMN BECAME TWO, later on 2026-09-20** - see section 4. Every cell that said
"Road" doubled, and `Level with x Runway` opened as a consequence rather than by doubling: its
hole had rested on a runway's thresholds being "ordinary nodes already served by Road", and
that sentence stopped being true the moment one column became two. Closing it instead would
have taken away a line that works today, which is not what a refactor may do.

**ANGLED FROM WAS ADDED 2026-09-20**, after the rest of this document, on a sketch
(`samples/suggestion.png`) labelled *"45 degrees to other road"* with the line drawn to that
road's near END. Nothing offered it: `Parallel` squares to a road through the DRAG's origin,
never through the road's own end, and `Collinear` offers only the 0 degree member. It proposes
the other three of the four the world grid offers - 45, 90 and 135 degrees - measured from that
reference and passing through each of its ends. **Collinear is the 0 degree member of the same
family**, which is what fixes the shape of both.

90 degrees earns its place separately: a stub leaving a threshold at right angles had no guide
at all, because the only square-to-a-road candidate passes through the drag rather than the
road's end.

**The holes, each with its reason** - a hole nobody can justify is a cell somebody forgot:

| Hole | Why |
|---|---|
| Extending x anything but This drawing | Extending means "the edge this gesture is already growing". There is no other edge it could mean. |
| Level with x World | A world axis has no position, so there is no point to be level with. |
| Collinear x World | The same: a direction with no position is not a line to be on. That is Parallel x World. |
| Parallel x This drawing | This IS Extending. A second name for one behaviour is what this whole document exists to remove. |
| Angled from x This drawing | `Level with x This drawing` already proposes lines through every pinned corner ALONG the anchor's reference and ACROSS it - the 0 and 90 degree members of this family off the same direction. The 90 degree spoke would be that identical line under a second name. The one hole here that is not about geometry. |
| Angled from x Stand | A pose is a point and a direction. There is no end to radiate from. |
| Angled from x World | A world axis has no position, so it has no end either - the same hole as `Collinear x World`. |
| Matching gap x everything but the two road columns | Its reference must agree with Parallel's choice of nearest road, or the two stop describing one road between them - see section 5. Runway separation is a real standard and a legitimate future cell, but it needs its own search, not a free ride. **And the pair it measures between must be of ONE kind**: ICAO separates taxiways by the wingspan admitted, while what a service road keeps from the next one is a question of what has to drive between them, so a mixed pair keeps a gap that is neither standard. |

## 4. The types

`Solve/GuideArbiter.h`, still CoreMinimal-only:

```cpp
namespace SnapGuide
{
    /** WHAT a guide means. Declaration order is the angular/perpendicular tiebreak. */
    enum class ERelation : uint8 { Extending, LevelWith, Parallel, Collinear, MatchingGap };

    /** WHAT it is measured against. Declaration order breaks ties within a relation. */
    enum class EReference : uint8
    { ThisGesture, Taxiway, ServiceRoad, Runway, Apron, Stand, World };

    /** The legal pairs - section 3's grid, as the one list everything reads. */
    AIRSIDE_API bool IsLegalCell(ERelation Relation, EReference Reference);
}
```

`FCandidate::Source` becomes `Relation` plus `Reference`. The arbiter's tiebreak -
`Candidate.Source < Best->Source` - becomes the pair compared lexicographically, relation first.

**Both are plain enums, as `ESource` was**, for the reason already recorded on it: UHT cannot
see an enum without a `.generated.h`, and a `Solve/` header may not have one.

**The one behaviour change, stated rather than discovered later.** Ties can only occur within a
fit kind, and the relations split cleanly across the two - Extending and Parallel are angular;
LevelWith, Collinear and MatchingGap are perpendicular. So:

- The **perpendicular** order is identical to today: PointAlign, Collinear, Offset.
- The **angular** order changes in one place. Today `Aligned` (a stand's pose) ranks third of
  eight, above `Collinear` and above `Parallel`; under `Road > Runway > Stand > World` it lands
  after Runway. This is an improvement and its justification is already in the code:
  `FAlignedGuideSource`'s own header calls it *"the weakest of the four network sources"*, which
  its rank contradicted. It is observable only on an exact tie.

## 5. What each source becomes

| Today | Relation x Reference | Change |
|---|---|---|
| `FExtendingGuideSource` | Extending x ThisGesture | none |
| `FPointAlignGuideSource` | LevelWith x *per point* | see below |
| `FAlignedGuideSource` | Parallel x Stand, **Collinear x Stand** | gains the pose's own line |
| `FParallelGuideSource` | Parallel x Road | **gains** the `IsRunwaySegment` skip |
| `FRunwayGuideSource` | Parallel x Runway, **Collinear x Runway** | gains a perpendicular candidate |
| `FCollinearGuideSource` | Collinear x Road, **Collinear x ThisGesture** | **gains** the `IsRunwaySegment` skip |
| `FWorldGuideSource` | Parallel x World | none |
| `FOffsetGuideSource` | MatchingGap x Road | reference now excludes runways |
| *(new)* `FApronGuideSource` | Parallel / Collinear / LevelWith x Apron | the apron work |

**`FGuidePoint` must carry its own reference.** `Anchor.AlignTo` is a flat array today; the plot
tool fills it with its own corners and the road tool with live network nodes, and
`FPointAlignGuideSource` cannot tell them apart. Gating LevelWith by column means the TOOL tags
each point - `ThisGesture` for its own corners, `Road` for network nodes. An EXISTING apron's corners
are network data and belong to `FApronGuideSource`, not to a tool tagging its own points. That is the same split the anchor already makes for `ReferenceName`: only the tool
knows what its own points are.

**Runway keeps the extended centreline.** Collinear skipping runways would delete "in line with
runway 09/27" outright, so it moves into `FRunwayGuideSource` - and arrives UNBOUNDED, like that
source's angular candidates. That is correct rather than convenient: the extended centreline is
the approach path, and it is meaningful from anywhere on the field.

**Offset's reference must agree with Parallel's.** Its header records that it deliberately picks
the same nearest road `FParallelGuideSource` does, so "parallel to the taxiway" and "the same
gap as its neighbour" describe ONE road between them. Once Parallel excludes runways, Offset
must too, or the two guides stop composing.

**A source declares its relation; its reference may be fixed or per-candidate.** `Kind()`
becomes `Relation()`. `Propose` gains the settings so a source can skip a column BEFORE walking
it - preserving the existing "skipped before it works, not filtered after" property that the
chain's own comment defends.

## 6. Widths: aligning like with like

Raised in review, and it is a real hole in the naive design: a road's CENTRELINE lined up with
an apron's EDGE is not what anybody means. You want the road's edge flush with the apron's.

| Drag point | Reference | Displacement |
|---|---|---|
| centreline | centreline | none - road to road, road to runway, road to a stand's centreline |
| boundary | boundary | none - an apron corner against another apron's edge |
| centreline | **boundary** | the drag's half-width, **one candidate per side** |

**BUILT 2026-09-20, and it applies to exactly one cell.** `Collinear x Apron` is the only place on
the grid where a centreline drag meets an extended BOUNDARY, so `FApronLineGuideSource` is the
only source that displaces. Stated as a table so a later reader does not generalise it:

| Cell | Displaced? |
|---|---|
| `Collinear x Apron` | **yes** - the road's edge is what you want flush with the apron's |
| `LevelWith x Apron` | no - a corner is a point, with no extended edge to run flush along |
| `AngledFrom x Apron` | no - same; it radiates from a corner |
| `Parallel x Apron` | no - angular, through the origin: it constrains direction, never position |
| anything x Road / Runway / Stand | no - those references are centrelines, and centre-to-centre is what a junction solve wants |

**Both sides, not the cursor's side.** Flush inside the apron (a taxiway running along it) and
flush outside (one abutting it) are both real intents, and neither is nonsense - unlike
`FOffsetGuideSource`'s side rule, which exists because the wrong side proposed drawing on top of
an existing road. The two lines are 2x half-width apart, far outside the 300 uu corridor, so
there is no flicker to guard against.

**The two half-widths differ.** `URoadProfile::GetHalfWidthLeft()` and `GetHalfWidthRight()` are
separate - a cross-section may be off-centre - so the two candidates are not a mirrored pair and
must not be computed as one.

**`FGuideAnchor` gains three fields**: `HalfWidthLeft`, `HalfWidthRight`, and what the moving
point represents (centreline or boundary). The road and runway tools say centreline; the apron
and plot tools say boundary.

**`DescribeGuideAnchor` gains an `IRoadEditTarget*`.** The tool holds a `WidthIndex`, not a
profile; resolving one to the other needs the target. It cannot take the `FToolContext` instead:
the call sits INSIDE `FBuildSession::MakeContext` while that context is being built, so it would
receive a half-filled struct whose own `Guide` field is the thing being computed.

**What this does NOT change**, stated so a later reader does not "fix" it:

- **Parallel x Apron is unaffected.** It is angular through the origin: it constrains direction,
  never position, so there is no edge to be flush with.
- **Matching gap stays centreline-to-centreline.** Edge-to-edge is tempting, but ICAO separation
  minima are specified between centrelines, so the number in the label is the one an airport is
  actually laid out to. Measuring edges would show a figure nobody designs against.

## 7. The bar

Two sections. `EActionSection` gains one value; `BuildBarWidget.cpp:55`'s `static_assert` binds
`SectionSpecs` to `EActionSection::Count`, so the new section cannot silently lose its slot -
that list already agrees by construction.

```
ALIGN BY   [Extending] [Level with] [Direction] [Collinear] [Angled from] [Matching gap]
SNAP TO    [Taxiway] [Service road] [Runway] [Apron] [Stand] [World]
```

Twelve buttons, up from eight. **Twenty-five legal cells, twelve buttons** - the AND is what
keeps a toggle per cell off the bar, and is the whole reason two lists beat one.

**"DIRECTION" IS `ERelation::Parallel`**, and the two names differ deliberately - renamed on
the bar 2026-09-20, from PIE. A player switched on `Angled from` and `World`, saw nothing, and
observed that the row does three things while the button claimed one of them: it offers a
direction AND its perpendicular ("square to the taxiway" is not parallel to anything), and for
the World column an absolute compass axis, which is parallel to no thing at all. Section 3's
grid had called this row "Parallel / square" all along; the button kept the first word.

**AND IT OFFERS FOUR ANGLES AGAINST EVERY REFERENCE**, not two - 2026-09-20, from the same
PIE session: "it should have more options than square and parallel... the 45 degree increments
should be consistent for direction." They were not. World had offered 0/45/90/135 since stage
1 while every other column in the row offered 0 and 90 only, so one button meant a different
thing depending on which column it crossed, and there was no way to point a road at 45 degrees
to the one beside it. `AddDirections` is the one loop all four network sources now call, so a
fifth reference cannot quietly be given two of the four.

The two named angles keep their words - "parallel to the taxiway", "square to the taxiway",
"aligned with stand 3" - and only the diagonals carry a number. `AngledFrom`'s labels changed
with them, to "45 degrees FROM THE END OF the taxiway": the two relations are different fit
kinds, so both can hold at once, and until this they would have drawn two lines to two places
under identical words.

The ENUM was not renamed with it - 34 sites across 11 files, plus `FParallelGuideSource` and
eighteen `bParallel`, and a good number of those are comments that reason about Parallel BY
NAME. A name only developers read did not justify flattening that prose; `ERelation::Parallel`
carries the tie between the two names in its own comment instead.

**ON BY DEFAULT: Extending, Level with, Direction, Collinear, Taxiway, Service road, World.**
Collinear joined them on 2026-09-20 (see section 12): every ANGULAR row sits out on a free
start, so with Collinear off the first click of every gesture was unguided until the player
found a button nothing told them about - which is what one did.

**AND `Angled from x World` STAYS A HOLE**, which is the other half of that same report. A
world axis has no end to radiate from, so such a line would have to pass through the drag's
OWN origin - which is the identical line `Direction x World` already offers. Two buttons for
one guide is what the two axes were split apart to stop.

**`ThisGesture` is a column with no button, permanently on.** Extending is the ONLY cell in its
row, so an Extending button and a This-drawing button would switch off exactly the same
behaviour - two controls for one thing, which is the confusion this document exists to remove.
The residue it cannot express - "stop guiding me off my own shape, but keep Collinear against
the field" - is already served better by the Alt hold, which suspends every source for the
duration of one drag. A toggle is for "I never want this", and nobody never wants guidance off
the shape they are drawing.

**Every action needs an icon.** `AirportMgr.UI.EveryActionResolvesAnIcon` exempts only the Time
section. All ten ids change, so `UUIStyle::IconFor`'s mappings are rewritten rather than
extended; two net new glyphs are needed. Content work, and the suite is red until it is done.

## 8. The tools that have no guides

Guides exist only where a tool implements `DescribeGuideAnchor`; the base returns false. Three
registrations answered when this was written - Taxiway (1), Road (9), Fuel depot (0). **Five do
now**: the runway and apron tools were built on 2026-09-20, because an Apron column is worth
little if the apron tool itself is unguided.

- **`FRunwayTool`** - origin is the first threshold; NO reference, because a runway has no
  incoming edge. Every network column and World fire; Extending correctly proposes nothing. The
  drag point is a centreline.
- **`FApronDrawTool`** - `FOutlineDrawTool::GetCorners()` already holds everything needed:
  origin is the last placed corner, reference is the previous edge, `AlignTo` is the corners
  placed so far, tagged `ThisGesture`. Identical in shape to `FPlotPlaceTool`'s anchor. The drag
  point is a **boundary**.

- **`FStandPlaceTool`** - guided on its FREE START only, and it names its LastHeading as the
  anchor's reference. Without a reference `FPointAlignGuideSource` declines outright, so the
  "level with a row of stands" the Stand column exists for would never have been proposed. Once
  aiming it is not idle, and there is no position left to constrain.

Still unguided, and named so the absence stays deliberate: Select, Guidelines and Holding
point. The first is not a placement; the other two click EXISTING nodes, where the snap chain
has already decided. Fuel depot has an anchor but no free start: its first click must land on a
service road, so a second rule about where that anchor may go would be a second opinion.

**The apron tool's anchor lives on `FOutlineDrawTool`**, not on `FApronDrawTool`: what makes an
anchor there is the OUTLINE gesture, which is the base class's whole job, and it names "this
edge" rather than "the apron" because the base does not know what its outline will become.

## 9. Tests

Twenty-six automation tests reference `ESource`, `FSnapGuideSettings`, `IGuideSource` or
`FGuideAnchor` across seven files. Most change only in how they name a source. These are the
ones that must be WRITTEN, each failing before its fix:

1. **`Airside.Tool.GuideGridHasNoCellOutsideTheList`** - the one that would have caught this.
   Build a network holding a taxiway, a runway, an apron and a stand; walk all thirty
   `(ERelation, EReference)` pairs with everything switched on; assert every candidate the chain
   produces carries a pair for which `IsLegalCell` is true. A source proposing into a hole fails
   here rather than in PIE.
2. **`Airside.Tool.RunwayColumnOffSilencesEveryRelation`** - the report, as a test. Parallel,
   Collinear and MatchingGap all on, Runway off, a runway the nearest thing to the drag: assert
   no candidate carries `EReference::Runway`, and - the control leg, per this suite's habit -
   that switching Runway on brings one back.
3. **`Airside.Tool.CollinearAgainstAnApronIsFlushByHalfWidth`** - a road whose profile has
   asymmetric half-widths, drawn against an apron edge; assert two candidates, displaced by
   `GetHalfWidthLeft()` and `GetHalfWidthRight()` respectively, and NOT by one value twice.
4. **`Airside.Tool.RunwayGuideOffersItsOwnLine`** - the extended centreline survives the
   partition: from far outside `SearchRadiusUu`, a perpendicular candidate on the runway's line.
5. **`Airside.Tool.OffsetAndParallelNameOneRoad`** - both sources pick the same nearest segment
   once runways are excluded from both. This pins the composition their headers promise.
6. **`AirportMgr.Actions.GuideGridIsInTheRegistry`** - replaces
   `SnapTogglesAreInTheRegistry`. Walks BOTH enums against the two bar sections, by name.
7. **Anchor tests for the two new tools**, at the level of the composition: spawn, select the
   tool, place the first click, assert `DescribeGuideAnchor` returns true with the expected
   origin and drag-point kind.

Per CLAUDE.md's refactor contract: `UE_LOG` count and comment-line count in the touched files
must not fall, and every `UFUNCTION` and interface virtual stays reachable at its old name.
`ARoadBuildController::IsGuideSourceOn` and `ToggleGuideSource` are called from
`BuildActions.cpp`'s lambdas only, so they may change signature - but they are the reachability
check to run, not to assume.

## 10. Build sequencing

`FSnapGuideSettings` gains and loses UPROPERTYs and `EActionSection` gains a value, so this is
**not** a Live Coding change - it needs a full `Build.bat` with the editor closed, or a worktree
build with `-NoHotReloadFromIDE`. Batch it: one close, not several.

Order, each step leaving the suite green:

1. The two enums, `IsLegalCell`, and `FCandidate` carrying the pair. Arbiter tiebreak follows.
   Sources keep their current behaviour, mapped onto pairs. **No behaviour change yet** - this
   step is a rename with a wider key, and the existing 26 tests are the proof.
2. `FSnapGuideSettings` to two flag sets and the AND. Test 2 goes green here; this is the fix.
3. The partition: `IsRunwaySegment` skips in Parallel, Collinear and Offset; the perpendicular
   candidate into `FRunwayGuideSource`. Tests 4 and 5.
4. Widths on the anchor, and `DescribeGuideAnchor` gaining the target. Test 3, against a road
   only - no apron source yet.
5. `FApronGuideSource`, and the Apron column. Test 1 becomes meaningful here.
6. The two tool anchors, runway then apron. Test 7.
7. The bar's second section, the ids and the icons. Test 6.

Steps 1 and 2 together are the reported bug. Everything after is the functionality that made it
worth restructuring rather than patching.

## 11. Unresolved

- **Icons for two new buttons** (Apron, Matching gap - the rest re-map existing glyphs). Content,
  not code, but the suite is red until they exist.
- **`SearchRadiusUu` against a bigger candidate set.** Collinear now spans five columns and the
  apron source proposes per edge; the reserve of 16 in `FSnapGuideChain::Resolve` is no longer a
  fair guess. No PIE pass has ever been done on this number - see the 2026-09-17 design section
  11, which says the same of `MaxPullUu`.
- **Level with x Apron corner** is listed as a legal cell but has the weakest case on the grid;
  if it proves noisy in PIE it is the first cell to cut, and cutting it is a one-line edit to
  `IsLegalCell` plus its row in this table.

## 12. The free start

**Added 2026-09-20 from PIE**, after sections 1 to 11 were written and built:
*"I think a lot of the tools would benefit from snapping to guides before the first place of
the road. It doesnt make sense for all of them, but some it does."*

**One virtual, not per-tool boilerplate.** `IBuildTool::WantsFreeStartGuides()` answers false by
default; the base `DescribeGuideAnchor` returns an anchor with `bFreeStart` set when it is true
and the tool is idle, and `FBuildSession::MakeContext` fills that anchor's `Origin` from the
plane hit. An override that declines must DELEGATE to the base rather than `return false` -
that is the one line each opted-in tool pays, and `Airside.Tool.BuildSession` names the five so
an override that forgot is a failure rather than a silence.

**It costs nothing to keep it honest, because the arbiter already does.** With the origin ON
the cursor, `SnapGuide::Arbitrate` can measure no direction from one to the other, so every
`EFit::Angular` candidate sits out of its own accord. A free start therefore offers exactly the
POSITIONAL rows - and `Level with` only where the tool also names a `Reference` direction for
those lines to run along, which is why `FStandPlaceTool` names its heading.

| Tool | | Why |
|---|---|---|
| Taxiway (1), Road (9) | yes | start a road in line with an existing one, or a matching gap from a pair |
| Runway (6) | yes | place a threshold in line with another runway, or a standard separation off it |
| Apron (2) | yes | start an outline flush with a road edge, or level with a corner |
| Stand (3) | yes | positioning a stand level with a row of stands is what the Stand column is for |
| Fuel depot (0) | no | its first click MUST land on a service road - already snap-constrained, and a second rule about where its anchor may go would be a second opinion |
| Guidelines (5), Holding point (8) | no | both click EXISTING nodes; the snap chain has already decided |
| Select (4) | no | not a placement |

**A TOOL MUST ALSO CONSUME IT.** Describing an anchor and stopping is what shipped earlier on
2026-09-20 and showed the player nothing: `FBuildSession` resolves the guide onto the context
and NOTHING draws it until a tool asks. Each opted-in tool's first click takes
`FToolContext::GuidedCursor()` and its preview emits the dashed line.
`Airside.Tool.FreeStartToolsDrawTheirGuide` and `Airside.Tool.FreeStartClickLandsOnTheGuide`
measure the drawing and the click, never the anchor, for exactly that reason.

**And `Collinear` is on by default because of it.** Section 7 states the default; the reason
lives here. With every angular row sitting out by construction, Collinear is the only row a
free start can offer anything from, so with it off the whole of this section was dead on a new
airport. `Airside.Tool.FreeStartWorksOnTheShippedDefaults` takes the shipped settings
deliberately - the one test in its file that does.

### Still open

- **`Level with x Stand` needs a tool to feed it.** `FStandPlaceTool` now supplies every live
  entity as a `FGuidePoint` tagged `Stand`, UNBOUNDED by `SearchRadiusUu` because this function
  is handed no cursor to measure a reach from. There are tens of entities on a field and not
  thousands, so nothing breaks; it is the second number after the candidate reserve that will
  want a PIE pass.
- **`Collinear x Stand` is a declared cell with no source.** It was one before this work too.
