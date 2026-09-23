# Drawn stands: the size you drag decides what can park

> Builds the mechanic `IcaoCode::LetterForStandSize` was written for and nothing called:
> 2026-09-17-stand-layout-solver-design.md, "Admission runs backwards". Decisions below were
> taken with the user on 2026-09-23.

## What the player does

Key 3 is the stand tool. It is the depot's plot gesture pointed at a taxiway:

1. **Click 1** snaps to a taxiway / taxilane within 20 m (`PlotGesture::AnchorReachUu`) - never a
   service road. This is the **entrance edge**: the aircraft enters here nose first.
2. **Click 2** runs along the taxiway and sets the entrance width, in the depot's 5 m steps.
3. **Click 3** sets the depth, inward, away from the taxiway. The stand is locked.
4. **Build** (bottom bar) commits. Cancel steps back one stage, as on the depot.

**A stand is a RECTANGLE, three clicks, not the depot's free quad.** The template it carries is a
rectangle, and a skewed quad would need its letter measured from an inscribed rectangle the player
cannot see.

**REVISED 2026-09-23 (task 9):** the stand tool does not offer free-start snap guides - it
behaves AS the depot's `FPlotPlaceTool` already does, for the same reason. Neither overrides
`IBuildTool::WantsFreeStartGuides`, so both stay the base `false`
(`BuildSessionTest.cpp`'s own `FreeStarts` list names four tools - Taxiway, Apron, Runway,
Road - and the depot was never a fifth). The first click is snapped to the taxiway's own step
grid exactly as the depot's is to a service road's, and a guide drawn over a click that then
snaps elsewhere is a guide not obeyed.

**Orientation.** The aircraft taxis in forwards and reverses (pushback) out onto the taxiway. So
its TAIL is to the entrance edge and its NOSE points inward, away from the taxiway. Say "entrance
edge", never "frontage", when talking about stands: frontage read as "where the nose is".

## The letter

`IcaoCode::LetterForStandSize(Width, Depth)`: the largest letter whose width AND depth both fit
(the 09-17 table). Width = entrance edge length; depth = inward extent.

- The ghost shows the live letter, the next letter's threshold ("Code C - 14 m wider for D") and
  the wing keep-out of the letter's template.
- **No letter is a refusal, not an error.** Build is greyed and the readout names the lever:
  "needs 8 m more depth". Overlap with a road or another stand is refused the same way.

## Model

A stand is an ordinary `FEntityInstance` with an `Outline` - the field depot plots use. No new
field.

