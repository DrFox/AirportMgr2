# Snap guides: say what you are lining up with, and draw it

Angle and length guidance for the build tools, in the shape Cities Skylines uses: candidate
alignments offered as you drag, the winner drawn as a dashed line to the thing it belongs to,
and per-source switches so the player decides which guidance they want.

## 1. Why

Nothing constrains a free cursor today. `FRoadSnapChain` answers a different question - "what
did the cursor hit: a node, a segment, or nothing" - and the road tools have no angular
constraint at all. The gap became sharp on 2026-09-17, when the plot gesture's back corners
were freed to move in the plane: the player can now draw any quadrilateral and has nothing
whatsoever helping them draw a square one.

The request, verbatim:

> *It would be really good to introduce some guidance to angles and lengths via snapping,
> Cities Skylines does this on roads and it is really helpful... Maybe we use some UI so the
> user can choose what they want to snap to - or turn it off... a dashed line to the item you
> are snapping to, aligning with etc.*

## 2. Decisions taken

- **Two families, composed, not competing.** DIRECTION guides constrain which way; DISTANCE
  guides constrain how far. A drag resolves its direction first, then its distance along that
  direction. They are separate lists with separate arbitration, because a rule that picked one
  winner across both would have "parallel to that taxiway" losing to "30 m from the last one".
- **THE DRIVER RESOLVES THE GUIDE, not the tool.** It arrives on `FToolContext` beside `Snap`,
  from the same place and for the same recorded reason: both drivers resolve the snap before a
  tool sees it, so the same gesture cannot behave differently in PIE and in the editor mode.
  It is also the only place hysteresis can live - `BuildPreview` and `BuildReadout` are both
  `const` and neither can remember last frame's winner (§5).
- **A chain of sources, following `FRoadSnapChain`.** `IGuideSource` proposes candidates;
  the chain arbitrates. In `Tool/`, taking `const URoadNetwork&`, because four of the seven
  sources must query the network and a pure `Solve/` design would mean every caller
  pre-gathering candidates for it. The arbitration MATHS lives in `Solve/` and is tested with
  no world.
- **Each source is a `FBuildAction`.** Toggles get a bar button, an optional key and lit-when-
  active state from the one list that already generates all three, so the UI cannot advertise
  a source that is off, or hide one that is on.
- **A held key suspends everything.** Toggles are for "I never want this"; the held key is for
  "not for this one drag", and without it the player fights the guide for a position it will
  not give them.
- **The winner is drawn as a DASHED LINE TO ITS REFERENCE**, labelled. See §6.

## 3. The sources

Eight, in priority order - most specific to what the player is doing, first. Every one is
individually switchable.

| # | Source | Proposes | Needs the network |
|---|---|---|---|
| 1 | Extending | the incoming segment's direction, and its perpendicular; for a plot corner, the frontage's | no - the tool supplies the reference |
| 2 | PointAlign | lines THROUGH a point the tool names, along the reference and its perpendicular | no - the tool supplies the points |
| 3 | Aligned | a placed entity's pose direction and perpendicular | yes |
| 4 | Collinear | the line an existing segment already lies on | yes |
| 5 | Parallel | the nearest road's direction, and its perpendicular | yes |
| 6 | Runway | every runway's heading, and its perpendicular | yes |
| 7 | World | 0/45/90/135 degrees | no |
| 8 | Offset | **distance**: the gap a neighbouring parallel road already keeps | yes |

**PointAlign was added on 2026-09-17**, after stage 1 shipped, on the request: *"can we have
multiple alignments, ie Square to frontage, and 0 degrees to point 3 so the player knows their
position is aligned to another point"*. It ranks second because a point in the gesture the
player is drawing RIGHT NOW is as specific as the edge they are extending, and both are more
specific than anything the network offers. Its points come from the tool, so like Extending it
needs no network; stage 2's `Aligned` can feed the same source network points without changing
it.

**It is the first source whose line does NOT pass through the drag's own origin.** Every other
source answers "which way from here"; this one answers "you are level with THAT". That is the
change §4 and §5 below had to absorb.

