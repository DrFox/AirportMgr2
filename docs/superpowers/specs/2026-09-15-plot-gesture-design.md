# The plot gesture: road-snapped, sized in bays, confirmed before it builds

2026-09-15. Second iteration on `2026-09-15-plot-built-buildings-design.md`, which this
revises rather than replaces. The pillar is unchanged; the way a plot is DRAWN is not.

Reference: `samples/manorend.png` — Manor Lords mid-gesture, at plot point 3 of 4.

## 1. What the probe found

The first iteration reused the apron's freeform closing-polygon gesture, because it was
already built and tested. That was the cheap path, and finding out whether cheap was good
enough was the probe's entire job.

It was not. PIE on 2026-09-15 (`samples/first.png`) showed the pillar works — pad, modules,
fence, gate gap, all correct — and that the gesture around it is a chore:

| Observed | Why it is a gesture problem, not a bug |
|---|---|
| Three modules huddled at one end of a large plot | Modules fill bays from one end; the rest is empty yard with nothing to say it is capacity |
| Nothing communicates the 8 m minimum depth | A freeform polygon cannot refuse an invalid shape until it is finished |
| Five clicks to draw a rectangle | A building is a rectangle; the polygon gesture belongs to the apron it came from |

Manor Lords solves all three with one change: the gesture is **constrained and staged**, and
a persistent readout says what you are about to get before you buy it.

## 2. Decisions taken

- **A rectangle snapped to a road, not a freeform polygon.** Buildings are rectangles.
  Freeform stays with aprons, where it came from and where it is right.