- **Captured at commit, together:** the pose and `DesignWingspan`. **REVISED 2026-09-23 (task
  9):** `DesignWingspan = IcaoCode::DesignSpanForLetter(L)`, not "the letter's max wingspan" -
  `MaxWingspanForLetter(L)` is the EXCLUSIVE edge for every letter but F (already the next
  letter's), so capturing it verbatim would read back one letter wider than drawn;
  `DesignSpanForLetter` is the hair-under value that still reads back as `L` through
  `LetterForWingspan`. `DesignWingspan` stays the slot the Inspector ("Code X") and
  `FStandSummary` read, so neither changes.

  Named by their real test names rather than "a test asserts": `Airside.Present.StandPlot.
  PlacesCodeC` and `Airside.Present.StandPlot.PlacesOtherLetters` (D, E, F) each place a stand
  THROUGH `PlaceStandInPlot` (the drawn path) and assert `LetterForWingspan(Entity.
  DesignWingspan)` reads the same letter `StandBox::LetterOf(outline)` does, so the two
  captured facts cannot drift for any letter that can be built (C-F). This covers PLOTTED
  stands only - the migration tests (`Airside.Model.StandOutline.LegacyGetsCodeCBox`,
  `...PointPlacedStandGetsOutline`) assert the migrated/point-placed OUTLINE equals
  `StandBox::BoxAt(pose, C)`, not this letter invariant; no test pins `LetterForWingspan
  (DesignWingspan) == LetterForStandSize(outline)` on that path today. A stand migrated or
  point-placed through `UEntityDefinition::MakeStandTransient`'s A320 design aircraft DOES
  capture a known, non-zero `DesignWingspan`, but nothing asserts it reads back as C the way
  the drawn-path tests do; 0 stays legal there and means "unknown", the raw
  `URoadNetwork::PlaceEntity` overload test fixtures use directly.
- **Kind, not outline.** `IsPlotted()` has meant "is a depot" at several sites. It stops meaning
  that: `FEntityInstance::IsDepot()` (`PoseRole != Aircraft`) and `IsStand()` (`PoseRole ==
  Aircraft`) become the tests, and `UPlotPresenter`, `FindEntityAt`, `DeleteEntity` labels and the
  fence gate on them. A stand gets pad paving; no fence, no modules. A Check-Architecture rule
  forbids `IsPlotted()` outside `RoadEntity.h` and the pad paver, which is the SHAPE this removes.

### Where the template sits in the drawn rectangle

Template local space (`UEntityDefinition::BuildStandTemplate`): origin = nose gear stop mark, +X
toward the nose, stand box X in `[NoseFwd - Depth, NoseFwd]`, Y in `[-W/2, W/2]`, all at the
letter's FLOOR (`StandWidthForLetter`, `StandDepthForLetter`).

The back edge `X = NoseFwd - Depth` is the tail side and is laid ON the drawn entrance edge,
centred across the width. So:

    Heading  = Inward
    Position = EntranceMid + Inward * (StandDepthForLetter(L) - MaxNoseFwdForLetter(L))

One function owns both directions: `StandBox` in `Solve/` - `PoseFor(EntranceA, EntranceB,
Inward, Letter)` and `BoxAt(Pose, Letter)`. Commit uses the first; migration uses the second.

**Slack.** Extra width is plain apron either side of a centred floor-width template; extra depth
is plain apron beyond the nose. Turning extra width into longer straight service roads (09-17
"absorbed as STRAIGHT") is OUT of this slice.

### Definition per letter

**REVISED 2026-09-23 (task 9):** the resolver is `ARoadNetworkActor::ResolveStandDefinitionFor
(EIcaoCode)`, not `UAirsideSettings::ResolveStandDefinition` - it lives on the actor because the
transient per-letter definitions it builds are cached there, alongside the Code C
`StandDefinition` property `ResolveStandDefinitionFor` forwards to unchanged. Letter C returns
that property's content (`DA_Stand_CodeC` in a real level); the others are transient definitions
built once through `BuildStandTemplate(Letter)` and cached, with `DesignAircraft` = the largest
shipped `UAircraftType` of that letter, or null. Code D has no aircraft today: a D stand admits by
span but has no parked-aircraft envelope to draw. **Code A and B's own templates do not fit their
own floor** - task 1's finding, live in `WhyStandRefused`: `ResolveStandDefinitionFor` returns
null for them and the tool refuses with "Code A/B stands cannot be built yet" rather than placing
a stand narrower than its own template needs.

**REVISED 2026-09-23 (final review I5): the lead-in is sized by the stand's LETTER.**
`FAnchorLink` read both the aircraft lead-in's radius and its span limit off the definition's
design aircraft, so a drawn D/E/F stand (no design aircraft) got Code C's radius and no limit.
One rule now (`LeadInSizingFor`): the letter of the captured `DesignWingspan` - the letter
admission reads - gives `RadiusForLetter` and `MaxWingspanForLetter`. A stand with span 0
("unknown") keeps the design-aircraft reading. Per-letter design aircraft in content (plane8
for F) stay deferred.

### Commit

`IRoadEditTarget::PlaceStandInPlot(Outline)` -> `URoadEditFacade`, sibling of
`PlaceEntityInPlot`, inside `FRoadEditScope` (undo). Refuses (returns invalid, logs why) on a
non-simple outline or no letter. Then the existing `PlaceEntity` path with the pose above, the
letter's definition, and the outline.

### Migration

On load, an alive stand (`PoseRole == Aircraft`) with no outline gets `StandBox::BoxAt(pose,
C)`'s four corners, and its `DesignWingspan` if 0. **REVISED 2026-09-23 (task 9):** this also
runs at POINT PLACEMENT, not only on load - `URoadNetwork::PlaceEntity`'s point-placement path
gives a stand its Code C box right there, rather than waiting for the next `EnsureStandOutlines`
pass at a load nobody has done yet, so a legacy call site placing a stand by pose alone still
gets an outline immediately. After that "a stand has an outline" holds everywhere. Every stand in
a saved level today is Code C.

## Admission

`ArrivalPlanner::ChooseStand`: same single `FindToGoals` search. **REVISED 2026-09-23 (task 9):**
the drop rule is not the raw compare `DesignWingspan < Airframe.Wingspan` - among reachable,
un-held candidates it drops any stand `IcaoCode::StandAdmits(DesignWingspan, Airframe.Wingspan)`
refuses, which compares the two BY LETTER rather than as raw doubles (0 either side still means
"unknown, admits anything", as today - `StandAdmits`'s own contract). `StandRank` then picks the
lowest ADMITTING letter, not the raw span, then the shortest taxi. It still returns a bigger stand
when every smaller one is held. This is the ONE rule - `AirportOps`'s `UStandAllocator::Reserve`
calls the same `IcaoCode::StandAdmits`/`StandRank` pair rather than keeping its own comparison, so
a stand held for an accepted flight and one chosen at live dispatch cannot disagree about what
fits. `OfferGenerator` and `FlightBoard` call `ArrivalPlanner::Plan`, which calls this, so an
airline is not offered an A380 until an F stand exists - no extra site.

Log (`LogAirside`, on every choice):
`ChooseStand: span 79.8 m -> node N (Code F); 3 too small, 1 held`.

**REVISED 2026-09-23 (final review C1, I6).** One candidate filter: `FEntityInstance::
IsStandCandidate()` (alive, pose node, `IsStand()`) is what both `ChooseStand` and
`UStandAllocator::Reserve` ask before `StandAdmits` - the allocator had no kind check, and a
fuel depot's 0 span "admits anything", so it held depots for airliners.

**Every stand too small is its own refusal**, `EArrivalRefusal::NoStandBigEnough`, not
`NoRouteToStand`: when no taxi-in is found and no live stand on the field `StandAdmits` the
aircraft (reachable or not - a too-small stand's lead-in carries its letter's span limit, so
to a widebody it is not reachable at all), `Plan` says so. The player text names the letter
to build: "Arrival refused: this aircraft needs a Code F stand, and none on the field is big
enough. Draw a bigger stand." (`FArrivalPlan::AircraftWingspan` carries the span; the inbox
passes the flight's own; the toast, which has only the reason, gets the letter-free
sentence.) It is a permanent refusal for `UOfferGenerator`, so no A380 is offered until an F
stand exists.

## Paint

Built in `RoadSurfacePresenter::RebuildMarkings` beside the holding bars, same builder idiom
(`MarkingQuads::AddQuad`/`AddRect`, UV1 = 0 solid), through `FStandMarkingBuilder` (task 8):

- **Lead-in line**: entrance midpoint to the stop mark, along the heading, 15 cm wide.
- **Stop bar**: across the heading at the stop mark, 40 cm x 3 m.
- **Letter**: REVISED 2026-09-23 (task 8) - not the `UTextRenderComponent` this section
  originally specified. The letter is PAINT, seven-segment strokes of quads through the same
  builder: A-F are exactly the letters a calculator's seven-segment display draws (A b C d E
  F), 3 m tall with a 30 cm stroke, centred 4 m inside the entrance. Three reasons, in the
  order they mattered: (1) paint needs no component lifecycle, where a `UTextRenderComponent`
  is a transient subobject and this project has already been bitten by a transient subobject
  pointer resetting to the CDO on level duplication (see the memory note on that trap); (2)
  paint is headless-testable through the same buffers every other marking is measured
  through, rather than requiring a spawned actor and a font; (3) paint lies flat by
  construction - every vertex carries the Z it is given - where a `UTextRenderComponent`
  needs its pitch set correctly to avoid standing up out of the ground (see the TextRender
  glyph-frame memory this section used to cite as the risk). DesignWingspan = 0 (a raw-model
  fixture stand nobody measured) paints no letter, but still paints the lead-in and stop bar.