**The order is the flicker rule's tiebreak and nothing else** - a lower source still wins if
it is the only one in tolerance. The reasoning: what you are extending is what you are
thinking about; the world grid is what you fall back on when nothing else applies. Runways
rank above world axes and below local features because an airport squares to its runways, but
not in preference to the taxiway you are actually working on.

**`Offset` is the only distance source in this version.** The 5 m frontage quantum stays where
it is, in `PlotGesture::QuantisedFrontage` - it is a rule about what a plot may BE, not
guidance about what the player is near, and moving it here would make one mechanism
responsible for both.

## 4. What a source returns

**The types live in `Solve/GuideArbiter.h`**, not in `Tool/` with the chain that fills them.
`FCandidate` is `FVector2D`, `double` and `FString` - all Core - so the arbiter and everything
it reasons about stay inside `Solve/`'s CoreMinimal-only rule and stay world-free testable.
`Tool/SnapGuideChain.h` owns `IGuideSource` and the chain, because those need the network.

```cpp
namespace SnapGuide
{
    enum class ESource : uint8
    {
        Extending, PointAlign, Aligned, Collinear, Parallel, Runway, World, Offset
    };

    /** How a candidate is judged near - see §5. */
    enum class EFit : uint8
    {
        /** The cursor's DIRECTION from the origin is within ToleranceDegrees. */
        Angular,

        /** The cursor is within ToleranceUu of the line, however it got there. */
        Perpendicular
    };

    /** One thing the cursor could line up with. */
    struct FCandidate
    {
        /** Unit, and for a direction guide this is the whole answer. */
        FVector2D Direction = FVector2D(1.0, 0.0);

        /**
         * The point the candidate's LINE passes through.
         *
         * Every source but PointAlign fills this with the drag's own origin, which is what
         * made it implicit before. An alignment to another point is a line through THAT
         * point and the origin is nowhere on it, so the line has to carry its own.
         */
        FVector2D Through = FVector2D::ZeroVector;

        EFit Fit = EFit::Angular;

        /** For Offset: how far along the perpendicular, uu. Zero for direction guides. */
        double Distance = 0.0;

        /**
         * The point the dashed line is drawn TO - the road it is parallel with, the stand it
         * squares to. NOT the guide's own geometry: the player needs to see WHICH thing they
         * are lining up with, which is the whole of Cities Skylines' advantage here.
         */
        FVector2D ReferenceAt = FVector2D::ZeroVector;

        /** "parallel to taxiway", "runway 09", "45 degrees". Shown beside the line. */
        FString Description;

        ESource Source = ESource::World;
    };

    struct FResult
    {
        bool bActive = false;

        /**
         * Every guide holding this frame - AT MOST ONE PER FIT KIND, so at most two.
         *
         * One list, not a Winner plus an also-ran: once the point is their intersection
         * neither is privileged, and two fields would be two things to keep in step.
         */
        TArray<FCandidate, TInlineAllocator<2>> Winners;

        /** Where the constrained point ended up, which is what the tool uses. */
        FVector2D Point = FVector2D::ZeroVector;
    };
}
```

**`Description` is a string, exactly as `IToolReadoutSink`'s facts are, and for the same
reason**: the alternative is an enum of description KINDS that the overlay switches on, which
puts presentation back inside the plugin's decision-making.

## 5. Arbitration, and the flicker rule

Seven sources means several candidates within a degree of each other. A guide that flips
between "parallel to that taxiway" and "square to runway 09" every frame is worse than no
guide at all, so this is specified rather than left to "nearest wins".

1. **Eligibility.** An `Angular` candidate is eligible when the cursor's own direction from the
   origin is within `ToleranceDegrees` of it. Default 7 degrees. A `Perpendicular` candidate is
   eligible when the cursor is within `ToleranceUu` of its line. Default 300 uu.
2. **Ranking, WITHIN A FIT KIND.** Among eligible candidates of one kind, the smallest error
   wins; ties break on the source order in §3. Errors are NOT compared across kinds - degrees
   and uu are not the same quantity, and pretending otherwise would put the exchange rate
   between them in the middle of the flicker rule.
