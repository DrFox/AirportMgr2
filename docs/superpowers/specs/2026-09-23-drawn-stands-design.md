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

- **Captured at commit, together:** the pose and `DesignWingspan` = the letter's max wingspan.
  `DesignWingspan` stays the slot the Inspector ("Code X") and `FStandSummary` read, so neither
  changes. A test asserts, over every stand, `LetterForWingspan(DesignWingspan) ==
  LetterForStandSize(outline)` so the two captured facts cannot drift.
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

`UAirsideSettings::ResolveStandDefinition(EIcaoCode)` - the one resolver. Letter C returns
`DA_Stand_CodeC` (the authored content); the others are transient definitions built once through
`BuildStandTemplate(Letter)` and cached, with `DesignAircraft` = the largest shipped
`UAircraftType` of that letter, or null. Code D has no aircraft today: a D stand admits by span
but has no parked-aircraft envelope to draw.

### Commit

`IRoadEditTarget::PlaceStandInPlot(Outline)` -> `URoadEditFacade`, sibling of
`PlaceEntityInPlot`, inside `FRoadEditScope` (undo). Refuses (returns invalid, logs why) on a
non-simple outline or no letter. Then the existing `PlaceEntity` path with the pose above, the
letter's definition, and the outline.

### Migration

On load, an alive stand (`PoseRole == Aircraft`) with no outline gets `StandBox::BoxAt(pose,
C)`'s four corners, and its `DesignWingspan` if 0. After that "a stand has an outline" holds
everywhere. Every stand in a saved level today is Code C.

## Admission

`ArrivalPlanner::ChooseStand`: same single `FindToGoals` search. Among reachable, un-held
candidates it drops `DesignWingspan < Airframe.Wingspan` (0 = unknown = admitted, as today) and
picks the lowest letter, then the shortest taxi. It still returns a bigger stand when every
smaller one is held. `OfferGenerator` and `FlightBoard` call `ArrivalPlanner::Plan`, which calls
this, so an airline is not offered an A380 until an F stand exists - no extra site.

Log (`LogAirside`, on every choice):
`ChooseStand: span 79.8 m -> node N (Code F); 3 too small, 1 held`.

## Paint

Built in `RoadSurfacePresenter::RebuildMarkings` beside the holding bars, same builder idiom
(`MarkingQuads::AddQuad`, UV1 = 0 solid):

- **Lead-in line**: entrance midpoint to the stop mark, along the heading.
- **Stop bar**: across the heading at the stop mark.
- **Letter**: a flat `UTextRenderComponent` near the entrance, owned by the surface presenter.
  (Local +X is the glyph's facing - see the TextRender memory; pitch it to lie flat.)

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