- **Depth is ROWS, not yard.** This overturns §2 of the previous spec ("one row of bays,
  depth 8 m; extra depth is yard, not a second row"). Manor Lords' depth buys *extension
  space* - yard at first, purchasable later - and that is the reading that makes the third
  click worth asking for. A depth that bought nothing would be a step the player learns to
  click through.
- **Width x depth = slots, row 1 filled.** Row 1 is what the plot is built with; every slot
  behind it is expansion space. Buying into them is the next slice, and the slots are drawn
  from this one so depth pays off as COMMUNICATION before it pays off mechanically.
- **The readout is a second sink, not state the HUD polls.** See §5.
- **Commit is a button, not the fourth click.** The fourth click locks; the HUD builds. That
  beat is where cost and warnings are read.

## 3. The gesture

Four stages, one object each - the pattern `IOutlineDrawState` already justifies: a stage
carrying a width holds different data from one carrying nothing, and on an enum that width
would be readable in a stage that never touched it.

| Stage | Cursor | Click |
|---|---|---|
| **Idle** | Snaps to the nearest service-road segment; candidate anchors at bay intervals along it, nearest highlighted | Anchors the plot and fixes which side of the road it is on |
| **Width** | Runs along the road either way from the anchor, quantised to whole bays | Locks the frontage width |
| **Depth** | Runs perpendicular, away from the road, quantised to whole rows | Locks the depth |
| **Confirm** | Nothing moves; the readout reports `Committable` | Nothing - the HUD's Build button commits |

**Cancel steps back ONE stage**, not to idle. Same reasoning the outline tool records:
binning the whole gesture for one misclick is a harsher answer than the mistake deserves.

### 3.1 The snap is reuse, not new code

`FRoadSnapResult` already carries `Kind`, `Position`, `Segment` and `SegmentT` - the
parameter along a segment's A->B chord - and `FRoadSnapSettings` is the single per-airport
tuning struct BOTH drivers build from through `ARoadNetworkActor::MakeTunables`. Anchor
candidates are `SegmentT` quantised to bay multiples. A second snapper would be a second
opinion about where the cursor is, and the two would differ between PIE and the editor mode
for exactly the reason `FRoadSnapSettings`' own comment records.

**Service roads only.** A depot's trucks are ground vehicles; snapping to a taxiway would
let the player build a depot that dispatches onto one.

### 3.2 Which side, and which way along

Both come from the cursor at the moment of the click, not from a rule:

- **Side** is the sign of the cursor's offset from the segment tangent at the anchor.
- **Width direction** is whichever way along the road the cursor then moves. Dragging back
  past the anchor runs the plot the other way rather than refusing.

## 4. Width x depth = slots

`PlotFit::BayWidthUu` (4 m) and `BayDepthUu` (8 m) are unchanged and remain the unit.

- **Width** is bays across the frontage. Minimum 1.
- **Depth** is rows back from it. Minimum 1.
- **Row 1 is filled** with the placement mix, up to its bay count. Rows behind it are empty
  slots.

**The placement mix stays `{Shed, Tank, Pump}`**, as `FPlotOutlineTarget` already defaults
it - one of each is the concept sheet's depot and the smallest depot that actually works.
A width of fewer than three bays truncates it, and the readout says so rather than the
player discovering it afterwards. Choosing the mix at placement is still out of scope; it
arrives with buying, where the UI for it belongs.

Only row 1 fronts the road. Interior modules are reached across the yard, which costs
nothing today because trucks already spawn at the gate rather than rolling out of a shed -
the fidelity debt the previous spec named and did not pay.

**"No room to grow" is depth == 1**, and is the direct analogue of Manor Lords' *Plots
without Extension Space*. A warning, never a refusal: a one-row depot works perfectly well
and the player may want exactly that.

## 5. The readout seam

`IToolReadoutSink`, passed into `BuildPreview` beside `IToolPreviewSink`:

```
struct AIRSIDE_API IToolReadoutSink
{
    virtual ~IToolReadoutSink() = default;

    /** A named number the player is deciding on: bays, rows, cost. */
    virtual void Fact(const FString& Label, const FString& Value) = 0;

    /** Something wrong with the current gesture that does not stop it. */
    virtual void Warning(const FString& Text) = 0;

    /** Whether committing now would succeed. Drives the Build button's enabled state. */
    virtual void Committable(bool bCan) = 0;
};
```

**A SECOND SINK AND NOT AN EXTENSION OF THE FIRST.** `IToolPreviewSink`'s whole documented
contract is to describe intent *in road plane coordinates naming a MEANING*; a bay count is
not road-plane geometry, and putting it there would blur the one boundary that keeps `Tool/`
free of presentation.

**AND NOT STATE THE HUD POLLS.** `BuildPreview` is `const` and re-runs every frame, so facts
emitted through it are structurally incapable of disagreeing with the geometry drawn beside
them. A `GetReadout()` the HUD pulled would be a second thing that must agree with the
preview, which is the failure this codebase names most often. `Committable` travels the same
way for the same reason: a Build button lit while committing would fail is that bug wearing
a hat.

**Strings, not numbers.** The sink carries display-ready values because the alternative is
an enum of fact KINDS that the HUD switches on - which puts the plugin back in the business
of knowing what the HUD can render. The plugin still names no widget.

### 5.1 Where it renders

The existing bottom bar in the game module - the horizontal sectioned Cities-Skylines bar,
not a vertical panel - gains a section that renders whatever facts the active tool emitted
this frame, and the Build button beside them. A tool that emits nothing renders nothing, so
every existing tool is unaffected without being touched.

**Manor Lords floats its readout near the cursor; this does not.** The bar already exists,
already has a home for per-tool state, and a second floating surface would be a second place
the player has to learn to look.

### 5.2 Committing

`IBuildTool` gains `virtual void OnCommit(const FToolContext&) {}`, defaulted to nothing so
no existing tool changes. Only the plot tool overrides it, and only the Confirm stage acts.

## 6. What it draws

Entirely in existing preview vocabulary - no new primitives:

| Thing | Call | Style |
|---|---|---|
| Plot boundary | `Polygon` | `Pending` |
| Bay and row divisions | `Line` | `Pending` |
| Anchor candidates along the road | `Marker` | `Snap` |
| Which way each filled bay faces | `CrossMark` | `Pending` |
| A gesture that cannot commit | `Label` | `Refused` |

**The facing marks are not decoration.** `+X` faces away from the road and the truck leaves
out of the back - a depot built facing the wrong way is invisible until something drives,
which is the most expensive class of mistake this project has.

## 7. What this deletes

`FAnchorLink::FindFrontageEdge` and `Airside.Build.FrontageEdgeFacesTheRoad`.

A road-snapped rectangle KNOWS its frontage: it is the edge on the segment that was snapped
to. So `PlaceEntityInPlot` takes the frontage edge as a parameter instead of searching for
it, and the search has no caller left. It is four days old and was written for this feature,
which is not a reason to keep a list nothing consumes - the rule that removed
`UEdMode::ToolCommandList` applies to new code as much as to old.

The plot also touches a road BY CONSTRUCTION, so the "no road within reach" refusal goes
with it. What remains is the snap simply finding no service road, which is a state the Idle
stage reports before a click is even possible.

## 8. Refusals and warnings

The gesture cannot produce most of the old failures, which is the point of constraining it.

| Case | Said |
|---|---|
| Cursor near no service road | Idle draws no anchors; `Label` says "move near a service road" |
| Depth of one row | Readout warning: "No room to grow" |
| Fewer bays than the mix needs | Readout fact says what you actually get, e.g. "Modules: 2 of 3" |
| Plot overlaps another installation | `Refused` style on the boundary, `Committable(false)` |

Overlap is newly REACHABLE and newly checkable: a rectangle can be dragged over a neighbour,
and unlike a freeform outline it can be tested before the gesture ends.

## 9. Tests

- **Solve.PlotGridSlots** - width x depth gives the slot count; row 1 is the filled row.
  World-free.
- **Tool.PlotAnchorsSnapToBays** - anchor candidates land on bay multiples of `SegmentT`, and
  none appears against a taxiway.
- **Tool.PlotWidthRunsBothWays** - dragging back past the anchor runs the plot the other way
  rather than refusing or inverting.
- **Tool.PlotCommitsOnlyFromConfirm** - `OnCommit` in Idle, Width or Depth does nothing; the
  fourth click alone does not build.
- **Tool.PlotReadoutMatchesPreview** - for one gesture state, the bays the readout reports
  equal the bay divisions the preview drew. Pins §5's claim that the two cannot drift.
- **Tool.PlotRefusesOverlap** - a rectangle dragged over a placed depot reports
  `Committable(false)`.
- Composition, per CLAUDE.md: drive the four stages against a spawned actor and assert one
  entity with the expected outline, not just that the states advanced.

Test names are distinct leaves - a bare-named test vanishes from the automation tree once a
dotted child exists, and only the run count catches it.

## 10. Out of scope

- **Buying modules into empty slots.** The next slice, and the reason depth exists. This one
  draws the slots and stops.
- Cost and upkeep per module. The readout shows a placement cost from
  `UEntityDefinition::PlacementCost`; per-module pricing arrives with buying.
- Trucks rolling out of their own shed. Still the named debt.
- The freeform tool. `FOutlineDrawTool` stays exactly as it is, serving the apron.

## 11. Open questions

1. Should the anchor grid be shared between neighbouring plots, so two depots on the same
   road butt up without a sliver between them? Manor Lords appears to do this. This spec
   quantises per segment, which gets it for free along one segment and not across a junction.
2. Does a plot drawn over a junction refuse, or clip to the segment it anchored on? This spec
   assumes the width simply stops at the segment's end.