3. **One winner per fit kind, so at most two.** This is what "multiple alignments" means: the
   angular guide says which way the corner went, the perpendicular one says what it ended up
   level with, and they are answering different questions about the same point.
4. **Hysteresis, per kind.** Each kind's previous winner is kept unless a challenger of that
   kind beats it by `StickinessDegrees` (2) or `StickinessUu` (100). A challenger better by
   EXACTLY the stickiness does NOT take it: the tie goes to not changing, which is what
   stickiness is for, and leaving it to the last bit of an `acos` is how the first
   implementation failed its own test.
5. **Suspension.** While the suspend key is held, nothing is eligible and `bActive` is false.

### Where the point lands

- **One winner:** its perpendicular projection of the cursor onto that line, as before.
- **Two winners:** THEIR INTERSECTION. Both labels are then true at once, which is the whole
  reason two are drawn - a corner shown as "0 degrees to corner 3" while sitting 40 uu off
  being level with corner 3 is a mark whose meaning has gone, and this codebase deletes those.
- **The pull guard.** Two nearly parallel lines meet a kilometre away. If the intersection is
  further than `MaxPullUu` (1000) from the cursor, or the lines are parallel at all, the
  PERPENDICULAR winner is dropped and the angular one alone constrains. Angular survives
  because it describes the direction the player is actively dragging; the alignment is
  opportunistic, and an opportunity is the right thing to give up.

`Solve/GuideArbiter.h` holds 1-3 as a pure function over a candidate list plus the previous
winner, so every case above is a world-free test. The chain in `Tool/` gathers candidates and
calls it.

**Hysteresis is measured against the previous WINNER, not the previous cursor.** A player who
drags slowly past two near-equal candidates should feel one guide hold and then hand over,
not a rapid alternation that a cursor-delta rule would still produce.

## 6. What it draws

A new `EPreviewStyle::Guide`: **dashed, and distinct from `Provisional`**. That value already
means "this edge is still moving"; a guide line means "this is what you are lined up with".
Two meanings, two styles - the overlay may well dash both, and that is its business, not the
plugin's.

The tool emits, when `Context.Guide.bActive`, ONE LINE AND ONE LABEL PER WINNER:

- `Sink.Line(Point, Winner.ReferenceAt, EPreviewStyle::Guide)` - the dashed line to the thing.
- `Sink.Label(<that line's midpoint>, Winner.Description, EPreviewStyle::Guide)` - "parallel to
  taxiway", "0 degrees to corner 3".

**The label sits at its own line's MIDPOINT, not at the corner.** With one guide the corner was
the obvious place - it is where the eye is. With two, both labels land on the same point and
overprint, and the plugin has no camera to offset them by a readable number of pixels. The
midpoint needs no measurement, and it puts each label on the line it describes.

**The line goes to the REFERENCE, not along the guide direction.** A ray shot off into the
distance says "you are at 45 degrees"; a line to the taxiway you are parallel with says WHAT
you are lining up with, which is the thing Cities Skylines gets right and the reason the
request named it.

## 7. The toggles

One `FBuildAction` per source, in a new `EActionSection::Snap`:

```
SNAP
[Extending] [Aligned] [Collinear] [Parallel] [Runway] [World] [Offset]
```

`IsActive` returns whether that source is enabled; `Execute` toggles it. Lit means on.

**A NEW SECTION COSTS FOUR LISTS AGREEING** - `EActionSection`, `SectionNames`,
`SectionSpecs`, and the bar's own `BindWidgetOptional` UPROPERTY - and two of those are
guarded by `static_assert` against `EActionSection::Count`. The other two fail at runtime by
drawing nothing. That is the documented cost of a section and it is worth paying here: seven
toggles dropped into `Edit` would swamp the five verbs already there.

**Enabled state lives on `ARoadNetworkActor`**, beside `Snap`, as a per-airport `UPROPERTY` -
the same argument `FRoadSnapSettings` records for being per-airport rather than per-driver, so
the editor mode and PIE agree about what is switched on.

Defaults on: Extending, Parallel, World. Off: Aligned, Collinear, Runway, Offset. **A player
meeting seven live guides at once learns nothing**; the three that fire most often teach the
mechanism, and the rest are found when wanted.

