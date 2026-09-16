# The four-point plot gesture: pin a corner at a time, and show only what is decided

Supersedes the gesture half of `2026-09-15-plot-gesture-design.md` (§2's "a rectangle snapped
to a road", §3's four stages, §5.1's "where it renders"). The YARD - how modules fill a plot -
is `2026-09-16-organic-module-placement-design.md` and is untouched by this document.

## 1. What the probe found

PIE on 2026-09-16, against `samples/manorend.png`:

> *The first click anchors the starting position and you see the white dot appear, then as you
> move the mouse the width of the plot is altered, you only see the solid white line along the
> road. The second click anchors the road facing width and you get the addition of the second
> white dot. Mouse moves now changes the depth of the 3rd corner and you see the dashed line,
> clicking it places a 3rd white dot. Now moving the mouse allows you to choose the depth of
> the 4th corner and now you have a polygon and the plot shows what can be built as you change
> the 4th position. Once clicked a build button appears over the plot asking for you to
> confirm the build. All the time there is the popover telling you what is happening.*

Measured against what we built:

| Manor Lords | Ours, before this |
|---|---|
| Four points, counted: `Plot Points: 3/4` | Three clicks, no progress shown |
| Point 1, then the frontage as a SOLID line and nothing else | The whole rectangle, from the first click |
| Point 2, then a DASHED boundary as corner 3 moves | - |
| Point 3, then contents appear as corner 4 moves | Contents from the first click |
| Point 4, then a Build button OVER THE PLOT | Build at the cursor from the Confirm stage |
| A popover, throughout | Facts on the bottom bar |

**Showing the contents early is the substantive error, not the noisy one.** At one and two
points the shape is not decided, so anything drawn inside it is a promise the next click
breaks. The rule Manor Lords follows is stricter and better: draw only what is PINNED, and
fill the plot only once the last corner is the only thing still moving.

## 2. Decisions taken

- **Four points: two on the road, two behind.** Points 1 and 2 set the frontage along the
  road; 3 and 4 are the back corners, set independently, so the plot is a quadrilateral and
  not a rectangle.
- **One plot, not a subdivision.** Manor Lords splits a dragged region into several burgage
  plots (`Burgage Plots: 2` in the screenshot); we do not. One drawn quad is one depot, with
  one gate and one yard. Recorded because the screenshot plainly shows the other behaviour
  and a later reader will wonder.
- **The frontage is quantised; the back corners are not.** Minimum 15 m, then 5 m steps. The
  frontage is the edge that must TILE with the plot next door, which is the whole reason a
  quantum exists; the back corners are shared with nothing and snapping them would only
  refuse shapes the ground calls for.
- **4 m stops being the plot's unit.** `PlotFit::BayWidthUu` is 4 m and currently means two
  things - the width of a shed AND the step of a plot. They separate: a shed is 4 m because a
  shed is 4 m, and a plot is 15 m minimum because a yard narrower than that is not a yard.
- **Solid means pinned, dashed means provisional** - two meanings on `EPreviewStyle`, not a
  `bDashed` flag. `IToolPreviewSink`'s contract is that a tool names a MEANING and the overlay
  decides the look; a flag would have the plugin specifying a dash pattern.
- **The readout moves to the plot.** See §5.

## 3. The gesture

| Points | The cursor decides | Drawn |
|---|---|---|
| 0 | which anchor on the road | candidate dots; the one a click takes, highlighted |
| 1 | frontage width, in 5 m steps | dot 1, and the frontage as a SOLID line. Nothing else |
| 2 | back corner A | dots 1-2, solid frontage, DASHED boundary through the moving corner |
| 3 | back corner B | dots 1-3, solid frontage, dashed boundary, AND the yard's contents |
| 4 | nothing; the shape is final | the whole plot, solid, contents, and Build |

Right click steps back one point at a time, as it already does - binning a whole gesture for
one misclick is a harsher answer than the mistake deserves.

**Contents appear at three points, not two.** With two pinned, both back corners are still
unknown and the plot has no settled depth anywhere. With three, only one corner moves, so the
modules drawn are what the player will get unless they move that one corner - which is a
promise the gesture can keep.

### 3.1 Why the fourth click does not build

Unchanged from the previous spec and restated because it is the thing most likely to be
"simplified" later: the last click LOCKS. The beat between locking and committing is where
cost and warnings are read. Build is a widget, so `OnCommit` stays reachable at any moment
and every stage but the last ignores it.

## 4. What the preview says

Two values join `EPreviewStyle`:

| Style | Means | Drawn as |
|---|---|---|
| `Pinned` | this edge is decided and will not move | a solid heavy line |
| `Provisional` | this edge follows the cursor | a dashed line of the same weight |

`ARoadBuildHUD` owns the dash: `Line` segments the span and draws alternate pieces. The
plugin never names a dash length, exactly as it never names a colour.

**The existing `Pending` keeps its meaning** - "this is what the click would do" - and is
still what the anchor highlight and the module footprints use. `Pinned` and `Provisional` say
something `Pending` cannot: whether the thing under them has stopped moving.

## 5. Where it renders

**A panel anchored to the plot**, not a section of the bottom bar. It carries:

```
  Plot Points: 3/4
  Frontage: 20 m     Modules: 3 of 3
  Room for: 4
  [ Build ]                    <- only at 4/4
```

**THE BAR'S READOUT SECTION IS REMOVED.** It was added this morning and the same session
proved it wrong twice: the Build button in the `edit` group could not be found at all, and
whether the fact strip was even legible could not be settled from a screenshot. The lesson
was written down at the time - a gesture's conclusion belongs where the player is looking -
and this applies it rather than leaving two places that say the same thing.

`ARoadBuildHUD::DrawCommitPrompt` becomes this panel: it already projects a road-plane point
to the screen and draws on a dark ground for legibility. The `edit.build` action and its
Enter key stay exactly as they are - the panel is a second way to reach the one action, and
`FBuildAction::TryRun` remains the one door.

## 6. What this costs elsewhere

- **`PlotFit::BuildGrid` is already dead.** Measured, not assumed: since the ghost stopped
  drawing bay marks it has no caller outside its own tests. `GridOutline`'s three callers are
  all in `FPlotPlaceTool` and all go when the tool carries four corners instead of a width and
  a depth. Both are deleted here, with the tests that only exercise them - a solver nothing
  calls is a thing a later reader has to disprove the importance of. `FitBays` STAYS: it is
  still `PlaceEntityInPlot`'s, for better and worse (§8).
- `FPlotPlaceTool::Width`/`Depth` in bays become four `FVector2D` corners. `ShownSize` and
  `ShownPlot` collapse into one `ShownQuad`.
- `Airside.Tool.PlotWidthRunsBothWays` and `PlotGhostAgreesWithTheBar` are about a rectangle's
  width and depth. They are rewritten against corners, not deleted: what they PIN - dragging
  back past the anchor, and the ghost agreeing with the bar - survives the shape change.

## 7. Refusals and warnings

- A self-crossing quad is refused at the click that would make it, not at commit.
  `RoadGeom::IsSimplePolygon` already answers this and `PlaceEntityInPlot` already calls it;
  the gesture asks earlier so the player is never holding an invalid shape.
- A frontage under 15 m cannot be reached: the quantum's floor IS 15 m, so there is nothing
  to refuse.
- A plot too shallow for any module keeps the existing behaviour - it builds, the yard drops
  what will not fit, and the readout says "Modules 0 of 3".

## 8. The second evaluator, named

`URoadEditFacade::PlaceEntityInPlot` still runs `PlotFit::FitBays` and truncates the module
list to the bay count. That was the grid's rule and it now competes with `PlotYard`, which
decides what fits by actually placing it - so a module can vanish for a reason the readout
never mentions. **Out of scope here and listed so it is not discovered by accident.** The fix
is to delete the truncation and let the yard report drops, which is a change to placement
rather than to the gesture.

## 9. Tests

- **`Airside.Tool.PlotPinsOneCornerAtATime`** - four clicks advance through four stages, and
  the stage count is what the readout reports as `N/4`.
- **`Airside.Tool.PlotDrawsOnlyWhatIsPinned`** - at one point the preview emits the frontage
  and NO closed boundary; at two, a boundary but NO module footprints; at three, footprints.
  Counted through a recording sink, per style. This is the test the whole document exists for.
- **`Airside.Tool.PlotFrontageSnapsInFiveMetreSteps`** - a frontage dragged to 17 m locks at
  15 or 20 and never at 17, and never below 15.
- **`Airside.Tool.PlotBackCornersAreFree`** - a back corner dragged to an arbitrary depth
  keeps it, so the two rules are demonstrably different rather than accidentally the same.
- **`Airside.Tool.PlotRefusesACrossedQuad`** - a fourth point that would cross an edge does
  not advance the stage.
- **`Airside.Tool.PlotGhostAgreesWithTheBar`** - rewritten against corners: the quad drawn is
  the quad the readout describes, on the SECOND gesture of a session, which is where the
  previous version caught a stale depth.
- **`AirportMgr.HUD.PlotPanelShowsProgressAndBuild`** - the panel's text names the point count
  and offers Build only at 4/4, tested through the same static text function
  `CommitPromptText` already uses, with no Canvas.

## 10. Out of scope

- Subdivision into several plots (§2).
- The chain-link fence kit. Independent, and the most visible remaining change.
- Buying modules into a standing plot.
- The shed's placement rule, which moves to "against an edge, turned to face into the plot"
  under the yard spec rather than this one.

## 11. Open questions

1. ~~Is the Build button in the panel clickable?~~ **Resolved 2026-09-16: no, Enter confirms.**
   The panel announces Build and names the key; Enter and the bar button remain the two live
   routes. A clickable prompt means a UMG panel positioned from a projected point, roughly ten
   times the work, and this first answers whether the panel is read at all. The panel's text
   comes from `ARoadBuildHUD::CommitPromptText`, which already reads the key off the registry,
   so a rebound Build cannot leave the panel advertising the wrong one.
2. Does the frontage quantum belong in `UAirsideSettings` rather than as a constant? It is
   the first number here a designer would want to change without a build.