- **Glyph frame** (REVISED 2026-09-23, final review I3): "up" is the heading (read by a pilot
  taxiing in) and the reader's RIGHT is `PerpCCW(Facing)` - Unreal is left-handed, the frame
  `RunwayMarkingBuilder`'s `FRunwayFrame` already paints designations in. Task 8 used
  `-PerpCCW` (a right-handed derivation), which painted every letter mirrored: "d" read as "b".
  Pinned by `Airside.Build.StandMarking.GlyphReadsUnmirrored` (C's strokes sit left of centre,
  d's upright right).

## Out of this slice

Extra width as longer service roads; a Code D aircraft and art; aprons of several stands;
reshaping a placed stand (remove and redraw).

## Tests

- **StandBox (Solve):** `PoseFor` then `BoxAt` returns the drawn rectangle's floor box; back edge
  on the entrance edge; tail toward the taxiway.
- **Gesture (StandPlotTool):** snaps to a taxiway, never a service road; 5 m steps; letter changes
  exactly at each threshold; too small -> not committable with the reason; cancel steps back.
- **Commit:** pose, heading, `DesignWingspan` agree with the outline; undo removes it.
- **Admission:** King Air takes B over a nearer E; falls through to bigger when smaller held; a
  too-small stand is never chosen; unknown span still admitted.
- **Migration:** an outline-less stand gets the C box at its pose.
- **Kind gate:** a drawn stand gets no fence, no modules; depot tests stay green.
- **Composition:** spawn the actor, draw a stand through the tool, run an arrival, it parks.
- **Lint:** the `IsPlotted()` rule goes red if a new call site appears.
- **Paint (Build, task 8):** one lead-in and one stop bar per drawn stand; the letter's
  seven-segment shape paints the right segment count (Code C: a,d,e,f; Code E: a,d,e,f,g);
  the lead-in's far end sits on the stop mark; every marking quad faces up. Composition:
  placing a stand paints it with no other edit in between.