**The suspend key is `Alt`, held.** Read in `MakeToolContext` beside the existing Ctrl and
Shift handling, not as a registry action - the registry binds presses, and this is a hold.

## 8. Sequencing

**One implementation plan per stage, not one for the document.** Five stages is more than a
plan should carry, and each stage below leaves the branch usable on its own - which is the
test for whether a split is real rather than administrative. The first already helps square a
plot.

1. The chain, the arbiter, `FToolContext::Guide`, the `Guide` style and its drawing, with
   **World** and **Extending** only, consumed by the **plot corners**.
2. ~~The network sources: Parallel, Collinear, Aligned, Runway.~~ **Done 2026-09-17.**
   Collinear is the only one of the four that is `EFit::Perpendicular` - "in line with that
   taxiway" is about where the cursor ended up, not which way it set off. Runway is
   deliberately exempt from `SearchRadiusUu` (10000 uu); the other three are bounded by it,
   because without a reach the nearest-wins race is decided by geometry off screen.
   `RoadNaming::Describe` gave the runway/service-road/taxiway classification one home; it
   had been living in `IsRunwaySegment` and in the plot tool's own `IsServiceRoad`.
3. The toggles, the `Snap` section, and the Alt suspend.
4. **Offset**, the distance family, which needs its own arbitration pass.
5. **Road drawing** as the second consumer.

## 9. Tests

World-free in `Solve/`:

- **`Airside.Solve.GuideArbiterPicksTheNearest`** - among eligible candidates the smallest
  angular error wins.
- **`Airside.Solve.GuideArbiterBreaksTiesBySource`** - two candidates at equal error resolve
  to the higher-priority source, every time, in both list orders. A tiebreak that depended on
  the order candidates happened to be gathered in would be a guide that changed with the
  network's iteration order.
- **`Airside.Solve.GuideArbiterHoldsItsWinner`** - a challenger better by less than
  `StickinessDegrees` does NOT take over; one better by more does. This is the flicker rule
  and it is the reason the arbiter is a separate unit.
- **`Airside.Solve.GuideArbiterRefusesOutsideTolerance`** - nothing eligible means `bActive`
  false, not a nearest-anyway answer.

With a world, in `Tool/`:

- **`Airside.Tool.GuideChainProposesFromTheNetwork`** - a laid taxiway produces a Parallel
  candidate with its direction and a `ReferenceAt` on that taxiway.
- **`Airside.Tool.GuideSuspendsOnHold`** - with the suspend flag set, an otherwise-winning
  candidate yields nothing.
- **`Airside.Tool.PlotCornerFollowsTheGuide`** - composition level: a corner dragged near 90
  degrees off the frontage lands exactly on it, and the preview draws a `Guide` line to the
  reference. Every `Solve/` test above passes if the tool never calls the chain.

In `AirportMgr`:

- **`AirportMgr.Actions.SnapTogglesAreInTheRegistry`** - one action per `ESource`, walked from
  the enum rather than listed, so a source added without a toggle fails here.

## 10. Out of scope

- Snapping the ROAD tools. Stage 5; the mechanism is the same and the reference differs.
- Curved roads. Every segment the tools author is straight.
- Snapping to anything not in the road network or the entity list - terrain contours, the
  landscape grid.
- Numeric entry ("make this exactly 40 m"). A different feature that happens to share a goal.

## 11. Open questions

1. `ToleranceDegrees` at 7, `StickinessDegrees` at 2, and (added 2026-09-17) `ToleranceUu` at
   300, `StickinessUu` at 100 and `MaxPullUu` at 1000 are placement feel, judged in PIE. They
   start as constants and become `UAirsideSettings` knobs if they need tuning, the same route
   `ClearanceUu` and `GateCorridorUu` took. The uu three have had NO PIE pass at all yet.
2. Should a guide that is active also snap the LENGTH to the 5 m step, or stay purely
   angular until stage 4? This spec says angular - mixing them before the distance family
   exists would put length rules in two places.
3. Does the dashed guide line need to survive the click, briefly, so the player sees what they
   got? Cities Skylines does not do this and the gesture already has a review beat at Confirm.
   Assumed no.
